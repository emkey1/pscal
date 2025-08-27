#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"

void registerFactorialBuiltin(void);
void registerFibonacciBuiltin(void);
void registerMandelbrotRowBuiltin(void);
void registerChudnovskyBuiltin(void);

void registerMathBuiltins(void) {
    extBuiltinRegisterCategory("math");
    extBuiltinRegisterFunction("math", "Factorial");
    extBuiltinRegisterFunction("math", "Fibonacci");
    extBuiltinRegisterFunction("math", "MandelbrotRow");
    extBuiltinRegisterFunction("math", "Chudnovsky");

    registerFactorialBuiltin();
    registerFibonacciBuiltin();
    registerMandelbrotRowBuiltin();
    registerChudnovskyBuiltin();
}
