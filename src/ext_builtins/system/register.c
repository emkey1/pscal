#include "backend_ast/builtin.h"

void registerFileExistsBuiltin(void);
void registerGetPidBuiltin(void);
void registerSwapBuiltin(void);
void registerVmVersionBuiltin(void);

void registerSystemBuiltins(void) {
    registerFileExistsBuiltin();
    registerGetPidBuiltin();
    registerSwapBuiltin();
    registerVmVersionBuiltin();
}
