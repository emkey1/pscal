#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"

void registerFileExistsBuiltin(void);
void registerGetCurrentDirBuiltin(void);
void registerGetPidBuiltin(void);
void registerRealTimeClockBuiltin(void);
void registerSwapBuiltin(void);

void registerSystemBuiltins(void) {
  const char *category = "system";
  extBuiltinRegisterCategory(category);
  extBuiltinRegisterGroup(category, "filesystem");
  extBuiltinRegisterGroup(category, "process");
  extBuiltinRegisterGroup(category, "timing");
  extBuiltinRegisterGroup(category, "utility");
  extBuiltinRegisterFunction(category, "filesystem", "FileExists");
  extBuiltinRegisterFunction(category, "filesystem", "GetCurrentDir");
  extBuiltinRegisterFunction(category, "process", "GetPid");
  extBuiltinRegisterFunction(category, "timing", "RealTimeClock");
  extBuiltinRegisterFunction(category, "utility", "Swap");

  registerFileExistsBuiltin();
  registerGetCurrentDirBuiltin();
  registerGetPidBuiltin();
  registerRealTimeClockBuiltin();
  registerSwapBuiltin();
}
