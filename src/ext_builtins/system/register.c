#include "backend_ast/builtin.h"

void registerGetPidBuiltin(void);
void registerSwapBuiltin(void);
void registerFileExistsBuiltin(void);

void pascal_ext_system_init(void) {
  registerGetPidBuiltin();
  registerSwapBuiltin();
  registerFileExistsBuiltin();
}
