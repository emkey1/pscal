#include "backend_ast/builtin.h"

void registerGetPidBuiltin(void);
void registerSwapBuiltin(void);

void registerExtendedBuiltins(void) {
    registerGetPidBuiltin();
    registerSwapBuiltin();
}
