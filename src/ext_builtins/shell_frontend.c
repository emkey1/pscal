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
    registerShellBuiltin(category, runtime_group, "__shell_loop_end", vmBuiltinShellLoopEnd);
    registerShellBuiltin(category, runtime_group, "__shell_if", vmBuiltinShellIf);
    registerShellBuiltin(category, runtime_group, "__shell_case", vmBuiltinShellCase);
    registerShellBuiltin(category, runtime_group, "__shell_case_clause", vmBuiltinShellCaseClause);
    registerShellBuiltin(category, runtime_group, "__shell_case_end", vmBuiltinShellCaseEnd);
    registerShellBuiltin(category, runtime_group, "__shell_define_function", vmBuiltinShellDefineFunction);
    registerShellBuiltin(category, runtime_group, "__shell_double_bracket", vmBuiltinShellDoubleBracket);

    registerShellBuiltin(category, command_group, "cd", vmBuiltinShellCd);
    registerShellBuiltin(category, command_group, "pwd", vmBuiltinShellPwd);
    registerShellBuiltin(category, command_group, "dirs", vmBuiltinShellDirs);
    registerShellBuiltin(category, command_group, "pushd", vmBuiltinShellPushd);
    registerShellBuiltin(category, command_group, "popd", vmBuiltinShellPopd);
    registerShellBuiltin(category, command_group, "source", vmBuiltinShellSource);
    registerShellBuiltin(category, command_group, "read", vmBuiltinShellRead);
    registerShellBuiltin(category, command_group, "eval", vmBuiltinShellEval);
    registerShellBuiltin(category, command_group, "let", vmBuiltinShellLet);
    registerShellBuiltin(category, command_group, "exit", vmBuiltinShellExit);
    registerShellBuiltin(category, command_group, "exec", vmBuiltinShellExecCommand);
    registerShellBuiltin(category, command_group, "shift", vmBuiltinShellShift);
    registerShellBuiltin(category, command_group, "set", vmBuiltinShellSet);
    registerShellBuiltin(category, command_group, "setenv", vmBuiltinShellSetenv);
    registerShellBuiltin(category, command_group, "declare", vmBuiltinShellDeclare);
    registerShellBuiltin(category, command_group, "typeset", vmBuiltinShellDeclare);
    registerShellBuiltin(category, command_group, "readonly", vmBuiltinShellReadonly);
    registerShellBuiltin(category, command_group, "command", vmBuiltinShellCommand);
    registerShellBuiltin(category, command_group, "export", vmBuiltinShellExport);
    registerShellBuiltin(category, command_group, "unset", vmBuiltinShellUnset);
    registerShellBuiltin(category, command_group, "unsetenv", vmBuiltinShellUnsetenv);
    registerShellBuiltin(category, command_group, "return", vmBuiltinShellReturn);
    registerShellBuiltin(category, command_group, "logout", vmBuiltinShellLogout);
    registerShellBuiltin(category, command_group, "finger", vmBuiltinShellFinger);
    registerShellBuiltin(category, command_group, "trap", vmBuiltinShellTrap);
    registerShellBuiltin(category, command_group, "local", vmBuiltinShellLocal);
    registerShellBuiltin(category, command_group, "break", vmBuiltinShellBreak);
    registerShellBuiltin(category, command_group, "continue", vmBuiltinShellContinue);
    registerShellBuiltin(category, command_group, "alias", vmBuiltinShellAlias);
    registerShellBuiltin(category, command_group, "unalias", vmBuiltinShellUnalias);
    registerShellBuiltin(category, command_group, "history", vmBuiltinShellHistory);
    registerShellBuiltin(category, command_group, "jobs", vmBuiltinShellJobs);
    registerShellBuiltin(category, command_group, "fg", vmBuiltinShellFg);
    registerShellBuiltin(category, command_group, "bg", vmBuiltinShellBg);
    registerShellBuiltin(category, command_group, "wait", vmBuiltinShellWait);
    registerShellBuiltin(category, command_group, "help", vmBuiltinShellHelp);
    registerShellBuiltin(category, command_group, "type", vmBuiltinShellType);
    registerShellBuiltin(category, command_group, "builtin", vmBuiltinShellBuiltin);
    registerShellBuiltin(category, command_group, ":", vmBuiltinShellColon);
    registerShellBuiltin(category, command_group, "bind", vmBuiltinShellBind);
    registerShellBuiltin(category, command_group, "shopt", vmBuiltinShellShopt);
    registerShellBuiltin(category, command_group, "umask", vmBuiltinShellUmask);
}
