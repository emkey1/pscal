#include "core/utils.h"
#include "backend_ast/builtin.h"

// Helper function to multiply two 2x2 matrices
static void multiplyMatrices(long long F[2][2], long long M[2][2]) {
    long long x = F[0][0] * M[0][0] + F[0][1] * M[1][0];
    long long y = F[0][0] * M[0][1] + F[0][1] * M[1][1];
    long long z = F[1][0] * M[0][0] + F[1][1] * M[1][0];
    long long w = F[1][0] * M[0][1] + F[1][1] * M[1][1];

    F[0][0] = x;
    F[0][1] = y;
    F[1][0] = z;
    F[1][1] = w;
}

// Helper function to raise a matrix to a power in O(log n) time
static void power(long long F[2][2], long long n) {
    if (n == 0 || n == 1) {
        return;
    }
    long long M[2][2] = {{1, 1}, {1, 0}};

    power(F, n / 2);
    multiplyMatrices(F, F);

    if (n % 2 != 0) {
        multiplyMatrices(F, M);
    }
}

static Value vmBuiltinFibonacci(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Fibonacci expects exactly 1 argument.");
        return makeInt(-1);
    }
    if (!IS_INTLIKE(args[0])) {
        runtimeError(vm, "Fibonacci argument must be an integer.");
        return makeInt(-1);
    }
    long long n = asI64(args[0]);
    if (n < 0) {
        runtimeError(vm, "Fibonacci argument must be non-negative.");
        return makeInt(-1);
    }
    if (n == 0) {
        return makeInt(0);
    }

    long long F[2][2] = {{1, 1}, {1, 0}};
    power(F, n - 1);

    // The result F(n) is in the top-left of the matrix
    return makeInt(F[0][0]);
}

void registerFibonacciBuiltin(void) {
    registerVmBuiltin("fibonacci", vmBuiltinFibonacci,
                      BUILTIN_TYPE_FUNCTION, "Fibonacci");
}
