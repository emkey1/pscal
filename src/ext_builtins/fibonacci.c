#include "core/utils.h"
#include "backend_ast/builtin.h"

static Value vmBuiltinFibonacci(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Fibonacci expects exactly 1 argument.");
        return makeInt(-1);
    }
    if (args[0].type != TYPE_INTEGER) {
        runtimeError(vm, "Fibonacci argument must be an integer.");
        return makeInt(-1);
    }
    long long n = args[0].i_val;
    if (n < 0) {
        runtimeError(vm, "Fibonacci argument must be non-negative.");
        return makeInt(-1);
    }
    long long a = 0;
    long long b = 1;
    for (long long i = 0; i < n; ++i) {
        long long temp = a;
        a = b;
        b = temp + b;
    }
    return makeInt(a);
}

void registerFibonacciBuiltin(void) {
    registerBuiltinFunction("Fibonacci", AST_FUNCTION_DECL, NULL);
    registerVmBuiltin("fibonacci", vmBuiltinFibonacci);
}
