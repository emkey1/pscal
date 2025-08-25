#include "backend_ast/builtin.h"

void registerFactorialBuiltin(void);
void registerFibonacciBuiltin(void);

void registerMathBuiltins(void) {
    registerFactorialBuiltin();
    registerFibonacciBuiltin();
}
