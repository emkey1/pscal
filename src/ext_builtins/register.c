#include "backend_ast/builtin.h"

void registerGetPidBuiltin(void);
void registerSwapBuiltin(void);
void registerFactorialBuiltin(void);
void registerFibonacciBuiltin(void);
void registerFileExistsBuiltin(void);
void registerMandelbrotRowBuiltin(void);

void registerExtendedBuiltins(void) {
    registerGetPidBuiltin();
    registerSwapBuiltin();
    registerFactorialBuiltin();
    registerFibonacciBuiltin();
    registerFileExistsBuiltin();
    registerMandelbrotRowBuiltin();
}
