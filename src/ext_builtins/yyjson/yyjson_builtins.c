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
    size_t refcount;
    int doc_handle;
} JsonHandleEntry;

static JsonHandleEntry *jsonHandleTable = NULL;
static size_t jsonHandleCapacity = 0;
static pthread_mutex_t jsonHandleMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jsonHandleCond = PTHREAD_COND_INITIALIZER;

static void jsonResetEntry(JsonHandleEntry *entry) {
    if (!entry) return;
    entry->kind = JSON_HANDLE_UNUSED;
    entry->doc = NULL;
    entry->val = NULL;
    entry->refcount = 0;
    entry->doc_handle = YYJSON_UNUSED_HANDLE;
}

static size_t jsonFindFreeSlotLocked(void) {
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
            return (size_t)-1;
        }
        for (size_t i = jsonHandleCapacity; i < new_capacity; ++i) {
            jsonResetEntry(&new_table[i]);
        }
        jsonHandleTable = new_table;
        slot = jsonHandleCapacity;
        jsonHandleCapacity = new_capacity;
    }
    return slot;
}

static int jsonAllocDocHandle(yyjson_doc *doc) {
    if (!doc) {
        return YYJSON_UNUSED_HANDLE;
    }

    pthread_mutex_lock(&jsonHandleMutex);
    size_t slot = jsonFindFreeSlotLocked();
    if (slot == (size_t)-1) {
        pthread_mutex_unlock(&jsonHandleMutex);
        return YYJSON_UNUSED_HANDLE;
    }

    JsonHandleEntry *entry = &jsonHandleTable[slot];
    entry->kind = JSON_HANDLE_DOC;
    entry->doc = doc;
    entry->val = NULL;
    entry->refcount = 0;
    entry->doc_handle = YYJSON_UNUSED_HANDLE;

    pthread_mutex_unlock(&jsonHandleMutex);
    return (int)slot;
}

static size_t jsonFindDocIndexLocked(yyjson_doc *doc) {
    for (size_t i = 0; i < jsonHandleCapacity; ++i) {
        JsonHandleEntry *entry = &jsonHandleTable[i];
        if (entry->kind == JSON_HANDLE_DOC && entry->doc == doc) {
            return i;
        }
    }
    return (size_t)-1;
}

static int jsonAllocValueHandle(yyjson_doc *doc, yyjson_val *val) {
    if (!doc || !val) {
        return YYJSON_UNUSED_HANDLE;
    }

    pthread_mutex_lock(&jsonHandleMutex);
    size_t doc_index = jsonFindDocIndexLocked(doc);
    if (doc_index == (size_t)-1) {
        pthread_mutex_unlock(&jsonHandleMutex);
        return YYJSON_UNUSED_HANDLE;
    }

    JsonHandleEntry *doc_entry = &jsonHandleTable[doc_index];
    if (doc_entry->doc == NULL) {
        pthread_mutex_unlock(&jsonHandleMutex);
        return YYJSON_UNUSED_HANDLE;
    }

    size_t slot = jsonFindFreeSlotLocked();
    if (slot == (size_t)-1) {
        pthread_mutex_unlock(&jsonHandleMutex);
        return YYJSON_UNUSED_HANDLE;
    }

    JsonHandleEntry *entry = &jsonHandleTable[slot];
    entry->kind = JSON_HANDLE_VAL;
    entry->doc = doc;
    entry->val = val;
    entry->refcount = 0;
    entry->doc_handle = (int)doc_index;

    pthread_mutex_unlock(&jsonHandleMutex);
    return (int)slot;
}

static bool jsonAcquireDoc(int handle, yyjson_doc **out_doc) {
    if (handle < 0 || !out_doc) {
        return false;
    }

    pthread_mutex_lock(&jsonHandleMutex);
    size_t idx = (size_t)handle;
    bool ok = false;
    if (idx < jsonHandleCapacity) {
        JsonHandleEntry *entry = &jsonHandleTable[idx];
        if (entry->kind == JSON_HANDLE_DOC && entry->doc) {
            entry->refcount++;
            *out_doc = entry->doc;
            ok = true;
        }
    }
    if (!ok) {
        pthread_mutex_unlock(&jsonHandleMutex);
        return false;
    }
    pthread_mutex_unlock(&jsonHandleMutex);
    return true;
}

static void jsonReleaseDoc(int handle) {
    if (handle < 0) {
        return;
    }

    pthread_mutex_lock(&jsonHandleMutex);
    size_t idx = (size_t)handle;
    if (idx < jsonHandleCapacity) {
        JsonHandleEntry *entry = &jsonHandleTable[idx];
        if (entry->kind == JSON_HANDLE_DOC && entry->refcount > 0) {
            entry->refcount--;
            pthread_cond_broadcast(&jsonHandleCond);
        }
    }
    pthread_mutex_unlock(&jsonHandleMutex);
}

