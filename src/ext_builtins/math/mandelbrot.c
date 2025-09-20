#include "core/utils.h"
#include "backend_ast/builtin.h"

static Value vmBuiltinMandelbrotRow(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 6) {
        runtimeError(vm, "MandelbrotRow expects 6 arguments.");
        return makeVoid();
    }
    if (!isRealType(args[0].type) || !isRealType(args[1].type) || !isRealType(args[2].type) ||
        !IS_INTLIKE(args[3]) || !IS_INTLIKE(args[4]) ||
        (args[5].type != TYPE_POINTER && args[5].type != TYPE_ARRAY)) {
        runtimeError(vm, "MandelbrotRow argument types are (Real, Real, Real, Integer, Integer, VAR array).");
        return makeVoid();
    }

    double minRe = (double)asLd(args[0]);
    double reFactor = (double)asLd(args[1]);
    double c_im = (double)asLd(args[2]);
    int maxIterations = (int)asI64(args[3]);
    int maxX = (int)asI64(args[4]);

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

    double c_re = minRe;
    for (int x = 0; x <= maxX; ++x, c_re += reFactor) {
        double Z_re = 0.0;
        double Z_im = 0.0;
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
        Value* outPtr = &outArr[x];
        // Directly assign the iteration count without freeing uninitialized memory
        outPtr->type = TYPE_INTEGER;
        SET_INT_VALUE(outPtr, n);
    }

    return makeVoid();
}

void registerMandelbrotRowBuiltin(void) {
    registerVmBuiltin("mandelbrotrow", vmBuiltinMandelbrotRow,
                      BUILTIN_TYPE_PROCEDURE, "MandelbrotRow");
}

