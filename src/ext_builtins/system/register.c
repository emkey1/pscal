#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"

void registerFileExistsBuiltin(void);
void registerGetPidBuiltin(void);
void registerSwapBuiltin(void);

void registerSystemBuiltins(void) {
  extBuiltinRegisterCategory("system");
  extBuiltinRegisterFunction("system", "FileExists");
  extBuiltinRegisterFunction("system", "GetPid");
  extBuiltinRegisterFunction("system", "Swap");

  registerFileExistsBuiltin();
  registerGetPidBuiltin();
  registerSwapBuiltin();
}