static bool jsonAcquireValue(int handle, yyjson_doc **out_doc, yyjson_val **out_val, int *out_doc_handle) {
    if (handle < 0 || !out_doc || !out_val) {
        return false;
    }

    pthread_mutex_lock(&jsonHandleMutex);
    size_t idx = (size_t)handle;
    bool ok = false;
    if (idx < jsonHandleCapacity) {
        JsonHandleEntry *entry = &jsonHandleTable[idx];
        if (entry->kind == JSON_HANDLE_VAL && entry->doc && entry->val) {
            int doc_handle = entry->doc_handle;
            if (doc_handle >= 0 && (size_t)doc_handle < jsonHandleCapacity) {
                JsonHandleEntry *doc_entry = &jsonHandleTable[doc_handle];
                if (doc_entry->kind == JSON_HANDLE_DOC && doc_entry->doc == entry->doc) {
                    if (doc_entry->doc) {
                        entry->refcount++;
                        doc_entry->refcount++;
                        *out_doc = entry->doc;
                        *out_val = entry->val;
                        if (out_doc_handle) {
                            *out_doc_handle = doc_handle;
                        }
                        ok = true;
                    }
                }
            }
        }
    }
    if (!ok) {
        pthread_mutex_unlock(&jsonHandleMutex);
        return false;
    }
    pthread_mutex_unlock(&jsonHandleMutex);
    return true;
}

static void jsonReleaseValue(int handle) {
    if (handle < 0) {
        return;
    }

    pthread_mutex_lock(&jsonHandleMutex);
    size_t idx = (size_t)handle;
    if (idx < jsonHandleCapacity) {
        JsonHandleEntry *entry = &jsonHandleTable[idx];
        if (entry->kind == JSON_HANDLE_VAL && entry->refcount > 0) {
            entry->refcount--;
            int doc_handle = entry->doc_handle;
            if (doc_handle >= 0 && (size_t)doc_handle < jsonHandleCapacity) {
                JsonHandleEntry *doc_entry = &jsonHandleTable[doc_handle];
                if (doc_entry->kind == JSON_HANDLE_DOC && doc_entry->refcount > 0) {
                    doc_entry->refcount--;
                }
            }
            pthread_cond_broadcast(&jsonHandleCond);
        }
    }
    pthread_mutex_unlock(&jsonHandleMutex);
}

static bool jsonReleaseValueHandle(int handle) {
    if (handle < 0) return false;
    pthread_mutex_lock(&jsonHandleMutex);
    size_t idx = (size_t)handle;
    bool released = false;
    if (idx < jsonHandleCapacity) {
        JsonHandleEntry *entry = &jsonHandleTable[idx];
        if (entry->kind == JSON_HANDLE_VAL) {
            while (entry->refcount > 0) {
                pthread_cond_wait(&jsonHandleCond, &jsonHandleMutex);
            }
            jsonResetEntry(entry);
            pthread_cond_broadcast(&jsonHandleCond);
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
            entry->doc = NULL;
            while (entry->refcount > 0) {
                pthread_cond_wait(&jsonHandleCond, &jsonHandleMutex);
            }
            for (size_t i = 0; i < jsonHandleCapacity; ++i) {
                JsonHandleEntry *val_entry = &jsonHandleTable[i];
                if (val_entry->kind == JSON_HANDLE_VAL && val_entry->doc == doc) {
                    val_entry->doc = NULL;
                    while (val_entry->refcount > 0) {
                        pthread_cond_wait(&jsonHandleCond, &jsonHandleMutex);
                    }
                    jsonResetEntry(val_entry);
                }
            }
            jsonResetEntry(entry);
            pthread_cond_broadcast(&jsonHandleCond);
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
    int handle = jsonAllocDocHandle(doc);
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
    int handle = jsonAllocDocHandle(doc);
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
    yyjson_doc *doc = NULL;
    if (!jsonAcquireDoc(handle, &doc)) {
        runtimeError(vm, "YyjsonGetRoot received an invalid document handle (%d).", handle);
        return makeInt(YYJSON_UNUSED_HANDLE);
    }

    Value result = makeInt(YYJSON_UNUSED_HANDLE);
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root) {
        runtimeError(vm, "YyjsonGetRoot: document has no root value.");
        goto cleanup;
    }

    int value_handle = jsonAllocValueHandle(doc, root);
    if (value_handle == YYJSON_UNUSED_HANDLE) {
        runtimeError(vm, "YyjsonGetRoot: unable to allocate value handle.");
        goto cleanup;
    }

    result = makeInt(value_handle);

cleanup:
    jsonReleaseDoc(handle);
    return result;
}

static Value vmBuiltinYyjsonGetKey(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2 || !IS_INTLIKE(args[0]) || args[1].type != TYPE_STRING) {
        runtimeError(vm, "YyjsonGetKey expects (value_handle:int, key:string).");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        runtimeError(vm, "YyjsonGetKey received an invalid value handle (%d).", handle);
        return makeInt(YYJSON_UNUSED_HANDLE);
    }

    Value result = makeInt(YYJSON_UNUSED_HANDLE);
    if (!yyjson_is_obj(val)) {
        runtimeError(vm, "YyjsonGetKey requires an object value handle.");
        goto cleanup;
    }

    const char *key = args[1].s_val ? args[1].s_val : "";
    yyjson_val *child = yyjson_obj_get(val, key);
    if (!child) {
        goto cleanup;
    }

    int child_handle = jsonAllocValueHandle(doc, child);
    if (child_handle == YYJSON_UNUSED_HANDLE) {
        runtimeError(vm, "YyjsonGetKey: unable to allocate value handle.");
        goto cleanup;
    }

    result = makeInt(child_handle);

cleanup:
    jsonReleaseValue(handle);
    return result;
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
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        runtimeError(vm, "YyjsonGetIndex received an invalid value handle (%d).", handle);
        return makeInt(YYJSON_UNUSED_HANDLE);
    }

    Value result = makeInt(YYJSON_UNUSED_HANDLE);
    if (!yyjson_is_arr(val)) {
        runtimeError(vm, "YyjsonGetIndex requires an array value handle.");
        goto cleanup;
    }

    yyjson_val *child = yyjson_arr_get(val, (size_t)index);
    if (!child) {
        goto cleanup;
    }

    int child_handle = jsonAllocValueHandle(doc, child);
    if (child_handle == YYJSON_UNUSED_HANDLE) {
        runtimeError(vm, "YyjsonGetIndex: unable to allocate value handle.");
        goto cleanup;
    }

    result = makeInt(child_handle);

