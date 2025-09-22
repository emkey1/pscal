#include "core/utils.h"
#include "backend_ast/builtin.h"
#include "third_party/yyjson/yyjson.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define YYJSON_UNUSED_HANDLE (-1)

typedef enum {
    JSON_HANDLE_UNUSED = 0,
    JSON_HANDLE_DOC,
    JSON_HANDLE_VAL
} JsonHandleKind;

typedef struct {
    JsonHandleKind kind;
    yyjson_doc *doc;
    yyjson_val *val;
} JsonHandleEntry;

static JsonHandleEntry *jsonHandleTable = NULL;
static size_t jsonHandleCapacity = 0;
static pthread_mutex_t jsonHandleMutex = PTHREAD_MUTEX_INITIALIZER;

static void jsonResetEntry(JsonHandleEntry *entry) {
    if (!entry) return;
    entry->kind = JSON_HANDLE_UNUSED;
    entry->doc = NULL;
    entry->val = NULL;
}

static int jsonAllocHandle(JsonHandleKind kind, yyjson_doc *doc, yyjson_val *val) {
    if ((kind == JSON_HANDLE_DOC && !doc) || (kind == JSON_HANDLE_VAL && (!doc || !val))) {
        return YYJSON_UNUSED_HANDLE;
    }

    pthread_mutex_lock(&jsonHandleMutex);
    size_t slot = jsonHandleCapacity;
    for (size_t i = 0; i < jsonHandleCapacity; ++i) {
        if (jsonHandleTable[i].kind == JSON_HANDLE_UNUSED) {
            slot = i;
            break;
        }
    }

    if (slot == jsonHandleCapacity) {
        size_t new_capacity = jsonHandleCapacity ? jsonHandleCapacity * 2 : 16;
        JsonHandleEntry *new_table = realloc(jsonHandleTable, sizeof(JsonHandleEntry) * new_capacity);
        if (!new_table) {
            pthread_mutex_unlock(&jsonHandleMutex);
            return YYJSON_UNUSED_HANDLE;
        }
        for (size_t i = jsonHandleCapacity; i < new_capacity; ++i) {
            new_table[i].kind = JSON_HANDLE_UNUSED;
            new_table[i].doc = NULL;
            new_table[i].val = NULL;
        }
        jsonHandleTable = new_table;
        slot = jsonHandleCapacity;
        jsonHandleCapacity = new_capacity;
    }

    jsonHandleTable[slot].kind = kind;
    jsonHandleTable[slot].doc = doc;
    jsonHandleTable[slot].val = (kind == JSON_HANDLE_VAL) ? val : NULL;

    pthread_mutex_unlock(&jsonHandleMutex);
    return (int)slot;
}

static yyjson_doc *jsonLookupDoc(int handle) {
    if (handle < 0) return NULL;
    pthread_mutex_lock(&jsonHandleMutex);
    yyjson_doc *doc = NULL;
    size_t idx = (size_t)handle;
    if (idx < jsonHandleCapacity) {
        JsonHandleEntry entry = jsonHandleTable[idx];
        if (entry.kind == JSON_HANDLE_DOC) {
            doc = entry.doc;
        }
    }
    pthread_mutex_unlock(&jsonHandleMutex);
    return doc;
}

static bool jsonLookupValue(int handle, yyjson_doc **out_doc, yyjson_val **out_val) {
    if (handle < 0 || !out_doc || !out_val) return false;
    pthread_mutex_lock(&jsonHandleMutex);
    bool ok = false;
    size_t idx = (size_t)handle;
    if (idx < jsonHandleCapacity) {
        JsonHandleEntry entry = jsonHandleTable[idx];
        if (entry.kind == JSON_HANDLE_VAL && entry.doc && entry.val) {
            *out_doc = entry.doc;
            *out_val = entry.val;
            ok = true;
        }
    }
    pthread_mutex_unlock(&jsonHandleMutex);
    return ok;
}

static bool jsonReleaseValueHandle(int handle) {
    if (handle < 0) return false;
    pthread_mutex_lock(&jsonHandleMutex);
    size_t idx = (size_t)handle;
    bool released = false;
    if (idx < jsonHandleCapacity) {
        JsonHandleEntry *entry = &jsonHandleTable[idx];
        if (entry->kind == JSON_HANDLE_VAL) {
            jsonResetEntry(entry);
            released = true;
        }
    }
    pthread_mutex_unlock(&jsonHandleMutex);
    return released;
}

