#include "core/utils.h"
#include "backend_ast/builtin.h"
#include <math.h>

static Value vmBuiltinChudnovsky(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Chudnovsky expects exactly 1 argument.");
        return makeLongDouble(0.0L);
    }
    if (!is_intlike_type(args[0].type)) {
        runtimeError(vm, "Chudnovsky argument must be an integer.");
        return makeLongDouble(0.0L);
    }
    long n = (long)args[0].i_val;
    if (n <= 0) {
        runtimeError(vm, "Chudnovsky argument must be positive.");
        return makeLongDouble(0.0L);
    }

    const long double C3 = 262537412640768000.0L; // 640320^3
    long double sum = 13591409.0L;
    long double term = 1.0L;

    for (long k = 1; k < n; ++k) {
        long double k_d = (long double)k;
        term *= -(6.0L * k_d - 5.0L) * (2.0L * k_d - 1.0L) * (6.0L * k_d - 1.0L) * 24.0L;
        term /= k_d * k_d * k_d * C3;
        sum += term * (13591409.0L + 545140134.0L * k_d);
    }

    long double pi = 426880.0L * sqrtl(10005.0L) / sum;
    return makeLongDouble(pi);
}

void registerChudnovskyBuiltin(void) {
    registerBuiltinFunction("Chudnovsky", AST_FUNCTION_DECL, NULL);
    registerVmBuiltin("chudnovsky", vmBuiltinChudnovsky);
}
