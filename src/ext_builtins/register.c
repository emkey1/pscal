#include "backend_ast/builtin.h"

void registerGetPidBuiltin(void);
void registerSwapBuiltin(void);
void registerFactorialBuiltin(void);
void registerFibonacciBuiltin(void);
void registerFileExistsBuiltin(void);

void registerExtendedBuiltins(void) {
    registerGetPidBuiltin();
    registerSwapBuiltin();
    registerFactorialBuiltin();
    registerFibonacciBuiltin();
    registerFileExistsBuiltin();
}
