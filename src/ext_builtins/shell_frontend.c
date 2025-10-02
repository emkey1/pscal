#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"

static void registerShellBuiltin(const char *category, const char *group,
                                 const char *name, VmBuiltinFn handler) {
    registerVmBuiltin(name, handler, BUILTIN_TYPE_PROCEDURE, NULL);
    extBuiltinRegisterFunction(category, group, name);
}

void registerShellFrontendBuiltins(void) {
    const char *category = "shell";
    const char *runtime_group = "runtime";
    const char *command_group = "commands";

    extBuiltinRegisterCategory(category);
    extBuiltinRegisterGroup(category, runtime_group);
    extBuiltinRegisterGroup(category, command_group);

    registerShellBuiltin(category, runtime_group, "__shell_exec", vmBuiltinShellExec);
    registerShellBuiltin(category, runtime_group, "__shell_pipeline", vmBuiltinShellPipeline);
    registerShellBuiltin(category, runtime_group, "__shell_and", vmBuiltinShellAnd);
    registerShellBuiltin(category, runtime_group, "__shell_or", vmBuiltinShellOr);
    registerShellBuiltin(category, runtime_group, "__shell_subshell", vmBuiltinShellSubshell);
    registerShellBuiltin(category, runtime_group, "__shell_loop", vmBuiltinShellLoop);
    registerShellBuiltin(category, runtime_group, "__shell_if", vmBuiltinShellIf);

    registerShellBuiltin(category, command_group, "cd", vmBuiltinShellCd);
    registerShellBuiltin(category, command_group, "pwd", vmBuiltinShellPwd);
    registerShellBuiltin(category, command_group, "exit", vmBuiltinShellExit);
    registerShellBuiltin(category, command_group, "setenv", vmBuiltinShellSetenv);
    registerShellBuiltin(category, command_group, "export", vmBuiltinShellExport);
    registerShellBuiltin(category, command_group, "unset", vmBuiltinShellUnset);
    registerShellBuiltin(category, command_group, "unsetenv", vmBuiltinShellUnsetenv);
    registerShellBuiltin(category, command_group, "alias", vmBuiltinShellAlias);
    registerShellBuiltin(category, command_group, "history", vmBuiltinShellHistory);
}
