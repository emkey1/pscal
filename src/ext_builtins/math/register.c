#include "backend_ast/builtin.h"

void registerFactorialBuiltin(void);
void registerFibonacciBuiltin(void);
void registerMandelbrotRowBuiltin(void);
void registerChudnovskyBuiltin(void);

void registerMathBuiltins(void) {
    registerFactorialBuiltin();
    registerFibonacciBuiltin();
    registerMandelbrotRowBuiltin();
    registerChudnovskyBuiltin();
}