cleanup:
    jsonReleaseValue(handle);
    return result;
}

static Value vmBuiltinYyjsonGetLength(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonGetLength expects a single value handle.");
        return makeInt(-1);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        runtimeError(vm, "YyjsonGetLength received an invalid value handle (%d).", handle);
        return makeInt(-1);
    }

    Value result = makeInt(-1);
    if (yyjson_is_arr(val)) {
        result = makeInt((long long)yyjson_arr_size(val));
        goto cleanup;
    }
    if (yyjson_is_obj(val)) {
        result = makeInt((long long)yyjson_obj_size(val));
        goto cleanup;
    }

    runtimeError(vm, "YyjsonGetLength requires an array or object value handle.");

cleanup:
    jsonReleaseValue(handle);
    return result;
}

static Value vmBuiltinYyjsonGetType(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonGetType expects a single value handle.");
        return makeString("");
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        runtimeError(vm, "YyjsonGetType received an invalid value handle (%d).", handle);
        return makeString("");
    }

    Value result = makeString(jsonTypeToString(val));
    jsonReleaseValue(handle);
    return result;
}

static Value vmBuiltinYyjsonGetString(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonGetString expects a single value handle.");
        return makeString("");
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        runtimeError(vm, "YyjsonGetString received an invalid value handle (%d).", handle);
        return makeString("");
    }

    Value result = makeString("");
    if (!yyjson_is_str(val)) {
        runtimeError(vm, "YyjsonGetString requires a string value handle.");
        goto cleanup;
    }

    const char *str = yyjson_get_str(val);
    result = makeString(str ? str : "");

cleanup:
    jsonReleaseValue(handle);
    return result;
}

static Value vmBuiltinYyjsonGetNumber(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonGetNumber expects a single value handle.");
        return makeDouble(0.0);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        runtimeError(vm, "YyjsonGetNumber received an invalid value handle (%d).", handle);
        return makeDouble(0.0);
    }

    Value result = makeDouble(0.0);
    if (yyjson_is_real(val)) {
        result = makeDouble(yyjson_get_real(val));
        goto cleanup;
    }
    if (yyjson_is_int(val)) {
        result = makeDouble((double)yyjson_get_sint(val));
        goto cleanup;
    }

    runtimeError(vm, "YyjsonGetNumber requires a numeric value handle.");

cleanup:
    jsonReleaseValue(handle);
    return result;
}

static Value vmBuiltinYyjsonGetInt(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonGetInt expects a single value handle.");
        return makeInt64(0);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        runtimeError(vm, "YyjsonGetInt received an invalid value handle (%d).", handle);
        return makeInt64(0);
    }

    Value result = makeInt64(0);
    if (!yyjson_is_int(val)) {
        runtimeError(vm, "YyjsonGetInt requires an integer value handle.");
        goto cleanup;
    }

    result = makeInt64(yyjson_get_sint(val));

cleanup:
    jsonReleaseValue(handle);
    return result;
}

static Value vmBuiltinYyjsonGetBool(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonGetBool expects a single value handle.");
        return makeInt(0);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        runtimeError(vm, "YyjsonGetBool received an invalid value handle (%d).", handle);
        return makeInt(0);
    }

    Value result = makeInt(0);
    if (!yyjson_is_bool(val)) {
        runtimeError(vm, "YyjsonGetBool requires a boolean value handle.");
        goto cleanup;
    }

    result = makeInt(yyjson_get_bool(val) ? 1 : 0);

cleanup:
    jsonReleaseValue(handle);
    return result;
}

static Value vmBuiltinYyjsonIsNull(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonIsNull expects a single value handle.");
        return makeInt(0);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        runtimeError(vm, "YyjsonIsNull received an invalid value handle (%d).", handle);
        return makeInt(0);
    }

    Value result = makeInt(yyjson_is_null(val) ? 1 : 0);
    jsonReleaseValue(handle);
    return result;
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