static yyjson_doc *jsonDetachDocHandle(int handle) {
    if (handle < 0) return NULL;
    pthread_mutex_lock(&jsonHandleMutex);
    size_t idx = (size_t)handle;
    yyjson_doc *doc = NULL;
    if (idx < jsonHandleCapacity) {
        JsonHandleEntry *entry = &jsonHandleTable[idx];
        if (entry->kind == JSON_HANDLE_DOC && entry->doc) {
            doc = entry->doc;
            yyjson_doc *doc_ptr = entry->doc;
            jsonResetEntry(entry);
            for (size_t i = 0; i < jsonHandleCapacity; ++i) {
                if (jsonHandleTable[i].kind == JSON_HANDLE_VAL && jsonHandleTable[i].doc == doc_ptr) {
                    jsonResetEntry(&jsonHandleTable[i]);
                }
            }
        }
    }
    pthread_mutex_unlock(&jsonHandleMutex);
    return doc;
}

static const char *jsonTypeToString(yyjson_val *val) {
    switch (yyjson_get_type(val)) {
        case YYJSON_TYPE_NULL: return "null";
        case YYJSON_TYPE_BOOL: return yyjson_get_bool(val) ? "true" : "false";
        case YYJSON_TYPE_NUM:
            return yyjson_is_int(val) ? "int" : "real";
        case YYJSON_TYPE_STR: return "string";
        case YYJSON_TYPE_ARR: return "array";
        case YYJSON_TYPE_OBJ: return "object";
        case YYJSON_TYPE_RAW: return "raw";
        default: return "unknown";
    }
}

