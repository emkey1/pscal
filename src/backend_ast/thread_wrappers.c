#include "backend_ast/thread_wrappers.h"

#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "vm/vm.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static void freeFieldValueList(FieldValue* head) {
    while (head) {
        FieldValue* next = head->next;
        if (head->name) {
            free(head->name);
        }
        freeValue(&head->value);
        free(head);
        head = next;
    }
}

static bool appendField(FieldValue** head, FieldValue** tail, const char* name, Value value) {
    if (!head || !tail || !name) {
        freeValue(&value);
        return false;
    }
    FieldValue* node = (FieldValue*)calloc(1, sizeof(FieldValue));
    if (!node) {
        freeValue(&value);
        return false;
    }
    node->name = strdup(name);
    if (!node->name) {
        free(node);
        freeValue(&value);
        return false;
    }
    node->value = value;
    node->next = NULL;
    if (!*head) {
        *head = node;
    } else {
        (*tail)->next = node;
    }
    *tail = node;
    return true;
}

static Value makeThreadOptionsValue(const char* name, bool has_name, bool submit_only) {
    FieldValue* head = NULL;
    FieldValue* tail = NULL;

    if (has_name && name && name[0] != '\0') {
        if (!appendField(&head, &tail, "name", makeString(name))) {
            freeFieldValueList(head);
            return makeRecord(NULL);
        }
    }

    if (!appendField(&head, &tail, "submitOnly", makeBoolean(submit_only ? 1 : 0))) {
        freeFieldValueList(head);
        return makeRecord(NULL);
    }

    return makeRecord(head);
}

static bool copyArgument(Value* dest, const Value* src) {
    if (!dest || !src) {
        return false;
    }
    *dest = makeCopyOfValue(src);
    return true;
}

static Value threadSpawnOrSubmitCommon(VM* vm, int arg_count, Value* args, bool submit_only) {
    if (!vm) {
        return makeInt(-1);
    }
    if (arg_count < 2) {
        runtimeError(vm, "%s expects at least a target and thread name.",
                     submit_only ? "thread_pool_submit" : "thread_spawn_named");
        return makeInt(-1);
    }

    const Value* target = &args[0];
    const Value* name_arg = &args[1];

    if (!(target->type == TYPE_STRING || IS_INTLIKE(*target))) {
        runtimeError(vm, "%s target must be a string or integer id.",
                     submit_only ? "thread_pool_submit" : "thread_spawn_named");
        return makeInt(-1);
    }

    const char* requested_name = "";
    bool include_name = false;
    if (name_arg->type == TYPE_STRING) {
        include_name = true;
        requested_name = name_arg->s_val ? name_arg->s_val : "";
    } else if (name_arg->type == TYPE_NIL) {
        include_name = false;
        requested_name = "";
    } else {
        runtimeError(vm, "%s expects the second argument to be a string thread name.",
                     submit_only ? "thread_pool_submit" : "thread_spawn_named");
        return makeInt(-1);
    }

    int forwarded = arg_count - 2;
    int total_args = forwarded + 1; // target + forwarded builtin args
    // Options appended as final argument.
    Value* call_args = (Value*)calloc(total_args + 1, sizeof(Value));
    if (!call_args) {
        runtimeError(vm, "Out of memory building thread request.");
        return makeInt(-1);
    }

    bool ok = copyArgument(&call_args[0], target);
    for (int i = 0; ok && i < forwarded; ++i) {
        ok = copyArgument(&call_args[1 + i], &args[2 + i]);
    }

    Value options = makeThreadOptionsValue(requested_name, include_name, submit_only);
    if (options.type != TYPE_RECORD) {
        ok = false;
    }
    call_args[total_args] = options;

    Value result;
    if (!ok) {
        runtimeError(vm, "Failed to build thread request arguments.");
        result = makeInt(-1);
    } else if (submit_only) {
        result = vmBuiltinThreadPoolSubmit(vm, total_args + 1, call_args);
    } else {
        result = vmBuiltinThreadSpawnBuiltin(vm, total_args + 1, call_args);
    }

    for (int i = 0; i < total_args + 1; ++i) {
        freeValue(&call_args[i]);
    }
    free(call_args);
    return result;
}

Value builtinThreadSpawnNamedWrapper(VM* vm, int arg_count, Value* args) {
    return threadSpawnOrSubmitCommon(vm, arg_count, args, false);
}

Value builtinThreadPoolSubmitWrapper(VM* vm, int arg_count, Value* args) {
    return threadSpawnOrSubmitCommon(vm, arg_count, args, true);
}
