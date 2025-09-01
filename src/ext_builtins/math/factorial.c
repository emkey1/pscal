#include "core/utils.h"
#include "backend_ast/builtin.h"

static Value vmBuiltinFactorial(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Factorial expects exactly 1 argument.");
        return makeInt(-1);
    }
    if (!IS_INTLIKE(args[0])) {
        runtimeError(vm, "Factorial argument must be an integer.");
        return makeInt(-1);
    }
    long long n = asI64(args[0]);
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