static Value vmBuiltinYyjsonRead(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || args[0].type != TYPE_STRING) {
        runtimeError(vm, "YyjsonRead expects a single string argument.");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    const char *json = args[0].s_val ? args[0].s_val : "";
    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_opts((char *)json, strlen(json), 0, NULL, &err);
    if (!doc) {
        runtimeError(vm, "YyjsonRead failed at position %zu: %s", err.pos, err.msg ? err.msg : "unknown error");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    int handle = jsonAllocHandle(JSON_HANDLE_DOC, doc, NULL);
    if (handle == YYJSON_UNUSED_HANDLE) {
        yyjson_doc_free(doc);
        runtimeError(vm, "YyjsonRead: unable to allocate document handle.");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    return makeInt(handle);
}

static Value vmBuiltinYyjsonReadFile(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || args[0].type != TYPE_STRING) {
        runtimeError(vm, "YyjsonReadFile expects a single string argument.");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    const char *path = args[0].s_val ? args[0].s_val : "";
    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_file(path, 0, NULL, &err);
    if (!doc) {
        runtimeError(vm, "YyjsonReadFile failed at position %zu: %s", err.pos, err.msg ? err.msg : "unknown error");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    int handle = jsonAllocHandle(JSON_HANDLE_DOC, doc, NULL);
    if (handle == YYJSON_UNUSED_HANDLE) {
        yyjson_doc_free(doc);
        runtimeError(vm, "YyjsonReadFile: unable to allocate document handle.");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    return makeInt(handle);
}

static Value vmBuiltinYyjsonDocFree(struct VM_s *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonDocFree expects a single document handle.");
        return makeVoid();
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = jsonDetachDocHandle(handle);
    if (!doc) {
        runtimeError(vm, "YyjsonDocFree received an invalid document handle (%d).", handle);
        return makeVoid();
    }
    yyjson_doc_free(doc);
    return makeVoid();
}

static Value vmBuiltinYyjsonFreeValue(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonFreeValue expects a single value handle.");
        return makeVoid();
    }
    int handle = (int)AS_INTEGER(args[0]);
    if (!jsonReleaseValueHandle(handle)) {
        runtimeError(vm, "YyjsonFreeValue received an invalid value handle (%d).", handle);
    }
    return makeVoid();
}

static Value vmBuiltinYyjsonGetRoot(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonGetRoot expects a single document handle.");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = jsonLookupDoc(handle);
    if (!doc) {
        runtimeError(vm, "YyjsonGetRoot received an invalid document handle (%d).", handle);
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root) {
        runtimeError(vm, "YyjsonGetRoot: document has no root value.");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    int value_handle = jsonAllocHandle(JSON_HANDLE_VAL, doc, root);
    if (value_handle == YYJSON_UNUSED_HANDLE) {
        runtimeError(vm, "YyjsonGetRoot: unable to allocate value handle.");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    return makeInt(value_handle);
}

static Value vmBuiltinYyjsonGetKey(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2 || !IS_INTLIKE(args[0]) || args[1].type != TYPE_STRING) {
        runtimeError(vm, "YyjsonGetKey expects (value_handle:int, key:string).");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonLookupValue(handle, &doc, &val)) {
        runtimeError(vm, "YyjsonGetKey received an invalid value handle (%d).", handle);
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    if (!yyjson_is_obj(val)) {
        runtimeError(vm, "YyjsonGetKey requires an object value handle.");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    const char *key = args[1].s_val ? args[1].s_val : "";
    yyjson_val *child = yyjson_obj_get(val, key);
    if (!child) {
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    int child_handle = jsonAllocHandle(JSON_HANDLE_VAL, doc, child);
    if (child_handle == YYJSON_UNUSED_HANDLE) {
        runtimeError(vm, "YyjsonGetKey: unable to allocate value handle.");
    }
    return makeInt(child_handle);
}

static Value vmBuiltinYyjsonGetIndex(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2 || !IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1])) {
        runtimeError(vm, "YyjsonGetIndex expects (value_handle:int, index:int).");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    int handle = (int)AS_INTEGER(args[0]);
    long long index = AS_INTEGER(args[1]);
    if (index < 0) {
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonLookupValue(handle, &doc, &val)) {
        runtimeError(vm, "YyjsonGetIndex received an invalid value handle (%d).", handle);
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    if (!yyjson_is_arr(val)) {
        runtimeError(vm, "YyjsonGetIndex requires an array value handle.");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    yyjson_val *child = yyjson_arr_get(val, (size_t)index);
    if (!child) {
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    int child_handle = jsonAllocHandle(JSON_HANDLE_VAL, doc, child);
    if (child_handle == YYJSON_UNUSED_HANDLE) {
        runtimeError(vm, "YyjsonGetIndex: unable to allocate value handle.");
    }
    return makeInt(child_handle);
}

static Value vmBuiltinYyjsonGetLength(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonGetLength expects a single value handle.");
        return makeInt(-1);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonLookupValue(handle, &doc, &val)) {
        runtimeError(vm, "YyjsonGetLength received an invalid value handle (%d).", handle);
        return makeInt(-1);
    }
    if (yyjson_is_arr(val)) {
        return makeInt((long long)yyjson_arr_size(val));
    } else if (yyjson_is_obj(val)) {
        return makeInt((long long)yyjson_obj_size(val));
    }
    runtimeError(vm, "YyjsonGetLength requires an array or object value handle.");
    return makeInt(-1);
}

static Value vmBuiltinYyjsonGetType(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonGetType expects a single value handle.");
        return makeString("");
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonLookupValue(handle, &doc, &val)) {
        runtimeError(vm, "YyjsonGetType received an invalid value handle (%d).", handle);
        return makeString("");
    }
    return makeString(jsonTypeToString(val));
}

static Value vmBuiltinYyjsonGetString(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonGetString expects a single value handle.");
        return makeString("");
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonLookupValue(handle, &doc, &val)) {
        runtimeError(vm, "YyjsonGetString received an invalid value handle (%d).", handle);
        return makeString("");
    }
    if (!yyjson_is_str(val)) {
        runtimeError(vm, "YyjsonGetString requires a string value handle.");
        return makeString("");
    }
    const char *str = yyjson_get_str(val);
    return makeString(str ? str : "");
}

static Value vmBuiltinYyjsonGetNumber(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonGetNumber expects a single value handle.");
        return makeDouble(0.0);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonLookupValue(handle, &doc, &val)) {
        runtimeError(vm, "YyjsonGetNumber received an invalid value handle (%d).", handle);
        return makeDouble(0.0);
    }
    if (yyjson_is_real(val)) {
        return makeDouble(yyjson_get_real(val));
    }
    if (yyjson_is_int(val)) {
        return makeDouble((double)yyjson_get_sint(val));
    }
    runtimeError(vm, "YyjsonGetNumber requires a numeric value handle.");
    return makeDouble(0.0);
}

static Value vmBuiltinYyjsonGetInt(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonGetInt expects a single value handle.");
        return makeInt64(0);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonLookupValue(handle, &doc, &val)) {
        runtimeError(vm, "YyjsonGetInt received an invalid value handle (%d).", handle);
        return makeInt64(0);
    }
    if (!yyjson_is_int(val)) {
        runtimeError(vm, "YyjsonGetInt requires an integer value handle.");
        return makeInt64(0);
    }
    return makeInt64(yyjson_get_sint(val));
}

static Value vmBuiltinYyjsonGetBool(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonGetBool expects a single value handle.");
        return makeInt(0);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonLookupValue(handle, &doc, &val)) {
        runtimeError(vm, "YyjsonGetBool received an invalid value handle (%d).", handle);
        return makeInt(0);
    }
    if (!yyjson_is_bool(val)) {
        runtimeError(vm, "YyjsonGetBool requires a boolean value handle.");
        return makeInt(0);
    }
    return makeInt(yyjson_get_bool(val) ? 1 : 0);
}

static Value vmBuiltinYyjsonIsNull(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonIsNull expects a single value handle.");
        return makeInt(0);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonLookupValue(handle, &doc, &val)) {
        runtimeError(vm, "YyjsonIsNull received an invalid value handle (%d).", handle);
        return makeInt(0);
    }
    return makeInt(yyjson_is_null(val) ? 1 : 0);
}

void registerYyjsonReadBuiltin(void) {
    registerVmBuiltin("yyjsonread", vmBuiltinYyjsonRead, BUILTIN_TYPE_FUNCTION, "YyjsonRead");
}

void registerYyjsonReadFileBuiltin(void) {
    registerVmBuiltin("yyjsonreadfile", vmBuiltinYyjsonReadFile, BUILTIN_TYPE_FUNCTION, "YyjsonReadFile");
}

void registerYyjsonDocFreeBuiltin(void) {
    registerVmBuiltin("yyjsondocfree", vmBuiltinYyjsonDocFree, BUILTIN_TYPE_PROCEDURE, "YyjsonDocFree");
}

void registerYyjsonFreeValueBuiltin(void) {
    registerVmBuiltin("yyjsonfreevalue", vmBuiltinYyjsonFreeValue, BUILTIN_TYPE_PROCEDURE, "YyjsonFreeValue");
}

void registerYyjsonGetRootBuiltin(void) {
    registerVmBuiltin("yyjsongetroot", vmBuiltinYyjsonGetRoot, BUILTIN_TYPE_FUNCTION, "YyjsonGetRoot");
}

void registerYyjsonGetKeyBuiltin(void) {
    registerVmBuiltin("yyjsongetkey", vmBuiltinYyjsonGetKey, BUILTIN_TYPE_FUNCTION, "YyjsonGetKey");
}

void registerYyjsonGetIndexBuiltin(void) {
    registerVmBuiltin("yyjsongetindex", vmBuiltinYyjsonGetIndex, BUILTIN_TYPE_FUNCTION, "YyjsonGetIndex");
}

void registerYyjsonGetLengthBuiltin(void) {
    registerVmBuiltin("yyjsongetlength", vmBuiltinYyjsonGetLength, BUILTIN_TYPE_FUNCTION, "YyjsonGetLength");
}

void registerYyjsonGetTypeBuiltin(void) {
    registerVmBuiltin("yyjsongettype", vmBuiltinYyjsonGetType, BUILTIN_TYPE_FUNCTION, "YyjsonGetType");
}

void registerYyjsonGetStringBuiltin(void) {
    registerVmBuiltin("yyjsongetstring", vmBuiltinYyjsonGetString, BUILTIN_TYPE_FUNCTION, "YyjsonGetString");
}

void registerYyjsonGetNumberBuiltin(void) {
    registerVmBuiltin("yyjsongetnumber", vmBuiltinYyjsonGetNumber, BUILTIN_TYPE_FUNCTION, "YyjsonGetNumber");
}

void registerYyjsonGetIntBuiltin(void) {
    registerVmBuiltin("yyjsongetint", vmBuiltinYyjsonGetInt, BUILTIN_TYPE_FUNCTION, "YyjsonGetInt");
}

void registerYyjsonGetBoolBuiltin(void) {
    registerVmBuiltin("yyjsongetbool", vmBuiltinYyjsonGetBool, BUILTIN_TYPE_FUNCTION, "YyjsonGetBool");
}

void registerYyjsonIsNullBuiltin(void) {
    registerVmBuiltin("yyjsonisnull", vmBuiltinYyjsonIsNull, BUILTIN_TYPE_FUNCTION, "YyjsonIsNull");
}
