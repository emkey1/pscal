#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(PSCAL_TARGET_IOS) || (defined(TARGET_OS_MACCATALYST) && TARGET_OS_MACCATALYST)
#define PSCAL_MOBILE_PLATFORM 1
#endif

static void registerShellBuiltin(const char *category, const char *group,
                                 const char *name, VmBuiltinFn handler) {
    registerVmBuiltin(name, handler, BUILTIN_TYPE_PROCEDURE, NULL);
    extBuiltinRegisterFunction(category, group, name);
}

void registerShellFrontendBuiltins(void) {
    const char *category = "shell";
    const char *runtime_group = "runtime";
    const char *command_group = "commands";
    const char *thread_group = "threading";

    extBuiltinRegisterCategory(category);
    extBuiltinRegisterGroup(category, runtime_group);
    extBuiltinRegisterGroup(category, command_group);
    extBuiltinRegisterGroup(category, thread_group);

    registerShellBuiltin(category, runtime_group, "__shell_exec", vmBuiltinShellExec);
    registerShellBuiltin(category, runtime_group, "__shell_pipeline", vmBuiltinShellPipeline);
    registerShellBuiltin(category, runtime_group, "__shell_arithmetic", vmBuiltinShellArithmetic);
    registerShellBuiltin(category, runtime_group, "__shell_and", vmBuiltinShellAnd);
    registerShellBuiltin(category, runtime_group, "__shell_or", vmBuiltinShellOr);
    registerShellBuiltin(category, runtime_group, "__shell_subshell", vmBuiltinShellSubshell);
    registerShellBuiltin(category, runtime_group, "__shell_loop", vmBuiltinShellLoop);
    registerShellBuiltin(category, runtime_group, "__shell_loop_end", vmBuiltinShellLoopEnd);
    registerShellBuiltin(category, runtime_group, "__shell_enter_condition", vmBuiltinShellEnterCondition);
    registerShellBuiltin(category, runtime_group, "__shell_leave_condition", vmBuiltinShellLeaveCondition);
    registerShellBuiltin(category, runtime_group, "__shell_leave_condition_preserve",
                        vmBuiltinShellLeaveConditionPreserve);
    registerShellBuiltin(category, runtime_group, "__shell_if", vmBuiltinShellIf);
    registerShellBuiltin(category, runtime_group, "__shell_case", vmBuiltinShellCase);
    registerShellBuiltin(category, runtime_group, "__shell_case_clause", vmBuiltinShellCaseClause);
    registerShellBuiltin(category, runtime_group, "__shell_case_end", vmBuiltinShellCaseEnd);
    registerShellBuiltin(category, runtime_group, "__shell_define_function", vmBuiltinShellDefineFunction);
    registerShellBuiltin(category, runtime_group, "__shell_double_bracket", vmBuiltinShellDoubleBracket);

    registerShellBuiltin(category, command_group, "test", vmBuiltinShellTest);
    registerShellBuiltin(category, command_group, "cd", vmBuiltinShellCd);
    registerShellBuiltin(category, command_group, "pwd", vmBuiltinShellPwd);
    registerShellBuiltin(category, command_group, "dirs", vmBuiltinShellDirs);
    registerShellBuiltin(category, command_group, "pushd", vmBuiltinShellPushd);
    registerShellBuiltin(category, command_group, "popd", vmBuiltinShellPopd);
    registerShellBuiltin(category, command_group, "source", vmBuiltinShellSource);
    registerShellBuiltin(category, command_group, "read", vmBuiltinShellRead);
    registerShellBuiltin(category, command_group, "printf", vmBuiltinShellPrintf);
    registerShellBuiltin(category, command_group, "getopts", vmBuiltinShellGetopts);
    registerShellBuiltin(category, command_group, "mapfile", vmBuiltinShellMapfile);
    registerShellBuiltin(category, command_group, "readarray", vmBuiltinShellMapfile);
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
    registerShellBuiltin(category, command_group, "caller", vmBuiltinShellCaller);
    registerShellBuiltin(category, command_group, "history", vmBuiltinShellHistory);
    registerShellBuiltin(category, command_group, "jobs", vmBuiltinShellJobs);
    registerShellBuiltin(category, command_group, "disown", vmBuiltinShellDisown);
    registerShellBuiltin(category, command_group, "kill", vmBuiltinShellKill);
    registerShellBuiltin(category, command_group, "fg", vmBuiltinShellFg);
    registerShellBuiltin(category, command_group, "bg", vmBuiltinShellBg);
    registerShellBuiltin(category, command_group, "wait", vmBuiltinShellWait);
    registerShellBuiltin(category, command_group, "WaitForThread", vmBuiltinShellWaitForThread);
    registerShellBuiltin(category, thread_group, "ps-threads", vmBuiltinShellPsThreads);
    registerShellBuiltin(category, thread_group, "ThreadSpawnBuiltin", vmBuiltinThreadSpawnBuiltin);
    registerShellBuiltin(category, thread_group, "ThreadGetResult", vmBuiltinThreadGetResult);
    registerShellBuiltin(category, thread_group, "ThreadGetStatus", vmBuiltinThreadGetStatus);
    registerShellBuiltin(category, command_group, "hash", vmBuiltinShellHash);
    registerShellBuiltin(category, command_group, "enable", vmBuiltinShellEnable);
    registerShellBuiltin(category, command_group, "help", vmBuiltinShellHelp);
    registerShellBuiltin(category, command_group, "type", vmBuiltinShellType);
    registerShellBuiltin(category, command_group, "which", vmBuiltinShellWhich);
    registerShellBuiltin(category, command_group, "builtin", vmBuiltinShellBuiltin);
    registerShellBuiltin(category, command_group, ":", vmBuiltinShellColon);
    registerShellBuiltin(category, command_group, "bind", vmBuiltinShellBind);
    registerShellBuiltin(category, command_group, "shopt", vmBuiltinShellShopt);
    registerShellBuiltin(category, command_group, "umask", vmBuiltinShellUmask);
    registerShellBuiltin(category, command_group, "times", vmBuiltinShellTimes);
    registerShellBuiltin(category, command_group, "echo", vmBuiltinShellEcho);
    registerShellBuiltin(category, command_group, "true", vmBuiltinShellTrue);
    registerShellBuiltin(category, command_group, "false", vmBuiltinShellFalse);
#ifdef PSCAL_MOBILE_PLATFORM
    registerShellBuiltin(category, command_group, "pascal", vmBuiltinShellPascal);
#ifdef BUILD_DASCAL
    registerShellBuiltin(category, command_group, "dascal", vmBuiltinShellDascal);
#endif
    registerShellBuiltin(category, command_group, "clike", vmBuiltinShellClike);
    registerShellBuiltin(category, command_group, "rea", vmBuiltinShellRea);
    registerShellBuiltin(category, command_group, "exsh", vmBuiltinShellExshTool);
    registerShellBuiltin(category, command_group, "pscalvm", vmBuiltinShellPscalVm);
    registerShellBuiltin(category, command_group, "pscaljson2bc", vmBuiltinShellPscalJson2bc);
#ifdef BUILD_PSCALD
    registerShellBuiltin(category, command_group, "pscald", vmBuiltinShellPscald);
    registerShellBuiltin(category, command_group, "pscalasm", vmBuiltinShellPscalasm);
#endif
    registerShellBuiltin(category, command_group, "resize", vmBuiltinShellResize);
    registerShellBuiltin(category, command_group, "gwin", vmBuiltinShellGwin);
    registerShellBuiltin(category, command_group, "ps", vmBuiltinShellPs);
    registerShellBuiltin(category, command_group, "lps", vmBuiltinShellPs);
    registerShellBuiltin(category, command_group, "sh", vmBuiltinShellExshTool);
#endif
#if defined(PSCAL_TAB_TITLE_SUPPORT)
    registerShellBuiltin(category, command_group, "tabname", vmBuiltinShellTabName);
    registerShellBuiltin(category, command_group, "tname", vmBuiltinShellTabName);
    registerShellBuiltin(category, command_group, "tscommand", vmBuiltinShellTabStartupCommand);
    registerShellBuiltin(category, command_group, "tabscommand", vmBuiltinShellTabStartupCommand);
#endif
    registerShellBuiltin(category, command_group, "stdioinfo", vmBuiltinShellStdioInfo);
}
