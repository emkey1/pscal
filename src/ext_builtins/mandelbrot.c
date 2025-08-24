#include "core/utils.h"
#include "backend_ast/builtin.h"

static Value vmBuiltinMandelbrotRow(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 6) {
        runtimeError(vm, "MandelbrotRow expects 6 arguments.");
        return makeVoid();
    }
    if (args[0].type != TYPE_REAL || args[1].type != TYPE_REAL || args[2].type != TYPE_REAL ||
        args[3].type != TYPE_INTEGER || args[4].type != TYPE_INTEGER ||
        (args[5].type != TYPE_POINTER && args[5].type != TYPE_ARRAY)) {
        runtimeError(vm, "MandelbrotRow argument types are (Real, Real, Real, Integer, Integer, VAR array).");
        return makeVoid();
    }

    double minRe = args[0].r_val;
    double reFactor = args[1].r_val;
    double c_im = args[2].r_val;
    int maxIterations = (int)args[3].i_val;
    int maxX = (int)args[4].i_val;

    // Resolve the output array and validate its size and bounds.
    Value* arrVal = NULL;
    if (args[5].type == TYPE_POINTER) {
        arrVal = (Value*)args[5].ptr_val;
    } else {
        arrVal = &args[5];
    }
    if (!arrVal || arrVal->type != TYPE_ARRAY) {
        runtimeError(vm, "MandelbrotRow expected VAR array parameter.");
        return makeVoid();
    }
    if (arrVal->dimensions > 1) {
        runtimeError(vm, "MandelbrotRow output array must be single dimensional.");
        return makeVoid();
    }
    int lower = (arrVal->dimensions > 0 && arrVal->lower_bounds)
                    ? arrVal->lower_bounds[0]
                    : arrVal->lower_bound;
    int upper = (arrVal->dimensions > 0 && arrVal->upper_bounds)
                    ? arrVal->upper_bounds[0]
                    : arrVal->upper_bound;
    if (lower != 0) {
        runtimeError(vm, "MandelbrotRow output array must start at index 0.");
        return makeVoid();
    }
    if (upper < maxX) {
        runtimeError(vm, "MandelbrotRow output array too small for max X of %d.", maxX);
        return makeVoid();
    }
    Value* outArr = arrVal->array_val;
    if (!outArr) {
        runtimeError(vm, "MandelbrotRow received a NIL pointer for output array.");
        return makeVoid();
    }

    for (int x = 0; x <= maxX; ++x) {
        double c_re = minRe + x * reFactor;
        double Z_re = c_re;
        double Z_im = c_im;
        int n = 0;
        while (n < maxIterations) {
            double Z_re2 = Z_re * Z_re;
            double Z_im2 = Z_im * Z_im;
            if (Z_re2 + Z_im2 > 4.0) {
                break;
            }
            double tmp = 2.0 * Z_re * Z_im + c_im;
            Z_re = Z_re2 - Z_im2 + c_re;
            Z_im = tmp;
            n++;
        }
        freeValue(&outArr[x]);
        outArr[x] = makeInt(n);
    }

    return makeVoid();
}

void registerMandelbrotRowBuiltin(void) {
    registerBuiltinFunction("MandelbrotRow", AST_PROCEDURE_DECL, NULL);
    registerVmBuiltin("mandelbrotrow", vmBuiltinMandelbrotRow);
}

