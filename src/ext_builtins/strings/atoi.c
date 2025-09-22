#include <stdlib.h>
#include "core/utils.h"
#include "backend_ast/builtin.h"

static Value vmBuiltinAtoi(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "atoi expects exactly 1 argument.");
        return makeInt(0);
    }
    if (args[0].type != TYPE_STRING) {
        runtimeError(vm, "atoi argument must be a string.");
        return makeInt(0);
    }
    const char* s = args[0].s_val;
    if (!s) {
        runtimeError(vm, "atoi received NIL string.");
        return makeInt(0);
    }
    return makeInt(atoi(s));
}

void registerAtoiBuiltin(void) {
    registerVmBuiltin("atoi", vmBuiltinAtoi,
                      BUILTIN_TYPE_FUNCTION, "Atoi");
}

