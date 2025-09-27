#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"

void registerFactorialBuiltin(void);
void registerFibonacciBuiltin(void);
void registerMandelbrotRowBuiltin(void);
void registerChudnovskyBuiltin(void);

void registerMathBuiltins(void) {
    const char *category = "math";
    extBuiltinRegisterCategory(category);
    extBuiltinRegisterGroup(category, "series");
    extBuiltinRegisterGroup(category, "fractal");
    extBuiltinRegisterGroup(category, "constants");
    extBuiltinRegisterFunction(category, "series", "Factorial");
    extBuiltinRegisterFunction(category, "series", "Fibonacci");
    extBuiltinRegisterFunction(category, "fractal", "MandelbrotRow");
    extBuiltinRegisterFunction(category, "constants", "Chudnovsky");

    registerFactorialBuiltin();
    registerFibonacciBuiltin();
    registerMandelbrotRowBuiltin();
    registerChudnovskyBuiltin();
}
