#include "core/utils.h"
#include "backend_ast/builtin.h"
#include <string.h>
#include <stdlib.h>

static Value vmBuiltinReverseString(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "ReverseString expects 1 argument.");
        return makeString("");
    }
    if (args[0].type != TYPE_STRING) {
        runtimeError(vm, "ReverseString argument must be a string.");
        return makeString("");
    }
    const char* src = args[0].s_val;
    if (!src) {
        return makeString("");
    }
    size_t len = strlen(src);
    char* buf = (char*)malloc(len + 1);
    if (!buf) {
        runtimeError(vm, "Memory allocation failed in ReverseString.");
        return makeString("");
    }
    for (size_t i = 0; i < len; ++i) {
        buf[i] = src[len - 1 - i];
    }
    buf[len] = '\0';
    Value result = makeString(buf);
    free(buf);
    return result;
}

void registerReverseStringBuiltin(void) {
    registerBuiltinFunction("ReverseString", AST_FUNCTION_DECL, NULL);
    registerVmBuiltin("reversestring", vmBuiltinReverseString);
}
