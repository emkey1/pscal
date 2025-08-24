#include "core/utils.h"
#include "backend_ast/builtin.h"
#include <math.h>

static Value vmBuiltinChudnovsky(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Chudnovsky expects exactly 1 argument.");
        return makeReal(0.0);
    }
    if (args[0].type != TYPE_INTEGER) {
        runtimeError(vm, "Chudnovsky argument must be an integer.");
        return makeReal(0.0);
    }
    long n = args[0].i_val;
    if (n <= 0) {
        runtimeError(vm, "Chudnovsky argument must be positive.");
        return makeReal(0.0);
    }

    long double M = 1.0L;
    long double L = 13591409.0L;
    long double X = 1.0L;
    long double K = 6.0L;
    long double S = L;

    for (long k = 1; k < n; ++k) {
        long double k3 = (long double)k * k * k;
        M = (K * K * K - 16.0L * K) * M / k3;
        L += 545140134.0L;
        X *= -262537412640768000.0L;
        S += M * L / X;
        K += 12.0L;
    }

    long double pi = 426880.0L * sqrtl(10005.0L) / S;
    return makeReal((double)pi);
}

void registerChudnovskyBuiltin(void) {
    registerBuiltinFunction("Chudnovsky", AST_FUNCTION_DECL, NULL);
    registerVmBuiltin("chudnovsky", vmBuiltinChudnovsky);
}
