#include "core/utils.h"
#include "backend_ast/builtin.h"

static Value vmBuiltinFactorial(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Factorial expects exactly 1 argument.");
        return makeInt(-1);
    }
    if (args[0].type != TYPE_INTEGER) {
        runtimeError(vm, "Factorial argument must be an integer.");
        return makeInt(-1);
    }
    long long n = args[0].i_val;
    if (n < 0) {
        runtimeError(vm, "Factorial argument must be non-negative.");
        return makeInt(-1);
    }
    long long result = 1;
    for (long long i = 2; i <= n; ++i) {
        result *= i;
    }
    return makeInt(result);
}

void registerFactorialBuiltin(void) {
    registerBuiltinFunction("Factorial", AST_FUNCTION_DECL, NULL);
    registerVmBuiltin("factorial", vmBuiltinFactorial);
}
