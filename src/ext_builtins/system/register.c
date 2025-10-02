#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"

void registerFileExistsBuiltin(void);
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
  extBuiltinRegisterGroup(category, "shell");
  extBuiltinRegisterFunction(category, "filesystem", "FileExists");
  extBuiltinRegisterFunction(category, "process", "GetPid");
  extBuiltinRegisterFunction(category, "timing", "RealTimeClock");
  extBuiltinRegisterFunction(category, "utility", "Swap");
  extBuiltinRegisterFunction(category, "shell", "__shell_exec");
  extBuiltinRegisterFunction(category, "shell", "__shell_pipeline");
  extBuiltinRegisterFunction(category, "shell", "__shell_and");
  extBuiltinRegisterFunction(category, "shell", "__shell_or");
  extBuiltinRegisterFunction(category, "shell", "__shell_subshell");
  extBuiltinRegisterFunction(category, "shell", "__shell_loop");
  extBuiltinRegisterFunction(category, "shell", "__shell_if");
  extBuiltinRegisterFunction(category, "shell", "cd");
  extBuiltinRegisterFunction(category, "shell", "pwd");
  extBuiltinRegisterFunction(category, "shell", "exit");
  extBuiltinRegisterFunction(category, "shell", "export");
  extBuiltinRegisterFunction(category, "shell", "unset");
  extBuiltinRegisterFunction(category, "shell", "alias");

  registerFileExistsBuiltin();
  registerGetPidBuiltin();
  registerRealTimeClockBuiltin();
  registerSwapBuiltin();
}
