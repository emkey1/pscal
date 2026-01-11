/* Auto-generated include: shell builtins exposed to the VM. Included from shell.c. */
#include "shell/runner.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "ios/vproc.h"
#include "ios/vproc_tree.h"
#include <sys/types.h>
#include <sys/stat.h>
#if defined(PSCAL_TARGET_IOS)
#include "common/path_virtualization.h"
#include "common/path_truncate.h"
#include "smallclue/smallclue.h"
#include "ios/vproc.h"
static const char *shellResolvePathForIOS(const char *input, char *buffer, size_t buffer_len) {
    if (!input) {
        return NULL;
    }
    if (pathTruncateExpand(input, buffer, buffer_len)) {
        return buffer;
    }
    return input;
}
#endif
#if !defined(PSCAL_TAB_TITLE_SUPPORT)
#if defined(PSCAL_TARGET_IOS) || (defined(TARGET_OS_MACCATALYST) && TARGET_OS_MACCATALYST)
#define PSCAL_TAB_TITLE_SUPPORT 1
#endif
#endif
#if defined(PSCAL_TAB_TITLE_SUPPORT)
extern int PSCALRuntimeSetTabTitle(const char *title);
#endif
#include <pthread.h>
#include <sys/ioctl.h>
#include <setjmp.h>
#ifdef PSCAL_TARGET_IOS
#include <signal.h>
#include <dirent.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "common/pascal_state.h"
#include "clike/state.h"
#include "rea/state.h"
extern bool pscalRuntimeConsumeSigint(void);
extern char **environ;
extern void pscalShowGwinMessage(const char *message);
extern void pscalEditorDump(void);

typedef struct {
    struct sigaction sigint;
    struct sigaction sigquit;
    struct sigaction sigterm;
    struct sigaction sigtstp;
    struct sigaction sigabrt;
    struct sigaction sigsegv;
    sigset_t sigmask;
    bool have_sigint;
    bool have_sigquit;
    bool have_sigterm;
    bool have_sigtstp;
    bool have_sigabrt;
    bool have_sigsegv;
    bool have_sigmask;
} ShellSignalBackup;

typedef struct {
    int saved_fds[3];
    bool saved_valid[3];
} ShellStdFdBackup;

static bool shellToolTcgetattr(int fd, struct termios *termios) {
    if (!termios) {
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    if (vprocIoctlShim(fd, TIOCGETA, termios) == 0) {
        return true;
    }
#ifdef TCGETS
    if (vprocIoctlShim(fd, TCGETS, termios) == 0) {
        return true;
    }
#endif
#endif
    return tcgetattr(fd, termios) == 0;
}

static bool shellToolTcsetattr(int fd, int action, const struct termios *termios) {
    if (!termios) {
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    int cmd = 0;
#if defined(TCSETS)
    switch (action) {
        case TCSANOW:
            cmd = TCSETS;
            break;
        case TCSADRAIN:
            cmd = TCSETSW;
            break;
        case TCSAFLUSH:
            cmd = TCSETSF;
            break;
        default:
            cmd = 0;
            break;
    }
#elif defined(TIOCSETA)
    switch (action) {
        case TCSANOW:
            cmd = TIOCSETA;
            break;
        case TCSADRAIN:
            cmd = TIOCSETAW;
            break;
        case TCSAFLUSH:
            cmd = TIOCSETAF;
            break;
        default:
            cmd = 0;
            break;
    }
#endif
    if (cmd != 0 && vprocIoctlShim(fd, cmd, (void *)termios) == 0) {
        return true;
    }
#endif
    return tcsetattr(fd, action, termios) == 0;
}

static void shellSaveSignalState(ShellSignalBackup *backup) {
    if (!backup) {
        return;
    }
    memset(backup, 0, sizeof(*backup));
    backup->have_sigint = (sigaction(SIGINT, NULL, &backup->sigint) == 0);
    backup->have_sigquit = (sigaction(SIGQUIT, NULL, &backup->sigquit) == 0);
    backup->have_sigterm = (sigaction(SIGTERM, NULL, &backup->sigterm) == 0);
    backup->have_sigtstp = (sigaction(SIGTSTP, NULL, &backup->sigtstp) == 0);
    backup->have_sigabrt = (sigaction(SIGABRT, NULL, &backup->sigabrt) == 0);
    backup->have_sigsegv = (sigaction(SIGSEGV, NULL, &backup->sigsegv) == 0);
    backup->have_sigmask = (pthread_sigmask(SIG_SETMASK, NULL, &backup->sigmask) == 0);
}

static void shellRestoreSignalState(const ShellSignalBackup *backup) {
    if (!backup) {
        return;
    }
    if (backup->have_sigint) {
        sigaction(SIGINT, &backup->sigint, NULL);
    }
    if (backup->have_sigquit) {
        sigaction(SIGQUIT, &backup->sigquit, NULL);
    }
    if (backup->have_sigterm) {
        sigaction(SIGTERM, &backup->sigterm, NULL);
    }
    if (backup->have_sigtstp) {
        sigaction(SIGTSTP, &backup->sigtstp, NULL);
    }
    if (backup->have_sigabrt) {
        sigaction(SIGABRT, &backup->sigabrt, NULL);
    }
    if (backup->have_sigsegv) {
        sigaction(SIGSEGV, &backup->sigsegv, NULL);
    }
    if (backup->have_sigmask) {
        pthread_sigmask(SIG_SETMASK, &backup->sigmask, NULL);
    }
}

static void shellSaveStandardFds(ShellStdFdBackup *backup) {
    if (!backup) {
        return;
    }
    memset(backup, 0, sizeof(*backup));
    int targets[3] = { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };
    for (int i = 0; i < 3; ++i) {
        int fd = dup(targets[i]);
        if (fd >= 0) {
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            backup->saved_fds[i] = fd;
            backup->saved_valid[i] = true;
        } else {
            backup->saved_fds[i] = -1;
            backup->saved_valid[i] = false;
        }
    }
}

static void shellRestoreStandardFds(ShellStdFdBackup *backup, bool dbg) {
    if (!backup) {
        return;
    }
    int targets[3] = { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };
    for (int i = 0; i < 3; ++i) {
        if (!backup->saved_valid[i] || backup->saved_fds[i] < 0) {
            continue;
        }
        if (dup2(backup->saved_fds[i], targets[i]) == 0) {
            shellResetStandardStream(targets[i]);
        } else if (dbg) {
            fprintf(stderr,
                    "[tool-inline] restore fd=%d failed err=%s\n",
                    targets[i],
                    strerror(errno));
        }
        close(backup->saved_fds[i]);
        backup->saved_fds[i] = -1;
        backup->saved_valid[i] = false;
    }
}

static void shellInvalidateFrontendState(FrontendKind kind) {
    switch (kind) {
        case FRONTEND_KIND_PASCAL:
            pascalInvalidateGlobalState();
            break;
        case FRONTEND_KIND_CLIKE:
            clikeInvalidateGlobalState();
            break;
        case FRONTEND_KIND_REA:
            reaInvalidateGlobalState();
            break;
        case FRONTEND_KIND_SHELL:
            pascalInvalidateGlobalState();
            break;
        default:
            pascalInvalidateGlobalState();
            break;
    }
}
#endif

#if defined(PSCAL_TARGET_IOS)
#undef stdin
#undef stdout
#undef stderr
#define stdin shellVprocStdin()
#define stdout shellVprocStdout()
#define stderr shellVprocStderr()
#define printf(...) fprintf(stdout, __VA_ARGS__)
#define puts(text) shellVprocPuts((text))
#define putchar(ch) fputc((ch), stdout)
#define perror(label) shellVprocPerror((label))
#endif

static const char *shellTypesetArithmeticErrorFormat(void) {
    static int cached_style = -1;
    if (cached_style < 0) {
        cached_style = 0;
        const char *bash_path = shellSafeGetenv("BASH");
        if (!bash_path || bash_path[0] == '\0') {
            bash_path = "/bin/bash";
        }
        if (bash_path && bash_path[0] != '\0') {
            char quoted[768];
            size_t qlen = 0;
            quoted[qlen++] = '\'';
            const char *p = bash_path;
            for (; *p && qlen + 4 < sizeof(quoted); ++p) {
                if (*p == '\'') {
                    quoted[qlen++] = '\'';
                    quoted[qlen++] = '\\';
                    quoted[qlen++] = '\'';
                    quoted[qlen++] = '\'';
                } else {
                    quoted[qlen++] = *p;
                }
            }
            if (*p == '\0' && qlen + 2 < sizeof(quoted)) {
                quoted[qlen++] = '\'';
                quoted[qlen] = '\0';

                char command[1024];
                snprintf(command,
                         sizeof(command),
                         "%s --noprofile --norc -c 'typeset -i __pscaltmp=1+' 2>&1",
                         quoted);
                FILE *pipe = popen(command, "r");
                if (pipe) {
                    char buffer[256];
                    size_t length = fread(buffer, 1, sizeof(buffer) - 1, pipe);
                    buffer[length] = '\0';
                    if (strstr(buffer, "arithmetic syntax error") != NULL) {
                        cached_style = 1;
                    }
                    pclose(pipe);
                }
            }
        }
    }
    if (cached_style == 1) {
        return "%s: %s: arithmetic syntax error: operand expected (error token is \"%s\")";
    }
    return "%s: %s: syntax error: operand expected (error token is \"%s\")";
}
Value vmBuiltinShellExec(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
    shellCollectJobs();
    ShellCommand cmd;
    if (!shellBuildCommand(vm, arg_count, args, &cmd)) {
        goto cleanup;
    }
    result = shellExecuteCommand(vm, &cmd);

cleanup:
    shellRestoreCurrentVm(previous_vm);
    return result;
}

Value vmBuiltinShellPipeline(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
    char *merge_pattern = NULL;
    if (arg_count != 1 || args[0].type != TYPE_STRING || !args[0].s_val) {
        runtimeError(vm, "shell pipeline: expected metadata string");
        goto cleanup;
    }
    if (shellLoopSkipActive()) {
        goto cleanup;
    }
    const char *meta = args[0].s_val;
    size_t stages = 0;
    bool negated = false;
    char *copy = strdup(meta);
    if (!copy) {
        runtimeError(vm, "shell pipeline: out of memory");
        goto cleanup;
    }
    char *cursor = copy;
    while (cursor && *cursor) {
        char *next = strchr(cursor, ';');
        if (next) *next = '\0';
        char *eq = strchr(cursor, '=');
        if (eq) {
            *eq = '\0';
            const char *key = cursor;
            const char *value = eq + 1;
            if (strcmp(key, "stages") == 0) {
                stages = (size_t)strtoul(value, NULL, 10);
            } else if (strcmp(key, "negated") == 0) {
                shellParseBool(value, &negated);
            } else if (strcmp(key, "merge") == 0) {
                free(merge_pattern);
                merge_pattern = strdup(value ? value : "");
            }
        }
        if (!next) break;
        cursor = next + 1;
    }
        free(copy);
    bool skip_pipeline = false;
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (ctx->active && stages == 1 && !negated) {
        bool has_merge = false;
        if (merge_pattern) {
            for (const char *p = merge_pattern; *p; ++p) {
                if (*p != '0') {
                    has_merge = true;
                    break;
                }
            }
        }
        if (!has_merge) {
            skip_pipeline = true;
        }
    }

    if (!skip_pipeline) {
        if (stages == 0) {
            runtimeError(vm, "shell pipeline: invalid stage count");
            free(merge_pattern);
            goto cleanup;
        }
        if (!shellEnsurePipeline(stages, negated)) {
            runtimeError(vm, "shell pipeline: unable to allocate context");
            free(merge_pattern);
            goto cleanup;
        }

        if (merge_pattern) {
            size_t pattern_len = strlen(merge_pattern);
            for (size_t i = 0; i < stages; ++i) {
                bool merge = false;
                if (i < pattern_len) {
                    merge = (merge_pattern[i] == '1');
                }
                if (ctx->merge_stderr && i < stages) {
                    ctx->merge_stderr[i] = merge;
                }
            }
        }
    }

    free(merge_pattern);
    merge_pattern = NULL;

cleanup:
    shellRestoreCurrentVm(previous_vm);
    return result;
}

static void shellArithmeticCommandTrim(const char **start, const char **end) {
    if (!start || !end) {
        return;
    }
    const char *s = *start;
    const char *e = *end;
    while (s < e && isspace((unsigned char)*s)) {
        s++;
    }
    while (e > s && isspace((unsigned char)e[-1])) {
        e--;
    }
    *start = s;
    *end = e;
}

static const char *shellArithmeticCommandParseName(const char *start, const char *end, char **out_name) {
    if (!start || !end || start >= end) {
        return NULL;
    }
    const char *cursor = start;
    while (cursor < end && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (cursor >= end || (!isalpha((unsigned char)*cursor) && *cursor != '_')) {
        return NULL;
    }
    const char *name_start = cursor;
    cursor++;
    while (cursor < end && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    size_t len = (size_t)(cursor - name_start);
    char *name = (char *)malloc(len + 1);
    if (!name) {
        return NULL;
    }
    memcpy(name, name_start, len);
    name[len] = '\0';
    while (cursor < end && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (out_name) {
        *out_name = name;
    } else {
        free(name);
    }
    return cursor;
}

static bool shellArithmeticCommandGetValue(const char *name, long long *out_value) {
    if (!name || !out_value) {
        return false;
    }
    char *raw = shellLookupParameterValue(name, strlen(name));
    long long value = 0;
    bool ok = shellArithmeticParseValueString(raw ? raw : "0", &value);
    free(raw);
    if (!ok) {
        return false;
    }
    *out_value = value;
    return true;
}

static bool shellArithmeticCommandSetValue(const char *name, long long value) {
    if (!name) {
        return false;
    }
    char buffer[64];
    int written = snprintf(buffer, sizeof(buffer), "%lld", value);
    if (written < 0 || written >= (int)sizeof(buffer)) {
        return false;
    }
    return shellSetTrackedVariable(name, buffer, false);
}

static bool shellArithmeticCommandEvalSimple(const char *start, const char *end, long long *out_value) {
    if (!start || !end || start >= end) {
        return false;
    }
    size_t len = (size_t)(end - start);
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        return false;
    }
    memcpy(copy, start, len);
    copy[len] = '\0';
    bool eval_error = false;
    char *result = shellEvaluateArithmetic(copy, &eval_error);
        free(copy);
    if (!result || eval_error) {
        free(result);
        return false;
    }
    long long value = 0;
    bool ok = shellArithmeticParseValueString(result, &value);
    free(result);
    if (!ok) {
        return false;
    }
    if (out_value) {
        *out_value = value;
    }
    return true;
}

static bool shellArithmeticCommandEvaluateExpression(const char *expr, long long *out_value) {
    if (!expr) {
        return false;
    }
    const char *start = expr;
    const char *end = expr + strlen(expr);
    shellArithmeticCommandTrim(&start, &end);
    if (start >= end) {
        return false;
    }

    if (end - start >= 2 && start[0] == '+' && start[1] == '+') {
        const char *cursor = start + 2;
        while (cursor < end && isspace((unsigned char)*cursor)) {
            cursor++;
        }
        char *name = NULL;
        const char *rest = shellArithmeticCommandParseName(cursor, end, &name);
        if (!rest || !name) {
            free(name);
            return false;
        }
        const char *after = rest;
        while (after < end && isspace((unsigned char)*after)) {
            after++;
        }
        if (after != end) {
            free(name);
            goto fallback;
        }
        if (!shellIsValidEnvName(name)) {
            free(name);
            return false;
        }
        long long value = 0;
        if (!shellArithmeticCommandGetValue(name, &value)) {
            free(name);
            return false;
        }
        value += 1;
        if (!shellArithmeticCommandSetValue(name, value)) {
            free(name);
            return false;
        }
        if (out_value) {
            *out_value = value;
        }
        free(name);
        return true;
    }
    if (end - start >= 2 && start[0] == '-' && start[1] == '-') {
        const char *cursor = start + 2;
        while (cursor < end && isspace((unsigned char)*cursor)) {
            cursor++;
        }
        char *name = NULL;
        const char *rest = shellArithmeticCommandParseName(cursor, end, &name);
        if (!rest || !name) {
            free(name);
            return false;
        }
        const char *after = rest;
        while (after < end && isspace((unsigned char)*after)) {
            after++;
        }
        if (after != end) {
            free(name);
            goto fallback;
        }
        if (!shellIsValidEnvName(name)) {
            free(name);
            return false;
        }
        long long value = 0;
        if (!shellArithmeticCommandGetValue(name, &value)) {
            free(name);
            return false;
        }
        value -= 1;
        if (!shellArithmeticCommandSetValue(name, value)) {
            free(name);
            return false;
        }
        if (out_value) {
            *out_value = value;
        }
        free(name);
        return true;
    }

    char *name = NULL;
    const char *rest = shellArithmeticCommandParseName(start, end, &name);
    if (rest && name) {
        if (!shellIsValidEnvName(name)) {
            free(name);
            return false;
        }
        const char *cursor = rest;
        while (cursor < end && isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (end - cursor >= 2 && cursor[0] == '+' && cursor[1] == '+') {
            const char *after_op = cursor + 2;
            while (after_op < end && isspace((unsigned char)*after_op)) {
                after_op++;
            }
            if (after_op != end) {
                free(name);
                goto fallback;
            }
            long long value = 0;
            if (!shellArithmeticCommandGetValue(name, &value)) {
                free(name);
                return false;
            }
            long long original = value;
            value += 1;
            if (!shellArithmeticCommandSetValue(name, value)) {
                free(name);
                return false;
            }
            if (out_value) {
                *out_value = original;
            }
            free(name);
            return true;
        }
        if (end - cursor >= 2 && cursor[0] == '-' && cursor[1] == '-') {
            const char *after_op = cursor + 2;
            while (after_op < end && isspace((unsigned char)*after_op)) {
                after_op++;
            }
            if (after_op != end) {
                free(name);
                goto fallback;
            }
            long long value = 0;
            if (!shellArithmeticCommandGetValue(name, &value)) {
                free(name);
                return false;
            }
            long long original = value;
            value -= 1;
            if (!shellArithmeticCommandSetValue(name, value)) {
                free(name);
                return false;
            }
            if (out_value) {
                *out_value = original;
            }
            free(name);
            return true;
        }

        char assign_op = '\0';
        if (cursor < end) {
            if (cursor + 1 < end && (cursor[0] == '+' || cursor[0] == '-' || cursor[0] == '*' || cursor[0] == '/' || cursor[0] == '%') &&
                cursor[1] == '=') {
                assign_op = cursor[0];
                cursor += 2;
            } else if (*cursor == '=') {
                assign_op = '=';
                cursor++;
            }
        }

        if (assign_op != '\0') {
            const char *rhs_start = cursor;
            const char *rhs_end = end;
            shellArithmeticCommandTrim(&rhs_start, &rhs_end);
            if (rhs_start >= rhs_end) {
                free(name);
                return false;
            }
            if ((assign_op == '/' || assign_op == '%') && rhs_end > rhs_start) {
                // avoid division by zero by checking after evaluation
            }
            long long rhs_value = 0;
            if (!shellArithmeticCommandEvalSimple(rhs_start, rhs_end, &rhs_value)) {
                free(name);
                return false;
            }
            if ((assign_op == '/' || assign_op == '%') && rhs_value == 0) {
                free(name);
                return false;
            }
            long long result = rhs_value;
            if (assign_op != '=') {
                long long current = 0;
                if (!shellArithmeticCommandGetValue(name, &current)) {
                    free(name);
                    return false;
                }
                switch (assign_op) {
                    case '+': result = current + rhs_value; break;
                    case '-': result = current - rhs_value; break;
                    case '*': result = current * rhs_value; break;
                    case '/': result = current / rhs_value; break;
                    case '%': result = current % rhs_value; break;
                    default:
                        free(name);
                        return false;
                }
            }
            if (!shellArithmeticCommandSetValue(name, result)) {
                free(name);
                return false;
            }
            if (out_value) {
                *out_value = result;
            }
            free(name);
            return true;
        }

        if (cursor == end) {
            long long value = 0;
            if (!shellArithmeticCommandGetValue(name, &value)) {
                free(name);
                return false;
            }
            if (out_value) {
                *out_value = value;
            }
            free(name);
            return true;
        }

        free(name);
    } else if (name) {
        free(name);
    }

fallback: {
    long long value = 0;
    if (!shellArithmeticCommandEvalSimple(start, end, &value)) {
        return false;
    }
    if (out_value) {
        *out_value = value;
    }
    return true;
}
}

Value vmBuiltinShellArithmetic(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();

    bool dispatch_mode = (arg_count >= 2 && args[0].type == TYPE_STRING);

    if (dispatch_mode) {
        shellCollectJobs();
        ShellCommand cmd;
        memset(&cmd, 0, sizeof(cmd));

        const char *meta = args[0].s_val;
        if (!meta) {
            runtimeError(vm, "((: expected metadata");
            shellUpdateStatus(1);
            goto dispatch_cleanup;
        }

        shellParseMetadata(meta, &cmd);

        if (args[1].type != TYPE_STRING) {
            runtimeError(vm, "((: expected expression");
            shellUpdateStatus(1);
            goto dispatch_cleanup;
        }

        const char *expression = args[1].s_val ? args[1].s_val : "";

        char *name_copy = strdup("__shell_arithmetic");
        if (!name_copy) {
            runtimeError(vm, "((: out of memory");
            shellUpdateStatus(1);
            goto dispatch_cleanup;
        }
        if (!shellCommandAppendArgOwned(&cmd, name_copy)) {
            runtimeError(vm, "((: unable to build command");
            shellUpdateStatus(1);
            goto dispatch_cleanup;
        }

        char *expr_copy = strdup(expression);
        if (!expr_copy) {
            runtimeError(vm, "((: out of memory");
            shellUpdateStatus(1);
            goto dispatch_cleanup;
        }
        if (!shellCommandAppendArgOwned(&cmd, expr_copy)) {
            runtimeError(vm, "((: unable to build command");
            shellUpdateStatus(1);
            goto dispatch_cleanup;
        }

        for (int i = 2; i < arg_count; ++i) {
            Value entry = args[i];
            if (entry.type != TYPE_STRING || !entry.s_val) {
                runtimeError(vm, "((: invalid redirection");
                shellUpdateStatus(1);
                goto dispatch_cleanup;
            }
            if (!shellAddRedirection(&cmd, entry.s_val)) {
                runtimeError(vm, "((: invalid redirection");
                shellUpdateStatus(1);
                goto dispatch_cleanup;
            }
        }

        result = shellExecuteCommand(vm, &cmd);
        goto cleanup;

dispatch_cleanup:
        shellFreeCommand(&cmd);
        goto cleanup;
    }

    if (arg_count < 1) {
        runtimeError(vm, "((: expected expression");
        shellUpdateStatus(1);
        goto cleanup;
    }

    if (args[0].type != TYPE_STRING) {
        runtimeError(vm, "((: expected expression");
        shellUpdateStatus(1);
        goto cleanup;
    }

    const char *expression = args[0].s_val ? args[0].s_val : "";

    long long expr_value = 0;
    if (!shellArithmeticCommandEvaluateExpression(expression, &expr_value)) {
        fprintf(stderr, "((: %s: arithmetic syntax error\n",
                (expression && *expression) ? expression : "");
        shellMarkArithmeticError();
        shellUpdateStatus(1);
        goto cleanup;
    }

    shellUpdateStatus(expr_value != 0 ? 0 : 1);

cleanup:
    shellRestoreCurrentVm(previous_vm);
    return result;
}

Value vmBuiltinShellAnd(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    if (gShellRuntime.last_status != 0) {
        shellUpdateStatus(gShellRuntime.last_status);
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

Value vmBuiltinShellOr(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    if (gShellRuntime.last_status == 0) {
        shellUpdateStatus(0);
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

typedef struct {
    ShellExecRedirBackup *backups;
    size_t backup_count;
} ShellSubshellFrame;

static ShellSubshellFrame *gShellSubshellStack = NULL;
static size_t gShellSubshellStackSize = 0;
static size_t gShellSubshellStackCapacity = 0;

static bool shellSubshellStackPush(ShellExecRedirBackup *backups, size_t backup_count) {
    if (gShellSubshellStackSize >= gShellSubshellStackCapacity) {
        size_t new_cap = (gShellSubshellStackCapacity == 0) ? 4 : (gShellSubshellStackCapacity * 2);
        ShellSubshellFrame *resized = (ShellSubshellFrame *)realloc(gShellSubshellStack, new_cap * sizeof(ShellSubshellFrame));
        if (!resized) {
            return false;
        }
        gShellSubshellStack = resized;
        gShellSubshellStackCapacity = new_cap;
    }
    ShellSubshellFrame *slot = &gShellSubshellStack[gShellSubshellStackSize++];
    slot->backups = backups;
    slot->backup_count = backup_count;
    return true;
}

static ShellSubshellFrame shellSubshellStackPop(void) {
    ShellSubshellFrame frame = {0};
    if (gShellSubshellStackSize == 0) {
        return frame;
    }
    frame = gShellSubshellStack[--gShellSubshellStackSize];
    return frame;
}

Value vmBuiltinShellSubshell(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    const char *meta = (arg_count > 0 && args[0].type == TYPE_STRING) ? args[0].s_val : "";
    bool leave = (meta && strstr(meta, "mode=leave") != NULL);
    if (leave) {
        ShellSubshellFrame frame = shellSubshellStackPop();
        if (frame.backups) {
            shellRestoreExecRedirections(frame.backups, frame.backup_count);
            shellFreeExecRedirBackups(frame.backups, frame.backup_count);
        }
        shellRestoreCurrentVm(previous_vm);
        return makeVoid();
    }

    ShellCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    bool ok = true;
    ShellExecRedirBackup *backups = NULL;
    size_t backup_count = 0;
    for (int i = 1; i < arg_count; ++i) {
        Value entry = args[i];
        if (entry.type != TYPE_STRING || !entry.s_val) {
            ok = false;
            break;
        }
        if (!shellAddRedirection(&cmd, entry.s_val)) {
            ok = false;
            break;
        }
    }
    if (ok && cmd.redir_count > 0) {
        if (!shellApplyExecRedirections(vm ? vm : gShellCurrentVm, &cmd, &backups, &backup_count)) {
            ok = false;
        }
    }
    if (cmd.redirs) {
        shellFreeRedirections(&cmd);
        cmd.redirs = NULL;
        cmd.redir_count = 0;
    }
    if (ok && (backups || backup_count == 0)) {
        if (!shellSubshellStackPush(backups, backup_count)) {
            if (backups) {
                shellRestoreExecRedirections(backups, backup_count);
                shellFreeExecRedirBackups(backups, backup_count);
            }
            ok = false;
        }
    } else if (backups) {
        shellRestoreExecRedirections(backups, backup_count);
        shellFreeExecRedirBackups(backups, backup_count);
    }
    if (!ok) {
        runtimeError(vm, "subshell: failed to apply redirections");
        shellUpdateStatus(1);
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

static bool shellLoopRedirectionTargetsFd(const ShellLoopFrame *frame, int fd) {
    if (!frame || fd < 0) {
        return false;
    }
    if (!frame->applied_redirs || frame->applied_redir_count == 0) {
        return false;
    }
    for (size_t i = 0; i < frame->applied_redir_count; ++i) {
        const ShellRedirection *redir = &frame->applied_redirs[i];
        if (!redir) {
            continue;
        }
        if (redir->fd == fd) {
            return true;
        }
    }
    return false;
}

static bool shellLoopAdoptPipeline(VM *vm,
                                   ShellLoopFrame *frame,
                                   int pipeline_index,
                                   bool pipeline_head,
                                   bool pipeline_tail) {
    if (!frame) {
        return true;
    }
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (!ctx->active || pipeline_index < 0) {
        return true;
    }
    size_t stages = ctx->stage_count;
    if ((size_t)pipeline_index >= stages) {
        runtimeError(vm, "shell loop: pipeline index out of range");
        return false;
    }

    bool merge_stderr =
        (ctx->merge_stderr && (size_t)pipeline_index < stages) ? ctx->merge_stderr[pipeline_index] : false;
    int stdin_fd = -1;
    int stdout_fd = -1;
    if (!pipeline_head && ctx->pipes && pipeline_index > 0) {
        stdin_fd = ctx->pipes[pipeline_index - 1][0];
    }
    if (!pipeline_tail && ctx->pipes) {
        stdout_fd = ctx->pipes[pipeline_index][1];
    }

    if (stdin_fd >= 0) {
        if (shellLoopRedirectionTargetsFd(frame, STDIN_FILENO)) {
            close(stdin_fd);
            ctx->pipes[pipeline_index - 1][0] = -1;
        } else {
            if (!shellEnsureExecRedirBackup(STDIN_FILENO,
                                            -1,
                                            &frame->redir_backups,
                                            &frame->redir_backup_count,
                                            &frame->redir_backup_capacity)) {
                runtimeError(vm, "shell loop: failed to prepare stdin backup");
                close(stdin_fd);
                ctx->pipes[pipeline_index - 1][0] = -1;
                return false;
            }
           if (dup2(stdin_fd, STDIN_FILENO) == -1) {
                int err = errno;
                runtimeError(vm, "shell loop: failed to adopt pipeline stdin: %s", strerror(err));
                shellRestoreExecRedirections(frame->redir_backups, frame->redir_backup_count);
                shellFreeExecRedirBackups(frame->redir_backups, frame->redir_backup_count);
                frame->redir_backups = NULL;
                frame->redir_backup_count = 0;
                frame->redir_backup_capacity = 0;
                frame->redirs_active = false;
                close(stdin_fd);
                ctx->pipes[pipeline_index - 1][0] = -1;
                return false;
            }
            close(stdin_fd);
            ctx->pipes[pipeline_index - 1][0] = -1;
            frame->redirs_active = true;
        }
    }

    if (stdout_fd >= 0) {
        if (shellLoopRedirectionTargetsFd(frame, STDOUT_FILENO)) {
            close(stdout_fd);
            ctx->pipes[pipeline_index][1] = -1;
        } else {
            if (!shellEnsureExecRedirBackup(STDOUT_FILENO,
                                            -1,
                                            &frame->redir_backups,
                                            &frame->redir_backup_count,
                                            &frame->redir_backup_capacity)) {
                runtimeError(vm, "shell loop: failed to prepare stdout backup");
                close(stdout_fd);
                ctx->pipes[pipeline_index][1] = -1;
                return false;
            }
            if (dup2(stdout_fd, STDOUT_FILENO) == -1) {
                int err = errno;
                runtimeError(vm, "shell loop: failed to adopt pipeline stdout: %s", strerror(err));
                shellRestoreExecRedirections(frame->redir_backups, frame->redir_backup_count);
                shellFreeExecRedirBackups(frame->redir_backups, frame->redir_backup_count);
                frame->redir_backups = NULL;
                frame->redir_backup_count = 0;
                frame->redir_backup_capacity = 0;
                frame->redirs_active = false;
                close(stdout_fd);
                ctx->pipes[pipeline_index][1] = -1;
                return false;
            }
            close(stdout_fd);
            ctx->pipes[pipeline_index][1] = -1;
            frame->redirs_active = true;
        }
    }

    if (merge_stderr && !shellLoopRedirectionTargetsFd(frame, STDERR_FILENO)) {
        if (!shellEnsureExecRedirBackup(STDERR_FILENO,
                                        STDOUT_FILENO,
                                        &frame->redir_backups,
                                        &frame->redir_backup_count,
                                        &frame->redir_backup_capacity)) {
            runtimeError(vm, "shell loop: failed to prepare stderr backup");
            return false;
        }
        if (dup2(STDOUT_FILENO, STDERR_FILENO) == -1) {
            int err = errno;
            runtimeError(vm, "shell loop: failed to adopt pipeline stderr: %s", strerror(err));
            shellRestoreExecRedirections(frame->redir_backups, frame->redir_backup_count);
            shellFreeExecRedirBackups(frame->redir_backups, frame->redir_backup_count);
            frame->redir_backups = NULL;
            frame->redir_backup_count = 0;
            frame->redir_backup_capacity = 0;
            frame->redirs_active = false;
            return false;
        }
        frame->redirs_active = true;
    }

    return true;
}

Value vmBuiltinShellLoop(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
    const char *meta = NULL;
    if (arg_count > 0 && args[0].type == TYPE_STRING && args[0].s_val) {
        meta = args[0].s_val;
    }

    ShellLoopKind kind = SHELL_LOOP_KIND_WHILE;
    bool until_flag = false;
    size_t redir_count = 0;
    int condition_kind_value = 0;
    size_t condition_payload_count = 0;
    int body_kind_value = 0;
    size_t body_payload_count = 0;
    int pipeline_index_value = -1;
    bool pipeline_head_value = false;
    bool pipeline_tail_value = false;
    if (meta && *meta) {
        char *copy = strdup(meta);
        if (copy) {
            for (char *token = strtok(copy, ";"); token; token = strtok(NULL, ";")) {
                char *eq = strchr(token, '=');
                if (eq) {
                    *eq = '\0';
                    const char *key = token;
                    const char *value = eq + 1;
                    if (strcmp(key, "mode") == 0) {
                        if (strcasecmp(value, "for") == 0) {
                            kind = SHELL_LOOP_KIND_FOR;
                        } else if (strcasecmp(value, "cfor") == 0) {
                            kind = SHELL_LOOP_KIND_CFOR;
                        } else if (strcasecmp(value, "until") == 0) {
                            kind = SHELL_LOOP_KIND_UNTIL;
                        } else if (strcasecmp(value, "while") == 0) {
                            kind = SHELL_LOOP_KIND_WHILE;
                        }
                    } else if (strcmp(key, "until") == 0) {
                        until_flag = (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0);
                    } else if (strcmp(key, "redirs") == 0) {
                        char *endptr = NULL;
                        unsigned long parsed = strtoul(value ? value : "0", &endptr, 10);
                        if (endptr && *endptr == '\0') {
                            redir_count = (size_t)parsed;
                        }
                    } else if (strcmp(key, "condkind") == 0) {
                        condition_kind_value = (int)strtol(value ? value : "0", NULL, 10);
                    } else if (strcmp(key, "condwords") == 0) {
                        condition_payload_count = (size_t)strtoul(value ? value : "0", NULL, 10);
                    } else if (strcmp(key, "bodykind") == 0) {
                        body_kind_value = (int)strtol(value ? value : "0", NULL, 10);
                    } else if (strcmp(key, "bodywords") == 0) {
                        body_payload_count = (size_t)strtoul(value ? value : "0", NULL, 10);
                    } else if (strcmp(key, "pipe") == 0) {
                        pipeline_index_value = (int)strtol(value ? value : "-1", NULL, 10);
                    } else if (strcmp(key, "head") == 0) {
                        shellParseBool(value, &pipeline_head_value);
                    } else if (strcmp(key, "tail") == 0) {
                        shellParseBool(value, &pipeline_tail_value);
                    }
                }
            }
            free(copy);
        }
    }
    if (kind != SHELL_LOOP_KIND_FOR && until_flag) {
        kind = SHELL_LOOP_KIND_UNTIL;
    }

    ShellPipelineContext *pipeline_ctx = &gShellRuntime.pipeline;
    bool pipeline_active = pipeline_ctx->active && pipeline_index_value >= 0 && pipeline_ctx->stage_count > 1;

    bool parent_skip = shellLoopSkipActive();
    ShellLoopFrame *frame = shellLoopPushFrame(kind);
    if (!frame) {
        runtimeError(vm, "shell loop: out of memory");
        shellRestoreCurrentVm(previous_vm);
        shellUpdateStatus(1);
        return result;
    }

    bool previous_pipeline_flag = gShellRuntime.pipeline_subshell_active;
    bool previous_local_scope_flag = gShellRuntime.local_scope_active;
    bool pipeline_scope_pushed = false;
    if (pipeline_active) {
        if (!shellLocalScopePush()) {
            runtimeError(vm, "shell loop: out of memory");
            shellLoopPopFrame();
            shellRestoreCurrentVm(previous_vm);
            shellUpdateStatus(1);
            return result;
        }
        pipeline_scope_pushed = true;
        gShellRuntime.local_scope_active = true;
        gShellRuntime.pipeline_subshell_active = true;
        frame->pipeline_prev_local_scope_flag = previous_local_scope_flag;
        frame->pipeline_prev_pipeline_flag = previous_pipeline_flag;
        frame->pipeline_scope_pushed = true;
        frame->pipeline_subshell = true;
    } else {
        frame->pipeline_prev_local_scope_flag = previous_local_scope_flag;
        frame->pipeline_prev_pipeline_flag = previous_pipeline_flag;
        frame->pipeline_scope_pushed = false;
        frame->pipeline_subshell = false;
    }
    frame->skip_body = parent_skip;
    frame->break_pending = false;
    frame->continue_pending = false;
    if (condition_kind_value < SHELL_LOOP_CONDITION_NONE ||
        condition_kind_value > SHELL_LOOP_CONDITION_ARITH) {
        condition_kind_value = SHELL_LOOP_CONDITION_NONE;
    }
    frame->cond_kind = (ShellLoopConditionKind)condition_kind_value;
    if ((frame->cond_kind == SHELL_LOOP_CONDITION_TEST ||
         frame->cond_kind == SHELL_LOOP_CONDITION_BRACKET) &&
        condition_payload_count == 0) {
        frame->cond_kind = SHELL_LOOP_CONDITION_NONE;
    }
    if (frame->cond_kind == SHELL_LOOP_CONDITION_ARITH && condition_payload_count == 0) {
        frame->cond_kind = SHELL_LOOP_CONDITION_NONE;
    }

    if (body_kind_value < SHELL_LOOP_BODY_KIND_NONE ||
        body_kind_value > SHELL_LOOP_BODY_KIND_BRACKET_ARITH) {
        body_kind_value = SHELL_LOOP_BODY_KIND_NONE;
    }
    frame->body_kind = (ShellLoopBodyKind)body_kind_value;
    size_t body_expression_count = 0;
    switch (frame->body_kind) {
        case SHELL_LOOP_BODY_KIND_ARITH:
            body_expression_count = 1;
            break;
        case SHELL_LOOP_BODY_KIND_TEST:
        case SHELL_LOOP_BODY_KIND_BRACKET:
            if (body_payload_count == 0) {
                frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
            }
            break;
        case SHELL_LOOP_BODY_KIND_TEST_ARITH:
        case SHELL_LOOP_BODY_KIND_BRACKET_ARITH:
            if (body_payload_count == 0) {
                frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
            } else {
                body_expression_count = 1;
            }
            break;
        default:
            break;
    }
    if (frame->body_kind == SHELL_LOOP_BODY_KIND_NONE) {
        body_payload_count = 0;
        body_expression_count = 0;
    }

    ShellCommand redir_cmd;
    memset(&redir_cmd, 0, sizeof(redir_cmd));
    ShellExecRedirBackup *redir_backups = NULL;
    size_t redir_backup_count = 0;

    bool ok = true;

    int payload_total = (arg_count > 0) ? arg_count - 1 : 0;
    size_t meta_payload = condition_payload_count + body_payload_count + body_expression_count;
    if ((size_t)payload_total < redir_count + meta_payload) {
        runtimeError(vm, "shell loop: redirection metadata mismatch");
        ok = false;
        if ((size_t)payload_total >= meta_payload) {
            redir_count = (size_t)payload_total - meta_payload;
        } else {
            redir_count = 0;
        }
    }
    int payload_without_redirs = payload_total - (int)redir_count - (int)meta_payload;
    if (payload_without_redirs < 0) {
        payload_without_redirs = 0;
    }
    int payload_cursor = 1;
    int condition_start_index = payload_cursor;
    payload_cursor += (int)condition_payload_count;
    int body_start_index = payload_cursor;
    payload_cursor += (int)body_payload_count;
    int body_expression_index = (body_expression_count > 0) ? payload_cursor : -1;
    payload_cursor += (int)body_expression_count;
    int redir_start_index = arg_count - (int)redir_count;
    if (redir_start_index < payload_cursor) {
        redir_start_index = payload_cursor;
    }

    if (kind == SHELL_LOOP_KIND_FOR) {
        if (payload_without_redirs < 1 || payload_cursor >= redir_start_index ||
            args[payload_cursor].type != TYPE_STRING || !args[payload_cursor].s_val) {
            runtimeError(vm, "shell loop: expected iterator name");
            ok = false;
        } else {
            const char *spec = args[payload_cursor].s_val;
            const char *text = spec;
            const char *word_meta = NULL;
            size_t word_meta_len = 0;
            uint8_t word_flags = 0;
            if (!shellDecodeWordSpec(spec, &text, &word_flags, &word_meta, &word_meta_len)) {
                text = spec ? spec : "";
            }
            frame->for_variable = strdup(text ? text : "");
            if (!frame->for_variable) {
                ok = false;
            }
        }

        payload_cursor++;

        size_t value_start_index = (size_t)payload_cursor;
        size_t value_count = 0;
        if (redir_start_index > (int)value_start_index) {
            value_count = (size_t)(redir_start_index - (int)value_start_index);
        }
        size_t values_capacity = 0;
        if (ok && value_count > 0) {
            for (size_t offset = 0; offset < value_count; ++offset) {
                Value *val = &args[value_start_index + offset];
                if (val->type != TYPE_STRING || !val->s_val) {
                    char *empty = strdup("");
                    if (!empty || !shellLoopFrameAppendValue(frame, &values_capacity, empty)) {
                        free(empty);
                        ok = false;
                        break;
                    }
                    continue;
                }
                const char *spec = val->s_val;
                const char *text = spec;
                const char *word_meta = NULL;
                size_t word_meta_len = 0;
                uint8_t word_flags = 0;
                if (!shellDecodeWordSpec(spec, &text, &word_flags, &word_meta, &word_meta_len)) {
                    text = spec ? spec : "";
                    word_flags = 0;
                }
                bool *quoted_map = NULL;
                size_t quoted_len = 0;
                bool zero_array = false;
                char *expanded = shellExpandWord(text,
                                                 word_flags,
                                                 word_meta,
                                                 word_meta_len,
                                                 &quoted_map,
                                                 &quoted_len,
                                                 &zero_array);
                if (!expanded) {
                    ok = false;
                    break;
                }
                char **fields = NULL;
                size_t field_count = 0;
                if (!shellSplitExpandedWord(expanded,
                                            word_flags,
                                            quoted_map,
                                            quoted_len,
                                            zero_array,
                                            &fields,
                                            &field_count)) {
                    free(expanded);
                    free(quoted_map);
                    ok = false;
                    break;
                }
                if (field_count == 0) {
                    free(expanded);
                    free(quoted_map);
                    shellFreeStringArray(fields, field_count);
                    continue;
                }
                free(expanded);
                free(quoted_map);
                for (size_t f = 0; f < field_count; ++f) {
                    char *field_value = fields[f];
                    if (!field_value) {
                        continue;
                    }
                    if (shellWordShouldGlob(word_flags, field_value)) {
                        glob_t glob_result;
                        const char *glob_pattern = field_value;
#if defined(PSCAL_TARGET_IOS)
                        char expanded_pattern[PATH_MAX];
                        bool glob_used_virtual = false;
                        if (field_value[0] == '/' &&
                            pathTruncateEnabled() &&
                            pathTruncateExpand(field_value, expanded_pattern, sizeof(expanded_pattern))) {
                            glob_pattern = expanded_pattern;
                            glob_used_virtual = true;
                        }
#endif
                        int glob_status = glob(glob_pattern, 0, NULL, &glob_result);
                        if (glob_status == 0) {
#if defined(PSCAL_TARGET_IOS)
                            if (glob_used_virtual) {
                                for (size_t g = 0; g < glob_result.gl_pathc; ++g) {
                                    char *match = glob_result.gl_pathv[g];
                                    if (!match) {
                                        continue;
                                    }
                                    char stripped[PATH_MAX];
                                    if (pathTruncateStrip(match, stripped, sizeof(stripped))) {
                                        char *copy = strdup(stripped);
                                        if (copy) {
                                            free(glob_result.gl_pathv[g]);
                                            glob_result.gl_pathv[g] = copy;
                                        }
                                    }
                                }
                            }
#endif
                            bool glob_ok = true;
                            for (size_t g = 0; g < glob_result.gl_pathc; ++g) {
                                char *dup = strdup(glob_result.gl_pathv[g]);
                                if (!dup || !shellLoopFrameAppendValue(frame, &values_capacity, dup)) {
                                    if (dup) {
                                        free(dup);
                                    }
                                    glob_ok = false;
                                    break;
                                }
                            }
                            globfree(&glob_result);
                            free(field_value);
                            fields[f] = NULL;
                            if (!glob_ok) {
                                ok = false;
                                break;
                            }
                        } else {
                            if (glob_status != GLOB_NOMATCH) {
                                fprintf(stderr, "exsh: glob failed for '%s'\n", field_value);
                            }
                        }
                    }
                    if (!ok) {
                        break;
                    }
                    if (field_value) {
                        if (!shellLoopFrameAppendValue(frame, &values_capacity, field_value)) {
                            ok = false;
                            fields[f] = NULL;
                            break;
                        }
                        fields[f] = NULL;
                    }
                }
                shellFreeStringArray(fields, field_count);
                if (!ok) {
                    break;
                }
            }
        }

        if (ok && frame->for_count == 0 && gParamCount > 0 && gParamValues) {
            for (int i = 0; i < gParamCount; ++i) {
                const char *param = gParamValues[i] ? gParamValues[i] : "";
                char *dup = strdup(param);
                if (!dup || !shellLoopFrameAppendValue(frame, &values_capacity, dup)) {
                    if (dup) {
                        free(dup);
                    }
                    ok = false;
                    break;
                }
            }
        }

        if (!ok || !frame->for_variable) {
            frame->skip_body = true;
            frame->break_pending = true;
        } else if (frame->for_count == 0) {
            frame->skip_body = true;
            frame->for_active = false;
        } else {
            if (!shellAssignLoopVariable(frame->for_variable, frame->for_values[0])) {
                runtimeError(vm, "shell loop: failed to assign '%s'", frame->for_variable);
                frame->skip_body = true;
                frame->break_pending = true;
                ok = false;
            } else {
                frame->for_index = 1;
                frame->for_active = true;
            }
        }
    } else if (kind == SHELL_LOOP_KIND_CFOR) {
        if (payload_without_redirs < 3) {
            runtimeError(vm, "shell loop: expected initializer, condition, update");
            ok = false;
        } else {
            if (payload_cursor + 2 >= redir_start_index) {
                runtimeError(vm, "shell loop: expected initializer, condition, update");
                ok = false;
            } else {
                const Value *init_val = &args[payload_cursor];
                const Value *cond_val = &args[payload_cursor + 1];
                const Value *update_val = &args[payload_cursor + 2];
                const char *init_text = (init_val->type == TYPE_STRING && init_val->s_val) ? init_val->s_val : "";
                const char *cond_text = (cond_val->type == TYPE_STRING && cond_val->s_val) ? cond_val->s_val : "";
                const char *update_text = (update_val->type == TYPE_STRING && update_val->s_val) ? update_val->s_val : "";
                frame->cfor_init = strdup(init_text);
                frame->cfor_condition = strdup(cond_text);
                frame->cfor_update = strdup(update_text);
                if (!frame->cfor_init || !frame->cfor_condition || !frame->cfor_update) {
                    ok = false;
                } else if (!shellLoopExecuteCForInitializer(frame)) {
                    ok = false;
                }
            }
        }
    } else {
        if (ok && condition_payload_count > 0) {
            if ((size_t)arg_count < condition_start_index + condition_payload_count + redir_count) {
                ok = false;
                frame->cond_kind = SHELL_LOOP_CONDITION_NONE;
            } else {
                if (frame->cond_kind == SHELL_LOOP_CONDITION_ARITH) {
                    const Value *entry = &args[condition_start_index];
                    const char *expr = (entry->type == TYPE_STRING && entry->s_val) ? entry->s_val : "";
                    frame->cond_arith_expression = strdup(expr);
                    if (!frame->cond_arith_expression) {
                        ok = false;
                        frame->cond_kind = SHELL_LOOP_CONDITION_NONE;
                    }
                } else if (frame->cond_kind == SHELL_LOOP_CONDITION_TEST ||
                           frame->cond_kind == SHELL_LOOP_CONDITION_BRACKET) {
                    frame->cond_words = (char **)calloc(condition_payload_count, sizeof(char *));
                    if (!frame->cond_words) {
                        ok = false;
                        frame->cond_kind = SHELL_LOOP_CONDITION_NONE;
                    } else {
                        for (size_t i = 0; i < condition_payload_count; ++i) {
                            const Value *entry = &args[condition_start_index + i];
                            const char *spec = (entry->type == TYPE_STRING && entry->s_val) ? entry->s_val : "";
                            frame->cond_words[i] = strdup(spec);
                            if (!frame->cond_words[i]) {
                                ok = false;
                                for (size_t j = 0; j < i; ++j) {
                                    free(frame->cond_words[j]);
                                }
                                free(frame->cond_words);
                                frame->cond_words = NULL;
                                frame->cond_kind = SHELL_LOOP_CONDITION_NONE;
                                break;
                            }
                        }
                        if (ok) {
                            frame->cond_word_count = condition_payload_count;
                        }
                    }
                } else {
                    frame->cond_kind = SHELL_LOOP_CONDITION_NONE;
                }
            }
        }
        if (ok) {
            switch (frame->body_kind) {
                case SHELL_LOOP_BODY_KIND_ARITH: {
                    if (body_expression_count == 0 || body_expression_index < 0 || body_expression_index >= redir_start_index) {
                        frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
                    } else {
                        const Value *entry = &args[body_expression_index];
                        const char *expr = (entry->type == TYPE_STRING && entry->s_val) ? entry->s_val : "";
                        frame->body_arith_expression = strdup(expr);
                        if (!frame->body_arith_expression) {
                            ok = false;
                            frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
                        }
                    }
                    break;
                }
                case SHELL_LOOP_BODY_KIND_TEST:
                case SHELL_LOOP_BODY_KIND_BRACKET:
                case SHELL_LOOP_BODY_KIND_TEST_ARITH:
                case SHELL_LOOP_BODY_KIND_BRACKET_ARITH: {
                    if (body_payload_count == 0 || body_start_index + (int)body_payload_count > redir_start_index) {
                        frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
                        break;
                    }
                    frame->body_words = (char **)calloc(body_payload_count, sizeof(char *));
                    if (!frame->body_words) {
                        frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
                        ok = false;
                        break;
                    }
                    frame->body_word_count = body_payload_count;
                    for (size_t i = 0; i < body_payload_count; ++i) {
                        const Value *entry = &args[body_start_index + (int)i];
                        const char *spec = (entry->type == TYPE_STRING && entry->s_val) ? entry->s_val : "";
                        frame->body_words[i] = strdup(spec);
                        if (!frame->body_words[i]) {
                            ok = false;
                            frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
                            for (size_t j = 0; j < i; ++j) {
                                free(frame->body_words[j]);
                            }
                            free(frame->body_words);
                            frame->body_words = NULL;
                            frame->body_word_count = 0;
                            break;
                        }
                    }
                    if (!ok) {
                        break;
                    }
                    if (frame->body_kind == SHELL_LOOP_BODY_KIND_TEST_ARITH ||
                        frame->body_kind == SHELL_LOOP_BODY_KIND_BRACKET_ARITH) {
                        if (body_expression_count == 0 || body_expression_index < 0 || body_expression_index >= redir_start_index) {
                            frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
                            break;
                        }
                        const Value *entry = &args[body_expression_index];
                        const char *expr = (entry->type == TYPE_STRING && entry->s_val) ? entry->s_val : "";
                        frame->body_arith_expression = strdup(expr);
                        if (!frame->body_arith_expression) {
                            ok = false;
                            frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
                        }
                    }
                    break;
                }
                case SHELL_LOOP_BODY_KIND_COLON:
                case SHELL_LOOP_BODY_KIND_TRUE:
                case SHELL_LOOP_BODY_KIND_FALSE:
                case SHELL_LOOP_BODY_KIND_NONE:
                default:
                    if (body_payload_count != 0 || body_expression_count != 0) {
                        frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
                    }
                    break;
            }
        } else {
            frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
        }
        if (frame->body_kind == SHELL_LOOP_BODY_KIND_NONE) {
            body_payload_count = 0;
            body_expression_count = 0;
        }
        if (payload_without_redirs > 0) {
            runtimeError(vm, "shell loop: unexpected arguments");
            ok = false;
        }
    }

    if (ok && redir_count > 0) {
        for (size_t i = 0; i < redir_count; ++i) {
            int arg_index = redir_start_index + (int)i;
            if (arg_index < 0 || arg_index >= arg_count) {
                runtimeError(vm, "shell loop: missing redirection argument");
                ok = false;
                break;
            }
            Value entry = args[arg_index];
            if (entry.type != TYPE_STRING || !entry.s_val) {
                runtimeError(vm, "shell loop: invalid redirection argument");
                ok = false;
                break;
            }
            if (!shellAddRedirection(&redir_cmd, entry.s_val)) {
                runtimeError(vm, "shell loop: failed to parse redirection");
                ok = false;
                break;
            }
        }
        if (ok && redir_cmd.redir_count > 0) {
            if (!shellApplyExecRedirections(vm ? vm : gShellCurrentVm, &redir_cmd, &redir_backups, &redir_backup_count)) {
                ok = false;
            } else {
                frame->redirs_active = true;
                frame->redir_backups = redir_backups;
                frame->redir_backup_count = redir_backup_count;
                frame->redir_backup_capacity = redir_backup_count;
                frame->applied_redirs = redir_cmd.redirs;
                frame->applied_redir_count = redir_cmd.redir_count;
                redir_backups = NULL;
                redir_backup_count = 0;
                redir_cmd.redirs = NULL;
                redir_cmd.redir_count = 0;
            }
        }
    }

    if (ok) {
        if (!shellLoopAdoptPipeline(vm ? vm : gShellCurrentVm,
                                    frame,
                                    pipeline_index_value,
                                    pipeline_head_value,
                                    pipeline_tail_value)) {
            ok = false;
        }
    }
    if (!ok) {
        shellUpdateStatus(1);
    }

    if (redir_backups) {
        shellRestoreExecRedirections(redir_backups, redir_backup_count);
        shellFreeExecRedirBackups(redir_backups, redir_backup_count);
        redir_backups = NULL;
    }
    if (redir_cmd.redirs) {
        shellFreeRedirections(&redir_cmd);
    }

    if (!pipeline_active || !ok) {
        if (pipeline_scope_pushed) {
            shellLocalScopePop();
            frame->pipeline_scope_pushed = false;
        }
        frame->pipeline_subshell = false;
        gShellRuntime.local_scope_active = previous_local_scope_flag;
        gShellRuntime.pipeline_subshell_active = previous_pipeline_flag;
    }

    shellMaybeResetPipeline();
    shellRestoreCurrentVm(previous_vm);
    return result;
}

Value vmBuiltinShellLoopEnd(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    ShellLoopFrame *frame = shellLoopTop();
    if (frame) {
        bool propagate_continue = frame->continue_pending;
        bool propagate_break = frame->break_pending;
        shellLoopPopFrame();
        if (gShellLoopStackSize == 0) {
            if (propagate_break) {
                gShellRuntime.break_requested = false;
                gShellRuntime.break_requested_levels = 0;
            }
            if (propagate_continue) {
                gShellRuntime.continue_requested = false;
                gShellRuntime.continue_requested_levels = 0;
            }
        }
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

Value vmBuiltinShellEnterCondition(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    shellRuntimeEnterCondition();
    return makeVoid();
}

Value vmBuiltinShellLeaveCondition(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    shellRuntimeLeaveCondition();
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellLeaveConditionPreserve(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    shellRuntimeLeaveCondition();
    return makeVoid();
}

Value vmBuiltinShellIf(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    shellMaybeResetPipeline();
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

Value vmBuiltinShellCase(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
    if (arg_count != 2 || args[1].type != TYPE_STRING || !args[1].s_val) {
        runtimeError(vm, "shell case: expected metadata and subject word");
        shellUpdateStatus(1);
        goto cleanup;
    }
    const char *subject_spec = args[1].s_val;
    const char *subject_text = subject_spec;
    const char *subject_meta = NULL;
    size_t subject_meta_len = 0;
    uint8_t subject_flags = 0;
    if (!shellDecodeWordSpec(subject_spec, &subject_text, &subject_flags, &subject_meta, &subject_meta_len)) {
        subject_text = subject_spec ? subject_spec : "";
        subject_flags = 0;
    }
    char *expanded_subject = shellExpandWord(subject_text,
                                             subject_flags,
                                             subject_meta,
                                             subject_meta_len,
                                             NULL,
                                             NULL,
                                             NULL);
    if (!expanded_subject) {
        runtimeError(vm, "shell case: out of memory");
        shellUpdateStatus(1);
        goto cleanup;
    }
    if (!shellCaseStackPush(expanded_subject)) {
        free(expanded_subject);
        runtimeError(vm, "shell case: out of memory");
        shellUpdateStatus(1);
        goto cleanup;
    }
    free(expanded_subject);
    shellUpdateStatus(0);

cleanup:
    shellRestoreCurrentVm(previous_vm);
    return result;
}

Value vmBuiltinShellCaseClause(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
    shellRuntimeEnterCondition();
    if (arg_count < 1 || args[0].type != TYPE_STRING) {
        runtimeError(vm, "shell case clause: expected metadata");
        shellUpdateStatus(1);
        goto cleanup;
    }
    ShellCaseContext *ctx = shellCaseStackTop();
    if (!ctx) {
        runtimeError(vm, "shell case clause: no active case");
        shellUpdateStatus(1);
        goto cleanup;
    }
    if (ctx->matched) {
        shellUpdateStatus(1);
        goto cleanup;
    }
    const char *subject = ctx->subject ? ctx->subject : "";
    bool matched = false;
    for (int i = 1; i < arg_count; ++i) {
        if (args[i].type != TYPE_STRING || !args[i].s_val) {
            continue;
        }
        const char *pattern_spec = args[i].s_val;
        const char *pattern_text = pattern_spec;
        const char *pattern_meta = NULL;
        size_t pattern_meta_len = 0;
        uint8_t pattern_flags = 0;
        if (!shellDecodeWordSpec(pattern_spec, &pattern_text, &pattern_flags, &pattern_meta, &pattern_meta_len)) {
            pattern_text = pattern_spec ? pattern_spec : "";
            pattern_flags = 0;
        }
        char *expanded_pattern = shellExpandWord(pattern_text,
                                                pattern_flags,
                                                pattern_meta,
                                                pattern_meta_len,
                                                NULL,
                                                NULL,
                                                NULL);
        if (!expanded_pattern) {
            runtimeError(vm, "shell case clause: out of memory");
            shellUpdateStatus(1);
            goto cleanup;
        }
        if (shellWordShouldGlob(pattern_flags, expanded_pattern)) {
            if (fnmatch(expanded_pattern, subject, 0) == 0) {
                free(expanded_pattern);
                matched = true;
                break;
            }
        } else {
            if (strcmp(expanded_pattern, subject) == 0) {
                free(expanded_pattern);
                matched = true;
                break;
            }
        }
        free(expanded_pattern);
    }
    if (matched) {
        ctx->matched = true;
        shellUpdateStatus(0);
    } else {
        shellUpdateStatus(1);
    }

cleanup:
    shellRuntimeLeaveCondition();
    shellRestoreCurrentVm(previous_vm);
    return result;
}

Value vmBuiltinShellCaseEnd(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    ShellCaseContext *ctx = shellCaseStackTop();
    if (!ctx) {
        runtimeError(vm, "shell case end: no active case");
        shellUpdateStatus(1);
        shellRestoreCurrentVm(previous_vm);
        return makeVoid();
    }
    bool matched = ctx->matched;
    shellCaseStackPop();
    if (!matched) {
        shellUpdateStatus(0);
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

static bool shellEvaluateNumericComparison(const char *left,
                                           const char *op,
                                           const char *right,
                                           bool *out_result);
static bool shellEvaluateFileUnary(const char *op,
                                   const char *operand,
                                   bool *out_result);
static bool shellEvaluateFileBinary(const char *left,
                                    const char *op,
                                    const char *right,
                                    bool *out_result);

static bool shellTestIsUnaryOperator(const char *token) {
    if (!token || token[0] == '\0') {
        return false;
    }
    static const char *const kUnaryOps[] = {
        "-b", "-c", "-d", "-e", "-f", "-g", "-h", "-k", "-p", "-r", "-s", "-t",
        "-u", "-w", "-x", "-L", "-S", "-O", "-G", "-N", "-n", "-z"
    };
    for (size_t i = 0; i < sizeof(kUnaryOps) / sizeof(kUnaryOps[0]); ++i) {
        if (strcmp(token, kUnaryOps[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool shellTestHasPatternChars(const char *pattern) {
    if (!pattern) {
        return false;
    }
    for (const char *p = pattern; *p; ++p) {
        if (*p == '*' || *p == '?' || *p == '[') {
            return true;
        }
    }
    return false;
}

static bool shellTestApplyUnary(const char *op, const char *operand, bool *out_result) {
    if (!op || !out_result) {
        return false;
    }
    const char *value = operand ? operand : "";
    if (strcmp(op, "-n") == 0) {
        *out_result = (value[0] != '\0');
        return true;
    }
    if (strcmp(op, "-z") == 0) {
        *out_result = (value[0] == '\0');
        return true;
    }
    bool result = false;
    if (shellEvaluateFileUnary(op, value, &result)) {
        *out_result = result;
        return true;
    }
    return false;
}

static bool shellTestApplyBinary(const char *left,
                                 const char *op,
                                 const char *right,
                                 bool *out_result) {
    if (!left || !op || !right || !out_result) {
        return false;
    }
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
        if (shellTestHasPatternChars(right)) {
            *out_result = (fnmatch(right, left, 0) == 0);
        } else {
            *out_result = (strcmp(left, right) == 0);
        }
        return true;
    }
    if (strcmp(op, "!=") == 0) {
        if (shellTestHasPatternChars(right)) {
            *out_result = (fnmatch(right, left, 0) != 0);
        } else {
            *out_result = (strcmp(left, right) != 0);
        }
        return true;
    }
    if (strcmp(op, "<") == 0) {
        *out_result = (strcmp(left, right) < 0);
        return true;
    }
    if (strcmp(op, ">") == 0) {
        *out_result = (strcmp(left, right) > 0);
        return true;
    }
    bool result = false;
    if (shellEvaluateNumericComparison(left, op, right, &result)) {
        *out_result = result;
        return true;
    }
    if (shellEvaluateFileBinary(left, op, right, &result)) {
        *out_result = result;
        return true;
    }
    return false;
}

static bool shellTestIsBinaryOperator(const char *token) {
    if (!token || token[0] == '\0') {
        return false;
    }
    static const char *const kBinaryOps[] = {
        "=", "==", "!=", "<", ">", "-eq", "-ne", "-gt", "-ge", "-lt", "-le",
        "-nt", "-ot", "-ef"
    };
    for (size_t i = 0; i < sizeof(kBinaryOps) / sizeof(kBinaryOps[0]); ++i) {
        if (strcmp(token, kBinaryOps[i]) == 0) {
            return true;
        }
    }
    return false;
}

typedef struct {
    VM *vm;
    const char *command_name;
    const char **argv;
    int argc;
    int pos;
    bool is_bracket;
    bool error_reported;
} ShellTestParser;

static void shellTestReportError(ShellTestParser *parser, const char *message) {
    if (!parser || parser->error_reported) {
        return;
    }
    const char *name = parser->command_name;
    if (parser->is_bracket) {
        name = "[";
    }
    if (!name || !*name) {
        name = "test";
    }
    runtimeError(parser->vm, "%s: %s", name, message);
    parser->error_reported = true;
}

static bool shellTestParseExpression(ShellTestParser *parser, bool *out_result);

static bool shellTestTryFastEvaluation(const char **argv, int argc, bool *out_result) {
    if (!argv || argc < 0 || !out_result) {
        return false;
    }

    if (argc == 0) {
        *out_result = false;
        return true;
    }

    if (argc == 1) {
        *out_result = (argv[0] && argv[0][0] != '\0');
        return true;
    }

    if (strcmp(argv[0], "!") == 0 && argc > 1) {
        bool inner = false;
        if (!shellTestTryFastEvaluation(argv + 1, argc - 1, &inner)) {
            return false;
        }
        *out_result = !inner;
        return true;
    }

    if (argc == 2) {
        bool unary_result = false;
        if (shellTestApplyUnary(argv[0], argv[1], &unary_result)) {
            *out_result = unary_result;
            return true;
        }
        *out_result = (argv[0] && argv[1] && strcmp(argv[0], argv[1]) == 0);
        return true;
    }

    if (argc == 3) {
        bool binary_result = false;
        if (shellTestApplyBinary(argv[0], argv[1], argv[2], &binary_result)) {
            *out_result = binary_result;
            return true;
        }
    }

    return false;
}

bool shellBuiltinTestFastEval(const char **argv, int argc, bool is_bracket, bool *out_result) {
    if (!argv || argc < 0 || !out_result) {
        return false;
    }
    if (is_bracket) {
        if (argc == 0) {
            return false;
        }
        if (strcmp(argv[argc - 1], "]") != 0) {
            return false;
        }
        argc--;
    }
    return shellTestTryFastEvaluation(argv, argc, out_result);
}

bool shellBuiltinArithmeticFastEval(const char *expression, int *out_status) {
    if (!expression || !out_status) {
        return false;
    }
    long long expr_value = 0;
    if (!shellArithmeticCommandEvaluateExpression(expression, &expr_value)) {
        return false;
    }
    *out_status = (expr_value != 0) ? 0 : 1;
    return true;
}

static bool shellTestParsePrimary(ShellTestParser *parser, bool *out_result) {
    if (!parser || !out_result) {
        return false;
    }
    if (parser->pos >= parser->argc) {
        shellTestReportError(parser, "missing argument");
        return false;
    }
    const char *token = parser->argv[parser->pos];
    if (strcmp(token, ")") == 0) {
        shellTestReportError(parser, "syntax error: unexpected ')'");
        return false;
    }
    if (strcmp(token, "(") == 0) {
        parser->pos++;
        bool inner = false;
        if (!shellTestParseExpression(parser, &inner)) {
            return false;
        }
        if (parser->pos >= parser->argc || strcmp(parser->argv[parser->pos], ")") != 0) {
            shellTestReportError(parser, "missing ')'");
            return false;
        }
        parser->pos++;
        *out_result = inner;
        return true;
    }
    if (strcmp(token, "-a") == 0 || strcmp(token, "-o") == 0) {
        shellTestReportError(parser, "unary operator expected");
        return false;
    }
    if (shellTestIsUnaryOperator(token)) {
        if (parser->pos + 1 >= parser->argc) {
            shellTestReportError(parser, "unary operator expected");
            return false;
        }
        const char *operand = parser->argv[parser->pos + 1];
        bool value = false;
        if (!shellTestApplyUnary(token, operand, &value)) {
            shellTestReportError(parser, "invalid unary operator");
            return false;
        }
        parser->pos += 2;
        *out_result = value;
        return true;
    }
    if (parser->pos + 2 < parser->argc) {
        const char *op = parser->argv[parser->pos + 1];
        if (shellTestIsBinaryOperator(op)) {
            const char *left = token;
            const char *right = parser->argv[parser->pos + 2];
            bool value = false;
            if (!shellTestApplyBinary(left, op, right, &value)) {
                shellTestReportError(parser, "invalid binary operator");
                return false;
            }
            parser->pos += 3;
            *out_result = value;
            return true;
        }
    }
    parser->pos++;
    *out_result = (token && token[0] != '\0');
    return true;
}

static bool shellTestParseNot(ShellTestParser *parser, bool *out_result) {
    if (!parser || !out_result) {
        return false;
    }
    bool negate = false;
    while (parser->pos < parser->argc && strcmp(parser->argv[parser->pos], "!") == 0) {
        negate = !negate;
        parser->pos++;
    }
    bool value = false;
    if (!shellTestParsePrimary(parser, &value)) {
        return false;
    }
    if (negate) {
        value = !value;
    }
    *out_result = value;
    return true;
}

static bool shellTestParseAnd(ShellTestParser *parser, bool *out_result) {
    if (!parser || !out_result) {
        return false;
    }
    bool value = false;
    if (!shellTestParseNot(parser, &value)) {
        return false;
    }
    while (parser->pos < parser->argc && strcmp(parser->argv[parser->pos], "-a") == 0) {
        parser->pos++;
        bool rhs = false;
        if (!shellTestParseNot(parser, &rhs)) {
            return false;
        }
        value = value && rhs;
    }
    *out_result = value;
    return true;
}

static bool shellTestParseExpression(ShellTestParser *parser, bool *out_result) {
    if (!parser || !out_result) {
        return false;
    }
    bool value = false;
    if (!shellTestParseAnd(parser, &value)) {
        return false;
    }
    while (parser->pos < parser->argc && strcmp(parser->argv[parser->pos], "-o") == 0) {
        parser->pos++;
        bool rhs = false;
        if (!shellTestParseAnd(parser, &rhs)) {
            return false;
        }
        value = value || rhs;
    }
    *out_result = value;
    return true;
}

Value vmBuiltinShellTest(VM *vm, int arg_count, Value *args) {
    const char *name = shellRuntimeCurrentCommandName();
    if (!name || !*name) {
        name = (vm && vm->current_builtin_name) ? vm->current_builtin_name : "test";
    }
    bool is_bracket = (name && strcmp(name, "[") == 0);

    const char **argv = NULL;
    if (arg_count > 0) {
        argv = (const char **)malloc(sizeof(const char *) * (size_t)arg_count);
        if (!argv) {
            const char *prefix = is_bracket ? "[" : name;
            if (!prefix || !*prefix) {
                prefix = "test";
            }
            runtimeError(vm, "%s: out of memory", prefix);
            shellUpdateStatus(2);
            return makeVoid();
        }
        for (int i = 0; i < arg_count; ++i) {
            if (args[i].type == TYPE_STRING && args[i].s_val) {
                argv[i] = args[i].s_val;
            } else {
                argv[i] = "";
            }
        }
    }

    int argc = arg_count;
    if (is_bracket) {
        if (argc == 0 || strcmp(argv[argc - 1], "]") != 0) {
            runtimeError(vm, "[: missing ']'");
            free(argv);
            shellUpdateStatus(2);
            return makeVoid();
        }
        argc--;
        if (argc == 0) {
            free(argv);
            shellUpdateStatus(1);
            return makeVoid();
        }
    }

    if (argc == 0) {
        free(argv);
        shellUpdateStatus(1);
        return makeVoid();
    }

    bool fast_result = false;
    if (shellTestTryFastEvaluation(argv, argc, &fast_result)) {
        shellUpdateStatus(fast_result ? 0 : 1);
        free(argv);
        return makeVoid();
    }

    ShellTestParser parser = {
        .vm = vm,
        .command_name = name,
        .argv = argv,
        .argc = argc,
        .pos = 0,
        .is_bracket = is_bracket,
        .error_reported = false
    };

    bool result = false;
    bool ok = shellTestParseExpression(&parser, &result);
    if (ok && parser.pos < parser.argc) {
        shellTestReportError(&parser, "too many arguments");
        ok = false;
    }

    if (!ok) {
        shellUpdateStatus(2);
    } else {
        shellUpdateStatus(result ? 0 : 1);
    }

    free(argv);
    return makeVoid();
}

Value vmBuiltinShellDefineFunction(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
    if (arg_count != 3) {
        runtimeError(vm, "shell define function: expected name, parameters, and body");
        shellUpdateStatus(1);
        goto cleanup;
    }
    if (args[0].type != TYPE_STRING || !args[0].s_val || args[0].s_val[0] == '\0') {
        runtimeError(vm, "shell define function: name must be a non-empty string");
        shellUpdateStatus(1);
        goto cleanup;
    }
    if (args[1].type != TYPE_STRING && args[1].type != TYPE_VOID && args[1].type != TYPE_NIL) {
        runtimeError(vm, "shell define function: parameter metadata must be a string");
        shellUpdateStatus(1);
        goto cleanup;
    }
    if (args[2].type != TYPE_POINTER || !args[2].ptr_val) {
        runtimeError(vm, "shell define function: missing compiled body");
        shellUpdateStatus(1);
        goto cleanup;
    }
    const char *name = args[0].s_val;
    const char *param_meta = NULL;
    if (args[1].type == TYPE_STRING && args[1].s_val) {
        param_meta = args[1].s_val;
    }
    ShellCompiledFunction *compiled = (ShellCompiledFunction *)args[2].ptr_val;
    if (!shellStoreFunction(name, param_meta, compiled)) {
        shellDisposeCompiledFunction(compiled);
        runtimeError(vm, "shell define function: failed to store '%s'", name);
        shellUpdateStatus(1);
        goto cleanup;
    }
    args[2].ptr_val = NULL;
    shellUpdateStatus(0);

cleanup:
    shellRestoreCurrentVm(previous_vm);
    return result;
}

static bool shellParseLong(const char *text, long *out_value) {
    if (!out_value || !text || *text == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    *out_value = value;
    return true;
}

static bool shellParseSignedLongLong(const char *text, long long *out_value) {
    if (!out_value) {
        return false;
    }
    if (!text || *text == '\0') {
        *out_value = 0;
        return true;
    }
    errno = 0;
    char *end = NULL;
    long long value = strtoll(text, &end, 0);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    *out_value = value;
    return true;
}

static bool shellParseUnsignedLongLong(const char *text, unsigned long long *out_value) {
    if (!out_value) {
        return false;
    }
    if (!text || *text == '\0') {
        *out_value = 0;
        return true;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    *out_value = value;
    return true;
}

static void shellGetoptsUpdateOptind(int value) {
    if (value < 1) {
        value = 1;
    }
    gShellRuntime.getopts_index = value;
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%d", value);
    shellSetTrackedVariable("OPTIND", buffer, false);
}

static void shellGetoptsSetOptarg(const char *value) {
    if (value && *value) {
        shellSetTrackedVariable("OPTARG", value, false);
    } else {
        shellUnsetTrackedVariable("OPTARG");
    }
}

static bool shellParseDouble(const char *text, double *out_value) {
    if (!out_value) {
        return false;
    }
    if (!text || *text == '\0') {
        *out_value = 0.0;
        return true;
    }
    errno = 0;
    char *end = NULL;
    double value = strtod(text, &end);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    *out_value = value;
    return true;
}

static bool shellEvaluateNumericComparison(const char *left,
                                          const char *op,
                                          const char *right,
                                          bool *out_result) {
    if (!left || !op || !right || !out_result) {
        return false;
    }
    long lhs = 0;
    long rhs = 0;
    if (!shellParseLong(left, &lhs) || !shellParseLong(right, &rhs)) {
        return false;
    }
    if (strcmp(op, "-eq") == 0) {
        *out_result = (lhs == rhs);
        return true;
    }
    if (strcmp(op, "-ne") == 0) {
        *out_result = (lhs != rhs);
        return true;
    }
    if (strcmp(op, "-gt") == 0) {
        *out_result = (lhs > rhs);
        return true;
    }
    if (strcmp(op, "-lt") == 0) {
        *out_result = (lhs < rhs);
        return true;
    }
    if (strcmp(op, "-ge") == 0) {
        *out_result = (lhs >= rhs);
        return true;
    }
    if (strcmp(op, "-le") == 0) {
        *out_result = (lhs <= rhs);
        return true;
    }
    return false;
}

static bool shellEvaluateFileUnary(const char *op,
                                   const char *operand,
                                   bool *out_result) {
    if (!op || !out_result) {
        return false;
    }
    const char *path = operand ? operand : "";
    if (strcmp(op, "-t") == 0) {
        long fd = 0;
        if (!shellParseLong(path, &fd)) {
            *out_result = false;
            return true;
        }
        *out_result = (fd >= 0 && pscalRuntimeFdIsInteractive((int)fd));
        return true;
    }

    if (*path == '\0') {
        *out_result = false;
        return true;
    }

    struct stat st;
    int rc = 0;
    bool use_lstat = (strcmp(op, "-h") == 0 || strcmp(op, "-L") == 0);
    if (use_lstat) {
        rc = lstat(path, &st);
    } else {
        rc = stat(path, &st);
    }
    if (rc != 0) {
        *out_result = false;
        return true;
    }

    if (strcmp(op, "-e") == 0) {
        *out_result = true;
        return true;
    }
    if (strcmp(op, "-f") == 0) {
        *out_result = S_ISREG(st.st_mode);
        return true;
    }
    if (strcmp(op, "-d") == 0) {
        *out_result = S_ISDIR(st.st_mode);
        return true;
    }
    if (strcmp(op, "-b") == 0) {
        *out_result = S_ISBLK(st.st_mode);
        return true;
    }
    if (strcmp(op, "-c") == 0) {
        *out_result = S_ISCHR(st.st_mode);
        return true;
    }
    if (strcmp(op, "-p") == 0) {
        *out_result = S_ISFIFO(st.st_mode);
        return true;
    }
    if (strcmp(op, "-S") == 0) {
        *out_result = S_ISSOCK(st.st_mode);
        return true;
    }
    if (strcmp(op, "-L") == 0 || strcmp(op, "-h") == 0) {
        *out_result = S_ISLNK(st.st_mode);
        return true;
    }
    if (strcmp(op, "-s") == 0) {
        *out_result = (st.st_size > 0);
        return true;
    }
    if (strcmp(op, "-r") == 0) {
        *out_result = (access(path, R_OK) == 0);
        return true;
    }
    if (strcmp(op, "-w") == 0) {
        *out_result = (access(path, W_OK) == 0);
        return true;
    }
    if (strcmp(op, "-x") == 0) {
        *out_result = (access(path, X_OK) == 0);
        return true;
    }
    if (strcmp(op, "-g") == 0) {
        *out_result = ((st.st_mode & S_ISGID) != 0);
        return true;
    }
    if (strcmp(op, "-u") == 0) {
        *out_result = ((st.st_mode & S_ISUID) != 0);
        return true;
    }
    if (strcmp(op, "-k") == 0) {
        *out_result = ((st.st_mode & S_ISVTX) != 0);
        return true;
    }
    if (strcmp(op, "-O") == 0) {
        *out_result = (st.st_uid == geteuid());
        return true;
    }
    if (strcmp(op, "-G") == 0) {
        *out_result = (st.st_gid == getegid());
        return true;
    }
    if (strcmp(op, "-N") == 0) {
        *out_result = (st.st_mtime > st.st_atime);
        return true;
    }

    return false;
}

static bool shellEvaluateFileBinary(const char *left,
                                    const char *op,
                                    const char *right,
                                    bool *out_result) {
    if (!left || !op || !right || !out_result) {
        return false;
    }
    struct stat left_stat;
    struct stat right_stat;
    if (strcmp(op, "-nt") == 0) {
        if (stat(left, &left_stat) != 0 || stat(right, &right_stat) != 0) {
            *out_result = false;
            return true;
        }
        *out_result = (left_stat.st_mtime > right_stat.st_mtime);
        return true;
    }
    if (strcmp(op, "-ot") == 0) {
        if (stat(left, &left_stat) != 0 || stat(right, &right_stat) != 0) {
            *out_result = false;
            return true;
        }
        *out_result = (left_stat.st_mtime < right_stat.st_mtime);
        return true;
    }
    if (strcmp(op, "-ef") == 0) {
        if (stat(left, &left_stat) != 0 || stat(right, &right_stat) != 0) {
            *out_result = false;
            return true;
        }
        *out_result = (left_stat.st_dev == right_stat.st_dev && left_stat.st_ino == right_stat.st_ino);
        return true;
    }
    return false;
}

Value vmBuiltinShellDoubleBracket(VM *vm, int arg_count, Value *args) {
    (void)vm;
    /* Be tolerant of unmatched parsing so that [[ ... ]] still works even if
     * the rewrite step failed and left a trailing "]]" argument. */
    while (arg_count > 0) {
        const Value *last = &args[arg_count - 1];
        const char *text = (last->type == TYPE_STRING && last->s_val) ? last->s_val : "";
        if (strcmp(text, "]]") != 0) {
            break;
        }
        arg_count--;
    }
    bool negate = false;
    int index = 0;
    while (index < arg_count) {
        const Value *value = &args[index];
        const char *text = (value->type == TYPE_STRING && value->s_val) ? value->s_val : "";
        if (strcmp(text, "!") != 0) {
            break;
        }
        negate = !negate;
        index++;
    }

    bool result = false;
    int remaining = arg_count - index;
    if (remaining <= 0) {
        goto done;
    }

    const char *first = (args[index].type == TYPE_STRING && args[index].s_val) ? args[index].s_val : "";
    if (remaining == 1) {
        result = (first[0] != '\0');
        goto done;
    }

    if (remaining == 2) {
        const char *operand = (args[index + 1].type == TYPE_STRING && args[index + 1].s_val)
                                  ? args[index + 1].s_val
                                  : "";
        bool evaluated = false;
        if (strcmp(first, "-z") == 0) {
            result = (operand[0] == '\0');
            evaluated = true;
        } else if (strcmp(first, "-n") == 0) {
            result = (operand[0] != '\0');
            evaluated = true;
        } else if (shellEvaluateFileUnary(first, operand, &result)) {
            evaluated = true;
        } else if (first && first[0] == '-' && first[1] != '\0') {
            /* Treat unrecognised unary operators as a failed test rather than
             * falling back to string truthiness. This keeps behaviour aligned
             * with traditional shells where unknown predicates are errors. */
            result = false;
            evaluated = true;
        }
        if (!evaluated) {
            result = (operand[0] != '\0');
        }
        goto done;
    }

    if (remaining >= 3) {
        const char *left = first;
        const char *op = (args[index + 1].type == TYPE_STRING && args[index + 1].s_val)
                             ? args[index + 1].s_val
                             : "";
        const char *right = (args[index + 2].type == TYPE_STRING && args[index + 2].s_val)
                                ? args[index + 2].s_val
                                : "";

        bool compared = false;
        if (shellEvaluateFileBinary(left, op, right, &result)) {
            compared = true;
        } else if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
            result = (strcmp(left, right) == 0);
            compared = true;
        } else if (strcmp(op, "!=") == 0) {
            result = (strcmp(left, right) != 0);
            compared = true;
        } else if (strcmp(op, ">") == 0) {
            result = (strcmp(left, right) > 0);
            compared = true;
        } else if (strcmp(op, "<") == 0) {
            result = (strcmp(left, right) < 0);
            compared = true;
        } else if (shellEvaluateNumericComparison(left, op, right, &result)) {
            compared = true;
        }

        if (!compared) {
            result = false;
        }
    }

done:
    if (negate) {
        result = !result;
    }
    shellUpdateStatus(result ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellCd(VM *vm, int arg_count, Value *args) {
    const char *path = NULL;
    int arg_index = 0;

    if (arg_count > 0 && args[0].type == TYPE_STRING && args[0].s_val &&
        strcmp(args[0].s_val, "--") == 0) {
        arg_index = 1;
    }

    int remaining = arg_count - arg_index;
    if (remaining <= 0) {
        path = shellSafeGetenv("PSCALI_REAL_HOME");
        if (!path || !*path) {
            path = shellSafeGetenv("HOME");
        }
        if (!path) {
            runtimeError(vm, "cd: HOME not set");
            shellUpdateStatus(1);
            return makeVoid();
        }
    } else if (args[arg_index].type == TYPE_STRING && args[arg_index].s_val) {
        if (remaining > 1) {
            runtimeError(vm, "cd: too many arguments");
            shellUpdateStatus(1);
            return makeVoid();
        }
        path = args[arg_index].s_val;
    } else {
        runtimeError(vm, "cd: expected directory path");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (!shellDirectoryStackEnsureInitialised(vm, "cd")) {
        return makeVoid();
    }

#if defined(PSCAL_TARGET_IOS)
    char resolved_target[PATH_MAX];
    if (path) {
        const char *expanded = shellResolvePathForIOS(path, resolved_target, sizeof(resolved_target));
        if (expanded) {
            path = expanded;
        }
    }
#endif

    char *new_cwd = NULL;
    char *old_cwd = NULL;
    if (!shellDirectoryStackChdir(vm, "cd", path, &new_cwd, &old_cwd)) {
        return makeVoid();
    }

    if (!shellDirectoryStackAssignTopOwned(new_cwd)) {
        shellDirectoryStackReportError(vm, "cd", "out of memory");
        shellUpdateStatus(1);
        if (old_cwd) {
            (void)chdir(old_cwd);
            shellDirectoryStackEnsureInitialised(vm, "cd");
            (void)shellDirectoryStackUpdateEnvironment(NULL, old_cwd);
        }
        free(old_cwd);
        return makeVoid();
    }

    if (!shellDirectoryStackUpdateEnvironment(old_cwd, gShellRuntime.dir_stack[0])) {
        const char *readonly = shellReadonlyGetErrorName();
        if (old_cwd) {
            (void)chdir(old_cwd);
            if (!shellDirectoryStackAssignTopOwned(old_cwd)) {
                free(old_cwd);
            }
            old_cwd = NULL;
        } else {
            (void)shellDirectoryStackAssignTopOwned(NULL);
        }
        if (errno == EPERM && readonly && *readonly) {
            runtimeError(vm, "cd: %s: readonly variable", readonly);
        } else if (errno != 0) {
            runtimeError(vm, "cd: %s", strerror(errno));
        } else {
            runtimeError(vm, "cd: failed to update environment");
        }
        shellUpdateStatus(errno ? errno : 1);
        free(old_cwd);
        return makeVoid();
    }
    free(old_cwd);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellPwd(VM *vm, int arg_count, Value *args) {
    (void)arg_count;
    (void)args;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        runtimeError(vm, "pwd: %s", strerror(errno));
        shellUpdateStatus(errno ? errno : 1);
        return makeVoid();
    }
#if defined(PSCAL_TARGET_IOS)
    char display[PATH_MAX];
    const char *visible = pathTruncateStrip(cwd, display, sizeof(display)) ? display : cwd;
    printf("%s\n", visible);
#else
    printf("%s\n", cwd);
#endif
    shellUpdateStatus(0);
    return makeVoid();
}

static void shellCatEmitEscaped(int ch) {
    if (ch == '\n') {
        fputc('\n', stdout);
        return;
    }
    if (ch == '\t') {
        fputs("^I", stdout);
        return;
    }
    if (ch == '\r') {
        fputs("^M", stdout);
        return;
    }
    if (ch < 32) {
        fputc('^', stdout);
        fputc(ch + 64, stdout);
        return;
    }
    if (ch == 127) {
        fputs("^?", stdout);
        return;
    }
    if (ch & 0x80) {
        fputs("M-", stdout);
        shellCatEmitEscaped(ch & 0x7F);
        return;
    }
    fputc(ch, stdout);
}

static int shellCatStream(FILE *stream, bool show_nonprinting) {
    char buffer[4096];
    while (true) {
        size_t read_bytes = fread(buffer, 1, sizeof(buffer), stream);
        if (read_bytes > 0) {
            if (show_nonprinting) {
                for (size_t i = 0; i < read_bytes; ++i) {
                    shellCatEmitEscaped((unsigned char)buffer[i]);
                }
            } else {
                if (fwrite(buffer, 1, read_bytes, stdout) != read_bytes) {
                    return errno ? errno : 1;
                }
            }
        }
        if (read_bytes < sizeof(buffer)) {
            if (feof(stream)) {
                break;
            }
            if (ferror(stream)) {
                return errno ? errno : 1;
            }
        }
    }
    return 0;
}

static int shellCatPath(const char *path, bool show_nonprinting) {
    if (!path || strcmp(path, "-") == 0) {
        return shellCatStream(stdin, show_nonprinting);
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "cat: %s: %s\n", path ? path : "", strerror(errno));
        return errno ? errno : 1;
    }
    int rc = shellCatStream(fp, show_nonprinting);
    fclose(fp);
    if (rc != 0) {
        fprintf(stderr, "cat: %s: %s\n", path, strerror(errno ? errno : EIO));
    }
    return rc;
}

Value vmBuiltinShellCat(VM *vm, int arg_count, Value *args) {
    bool show_nonprinting = false;
    int index = 0;
    (void)vm;

    while (index < arg_count) {
        const Value *arg = &args[index];
        if (arg->type != TYPE_STRING || !arg->s_val || arg->s_val[0] != '-') {
            break;
        }
        const char *opt = arg->s_val + 1;
        if (*opt == '\0') {
            break;
        }
        for (; *opt; ++opt) {
            if (*opt == 'v') {
                show_nonprinting = true;
            } else if (*opt == '-') {
                opt++;
                break;
            } else {
                runtimeError(vm, "cat: unknown option -- '%c'", *opt);
                shellUpdateStatus(1);
                return makeVoid();
            }
        }
        index++;
        if (opt[-1] == '-') {
            break;
        }
    }
    int status = 0;
    if (index >= arg_count) {
        status = shellCatPath("-", show_nonprinting);
    } else {
        for (int i = index; i < arg_count; ++i) {
            const Value *arg = &args[i];
            if (arg->type != TYPE_STRING || !arg->s_val) {
                runtimeError(vm, "cat: expected string arguments");
                status = 1;
                break;
            }
            int rc = shellCatPath(arg->s_val, show_nonprinting);
            if (rc != 0 && status == 0) status = rc;
        }
    }
    shellUpdateStatus(status);
    return makeVoid();
}

Value vmBuiltinShellChmod(VM *vm, int arg_count, Value *args) {
#ifdef PSCAL_TARGET_IOS
    if (arg_count < 2) {
        runtimeError(vm, "chmod: usage: chmod MODE file [file ...]");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (args[0].type != TYPE_STRING || !args[0].s_val || args[0].s_val[0] == '\0') {
        runtimeError(vm, "chmod: invalid mode");
        shellUpdateStatus(1);
        return makeVoid();
    }
    char *endptr = NULL;
    errno = 0;
    long parsed = strtol(args[0].s_val, &endptr, 8);
    if (errno != 0 || !endptr || *endptr != '\0' || parsed < 0 || parsed > 07777) {
        runtimeError(vm, "chmod: %s: invalid mode", args[0].s_val);
        shellUpdateStatus(1);
        return makeVoid();
    }
    mode_t mode = (mode_t)parsed;
    int final_status = 0;
    for (int i = 1; i < arg_count; ++i) {
        if (args[i].type != TYPE_STRING || !args[i].s_val || args[i].s_val[0] == '\0') {
            runtimeError(vm, "chmod: expected file paths");
            shellUpdateStatus(1);
            return makeVoid();
        }
        if (chmod(args[i].s_val, mode) != 0) {
            fprintf(stderr, "chmod: %s: %s\n", args[i].s_val, strerror(errno));
            if (final_status == 0) {
                final_status = errno ? errno : 1;
            }
        }
    }
    shellUpdateStatus(final_status);
    return makeVoid();
#else
    runtimeError(vm, "chmod: not supported on this platform");
    shellUpdateStatus(1);
    return makeVoid();
#endif
}

Value vmBuiltinShellClear(VM *vm, int arg_count, Value *args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "clear: unexpected arguments");
        shellUpdateStatus(1);
        return makeVoid();
    }
    const char *sequence = "\x1B[2J\x1B[H";
    if (fputs(sequence, stdout) == EOF) {
        runtimeError(vm, "clear: failed to write escape sequence");
        shellUpdateStatus(errno ? errno : 1);
        return makeVoid();
    }
    fflush(stdout);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellStdioInfo(VM *vm, int arg_count, Value *args) {
    (void)args;
    if (arg_count > 0) {
        if (arg_count == 1 && args[0].type == TYPE_STRING && args[0].s_val &&
            (strcmp(args[0].s_val, "--help") == 0 || strcmp(args[0].s_val, "-h") == 0)) {
            printf("stdioinfo\n");
            printf("  Show shell stdio and session routing details.\n");
            shellUpdateStatus(0);
            return makeVoid();
        }
        runtimeError(vm, "stdioinfo: unexpected arguments");
        shellUpdateStatus(1);
        return makeVoid();
    }

    printf("stdioinfo:\n");
    printf("  fds: stdin=%d stdout=%d stderr=%d\n", STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO);
    printf("  isatty: stdin=%d stdout=%d stderr=%d\n",
           isatty(STDIN_FILENO) != 0,
           isatty(STDOUT_FILENO) != 0,
           isatty(STDERR_FILENO) != 0);
    printf("  interactive: stdin=%d stdout=%d stderr=%d\n",
           pscalRuntimeFdIsInteractive(STDIN_FILENO) ? 1 : 0,
           pscalRuntimeFdIsInteractive(STDOUT_FILENO) ? 1 : 0,
           pscalRuntimeFdIsInteractive(STDERR_FILENO) ? 1 : 0);
    printf("  real_tty: stdin=%d stdout=%d stderr=%d\n",
           pscalRuntimeFdHasRealTTY(STDIN_FILENO) ? 1 : 0,
           pscalRuntimeFdHasRealTTY(STDOUT_FILENO) ? 1 : 0,
           pscalRuntimeFdHasRealTTY(STDERR_FILENO) ? 1 : 0);
#if defined(PSCAL_TARGET_IOS)
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    bool session_default = !session || vprocSessionStdioIsDefault(session);
    printf("  session: ptr=%p default=%d session_id=%llu kernel_pid=%d\n",
           (void *)session,
           session_default ? 1 : 0,
           session ? (unsigned long long)session->session_id : 0ull,
           session ? session->kernel_pid : 0);
    if (session) {
        printf("  host_fds: stdin=%d stdout=%d stderr=%d\n",
               session->stdin_host_fd,
               session->stdout_host_fd,
               session->stderr_host_fd);
        printf("  pscal_fds: stdin=%p stdout=%p stderr=%p\n",
               (void *)session->stdin_pscal_fd,
               (void *)session->stdout_pscal_fd,
               (void *)session->stderr_pscal_fd);
        printf("  pty: active=%d master=%p slave=%p\n",
               session->pty_active ? 1 : 0,
               (void *)session->pty_master,
               (void *)session->pty_slave);
    }
    VProc *vp = vprocCurrent();
    if (vp) {
        printf("  vproc: ptr=%p pid=%d\n", (void *)vp, vprocPid(vp));
        printf("  shim_map: stdin=%d stdout=%d stderr=%d\n",
               vprocTranslateFd(vp, STDIN_FILENO),
               vprocTranslateFd(vp, STDOUT_FILENO),
               vprocTranslateFd(vp, STDERR_FILENO));
    } else {
        printf("  vproc: <none>\n");
    }
#endif

    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellTabName(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1 || args[0].type != TYPE_STRING || !args[0].s_val) {
        runtimeError(vm, "tabname: expected a single title string");
        shellUpdateStatus(1);
        return makeVoid();
    }
#if defined(PSCAL_TAB_TITLE_SUPPORT)
    const char *title = args[0].s_val;
    int rc = PSCALRuntimeSetTabTitle(title);
    if (rc != 0) {
        runtimeError(vm, "tabname: unable to update tab title");
        shellUpdateStatus(1);
        return makeVoid();
    }
    shellUpdateStatus(0);
#else
    (void)args;
    runtimeError(vm, "tabname: tab naming is only available on iOS/iPadOS builds");
    shellUpdateStatus(1);
#endif
    return makeVoid();
}

#ifdef PSCAL_TARGET_IOS
static void shellApplyWindowSize(int rows, int cols) {
    if (rows <= 0 || cols <= 0) {
        return;
    }
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", rows);
    setenv("LINES", buffer, 1);
    snprintf(buffer, sizeof(buffer), "%d", cols);
    setenv("COLUMNS", buffer, 1);
    printf("\x1b[8;%d;%dt", rows, cols);
    fflush(stdout);
}

Value vmBuiltinShellResize(VM *vm, int arg_count, Value *args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "resize: unexpected arguments");
        shellUpdateStatus(1);
        return makeVoid();
    }
    int rows = pscalRuntimeDetectWindowRows();
    int cols = pscalRuntimeDetectWindowCols();
    if (rows <= 0 || cols <= 0) {
        runtimeError(vm, "resize: unable to detect terminal size");
        shellUpdateStatus(1);
        return makeVoid();
    }
    shellApplyWindowSize(rows, cols);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellGwin(VM *vm, int arg_count, Value *args) {
    char *piped_message = NULL;
    if (!pscalRuntimeStdinIsInteractive()) {
        size_t cap = 1024;
        size_t len = 0;
        piped_message = (char *)malloc(cap);
        if (piped_message) {
            int ch = 0;
            while ((ch = fgetc(stdin)) != EOF) {
                if (len + 1 >= cap) {
                    size_t new_cap = cap * 2;
                    char *resized = (char *)realloc(piped_message, new_cap);
                    if (!resized) {
                        free(piped_message);
                        piped_message = NULL;
                        break;
                    }
                    piped_message = resized;
                    cap = new_cap;
                }
                piped_message[len++] = (char)ch;
            }
            if (piped_message) {
                while (len > 0 && (piped_message[len - 1] == '\n' || piped_message[len - 1] == '\r')) {
                    len--;
                }
                piped_message[len] = '\0';
                if (len == 0) {
                    free(piped_message);
                    piped_message = NULL;
                }
            }
        }
    }

    const char *message = piped_message ? piped_message : "gwin for the win";
    if (arg_count > 0) {
        if (args[0].type != TYPE_STRING || !args[0].s_val) {
            runtimeError(vm, "gwin: expected an optional message string");
            shellUpdateStatus(1);
            free(piped_message);
            return makeVoid();
        }
        message = args[0].s_val;
    }
    pscalShowGwinMessage(message);
    shellUpdateStatus(0);
    if (piped_message) {
        free(piped_message);
    }
    return makeVoid();
}

Value vmBuiltinShellEditorDump(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "editordump: unexpected arguments");
        shellUpdateStatus(1);
        return makeVoid();
    }
    pscalEditorDump();
    shellUpdateStatus(0);
    return makeVoid();
}
#endif

#ifdef PSCAL_TARGET_IOS

typedef int (*ShellToolEntryFn)(int argc, char *argv[]);

extern VProcSessionStdio *PSCALRuntimeGetCurrentRuntimeStdio(void) __attribute__((weak));

static int shellRunToolInlineWithRestore(ShellToolEntryFn entry,
                                         int argc,
                                         char **argv,
                                         bool dbg) {
    ShellSignalBackup sig_backup;
    ShellStdFdBackup fd_backup;
    VProcSessionStdio *prev_session = vprocSessionStdioCurrent();
    VProcSessionStdio *active_session = prev_session;
    VProc *prev_vproc = vprocCurrent();
    struct termios stdin_termios;
    bool stdin_termios_valid = false;
    shellSaveSignalState(&sig_backup);
    shellSaveStandardFds(&fd_backup);
    if (dbg) {
        int stdin_host = STDIN_FILENO;
        VProc *vp = vprocCurrent();
        if (vp) {
            int translated = vprocTranslateFd(vp, STDIN_FILENO);
            if (translated >= 0) {
                stdin_host = translated;
            }
        }
        fprintf(stderr,
                "[tool-inline] stdio save stdin=%d host=%d tty=%d vproc=%p session=%p\n",
                STDIN_FILENO,
                stdin_host,
                pscalRuntimeStdinHasRealTTY() ? 1 : 0,
                (void *)prev_vproc,
                (void *)prev_session);
    }
    if (PSCALRuntimeGetCurrentRuntimeStdio) {
        VProcSessionStdio *runtime_stdio = PSCALRuntimeGetCurrentRuntimeStdio();
        if (runtime_stdio && !vprocSessionStdioIsDefault(runtime_stdio) &&
            (!active_session || vprocSessionStdioIsDefault(active_session))) {
            active_session = runtime_stdio;
        }
    }
    if (active_session && active_session != prev_session) {
        vprocSessionStdioActivate(active_session);
    }
    if (shellToolTcgetattr(STDIN_FILENO, &stdin_termios)) {
        stdin_termios_valid = true;
    }
    int status = entry(argc, argv);
    shellFlushStandardStreams();
    if (prev_session) {
        vprocSessionStdioActivate(prev_session);
    } else {
        vprocSessionStdioActivate(NULL);
    }
    if (prev_vproc) {
        vprocActivate(prev_vproc);
    } else {
        vprocDeactivate();
    }
    shellRestoreStandardFds(&fd_backup, dbg);
    if (stdin_termios_valid) {
        shellToolTcsetattr(STDIN_FILENO, TCSAFLUSH, &stdin_termios);
    }
    shellCloseVprocStreams();
    if (dbg) {
        int stdin_host = STDIN_FILENO;
        VProc *vp = vprocCurrent();
        if (vp) {
            int translated = vprocTranslateFd(vp, STDIN_FILENO);
            if (translated >= 0) {
                stdin_host = translated;
            }
        }
        fprintf(stderr,
                "[tool-inline] stdio restore stdin=%d host=%d tty=%d vproc=%p session=%p\n",
                STDIN_FILENO,
                stdin_host,
                pscalRuntimeStdinHasRealTTY() ? 1 : 0,
                (void *)vprocCurrent(),
                (void *)vprocSessionStdioCurrent());
    }
    shellRestoreSignalState(&sig_backup);
    shellRuntimeEnsureStandardFds();
    shellEnsureJobControl();
    shellJobControlRestoreForeground();
    if (dbg) {
        int shell_pid = vprocGetShellSelfPid();
        int sid = (shell_pid > 0) ? vprocGetSid(shell_pid) : -1;
        int fg = (sid > 0) ? vprocGetForegroundPgid(sid) : -1;
        fprintf(stderr,
                "[tool-inline] restore complete shell=%d sid=%d fg=%d\n",
                shell_pid,
                sid,
                fg);
    }
    return status;
}

#define SHELL_TOOL_THREAD_STACK_SZ (8 * 1024 * 1024)

typedef struct PSCALRuntimeContext PSCALRuntimeContext;
extern PSCALRuntimeContext *PSCALRuntimeGetCurrentRuntimeContext(void) __attribute__((weak));
extern void PSCALRuntimeSetCurrentRuntimeContext(PSCALRuntimeContext *ctx) __attribute__((weak));

typedef struct {
    ShellToolEntryFn entry;
    int argc;
    char **argv;
    bool argv_owned;
    FrontendKind frontend_kind;
    VProcSessionStdio *session_stdio;
    VProc *session_vproc;
    PSCALRuntimeContext *runtime_ctx;
    int stdout_fd;
    int stderr_fd;
    int status;
    volatile int finished;
    pthread_t worker_thread;
    pthread_t main_thread;
    bool use_shell_context;
} ShellToolThreadContext;

static __thread sigjmp_buf gShellThreadJumpBuf;
static __thread int gShellThreadLastSignal = 0;

static void shellThreadSigintHandler(int signo) {
    gShellThreadLastSignal = signo;
    siglongjmp(gShellThreadJumpBuf, 1);
}

static void *shellToolThreadMain(void *arg) {
    ShellToolThreadContext *ctx = (ShellToolThreadContext *)arg;
    if (!ctx || !ctx->entry) {
        if (ctx) {
            ctx->status = 127;
            ctx->finished = 1;
        }
        return NULL;
    }
    bool dbg = getenv("PSCALI_TOOL_DEBUG") != NULL;
    PSCALRuntimeContext *prev_runtime_ctx = NULL;
    if (PSCALRuntimeGetCurrentRuntimeContext) {
        prev_runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext();
    }
    if (PSCALRuntimeSetCurrentRuntimeContext && ctx->runtime_ctx) {
        PSCALRuntimeSetCurrentRuntimeContext(ctx->runtime_ctx);
        if (getenv("PSCALI_VPROC_DEBUG")) {
            fprintf(stderr,
                    "[tool-thread] runtime ctx=%p\n",
                    (void *)ctx->runtime_ctx);
        }
    }
    VProcSessionStdio *prev_stdio = vprocSessionStdioCurrent();
    if (ctx->session_stdio) {
        vprocSessionStdioActivate(ctx->session_stdio);
    }
    bool vproc_active = false;
    if (ctx->session_vproc) {
        vprocRegisterThread(ctx->session_vproc, pthread_self());
        vprocActivate(ctx->session_vproc);
        vproc_active = true;
    }
    if (dbg) {
        fprintf(stderr, "[tool-thread] worker start argc=%d entry=%p vproc=%p\n",
                ctx->argc, (void *)ctx->entry, (void *)ctx->session_vproc);
    }
    (void)ctx->stdout_fd;
    (void)ctx->stderr_fd;

    /* Unblock SIGINT for this thread */
    sigset_t unblock_mask;
    sigemptyset(&unblock_mask);
    sigaddset(&unblock_mask, SIGINT);
    sigaddset(&unblock_mask, SIGTSTP);
    pthread_sigmask(SIG_UNBLOCK, &unblock_mask, NULL);

    /* Install safe termination handler */
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shellThreadSigintHandler;
    sigemptyset(&sa.sa_mask);
    /* SA_RESTART is purposely omitted so we interrupt blocking calls */
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, &old_sa);
    struct sigaction sa_tstp, old_sa_tstp;
    memset(&sa_tstp, 0, sizeof(sa_tstp));
    sa_tstp.sa_handler = shellThreadSigintHandler;
    sigemptyset(&sa_tstp.sa_mask);
    sa_tstp.sa_flags = 0;
    sigaction(SIGTSTP, &sa_tstp, &old_sa_tstp);

    FrontendKind previous_kind = frontendPushKind(ctx->frontend_kind);
    ShellRuntimeState *thread_ctx = NULL;
    ShellRuntimeState *prev_ctx = NULL;
    if (ctx->use_shell_context) {
        thread_ctx = shellRuntimeCreateContext();
        prev_ctx = shellRuntimeActivateContext(thread_ctx);
    }

    gShellThreadLastSignal = 0;
    if (sigsetjmp(gShellThreadJumpBuf, 1) == 0) {
        if (ctx->stdout_fd >= 0) {
            dup2(ctx->stdout_fd, STDOUT_FILENO);
        }
        if (ctx->stderr_fd >= 0) {
            dup2(ctx->stderr_fd, STDERR_FILENO);
        }
        if (ctx->stdout_fd >= 0) {
            close(ctx->stdout_fd);
            ctx->stdout_fd = -1;
        }
        if (ctx->stderr_fd >= 0 && ctx->stderr_fd != ctx->stdout_fd) {
            close(ctx->stderr_fd);
            ctx->stderr_fd = -1;
        }
        ctx->status = ctx->entry(ctx->argc, ctx->argv);
    } else {
        /* Interrupted */
        if (gShellThreadLastSignal == SIGTSTP) {
            ctx->status = 128 + SIGTSTP;
        } else {
            ctx->status = 130;
        }
    }

    if (ctx->use_shell_context) {
        shellRuntimeActivateContext(prev_ctx);
        shellRuntimeDestroyContext(thread_ctx);
    }
    frontendPopKind(previous_kind);

    if (ctx->argv_owned && ctx->argv) {
        for (int i = 0; i < ctx->argc; ++i) {
            free(ctx->argv[i]);
        }
        free(ctx->argv);
        ctx->argv = NULL;
    }

    (void)ctx->status;

    /* Restore signal handler */
    sigaction(SIGINT, &old_sa, NULL);
    sigaction(SIGTSTP, &old_sa_tstp, NULL);
    if (ctx->session_stdio) {
        vprocSessionStdioActivate(prev_stdio);
    }
    if (vproc_active) {
        vprocDeactivate();
    }
    if (PSCALRuntimeSetCurrentRuntimeContext && prev_runtime_ctx) {
        PSCALRuntimeSetCurrentRuntimeContext(prev_runtime_ctx);
    }
    if (dbg) {
        fprintf(stderr, "[tool-thread] worker done status=%d\n", ctx->status);
    }
    return NULL;
}

static void *shellToolWaiterMain(void *arg) {
    ShellToolThreadContext *ctx = (ShellToolThreadContext *)arg;
    pthread_join(ctx->worker_thread, NULL);
    ctx->finished = 1;
    pthread_kill(ctx->main_thread, SIGURG);
    if (getenv("PSCALI_TOOL_DEBUG")) {
        fprintf(stderr, "[tool-thread] waiter signaled main\n");
    }
    return NULL;
}

static int shellRunToolInThread(ShellToolEntryFn entry,
                                int argc,
                                char **argv,
                                FrontendKind frontend_kind,
                                bool use_shell_context,
                                bool background) {
    if (!entry) {
        return 127;
    }
    if (background) {
        ShellToolThreadContext *context =
            (ShellToolThreadContext *)calloc(1, sizeof(ShellToolThreadContext));
        if (!context) {
            return entry(argc, argv);
        }
        /* Avoid racing with shared argv if the caller frees it after return. */
        context->argv_owned = false;
        if (argv) {
            context->argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
            if (context->argv) {
                context->argv_owned = true;
                for (int i = 0; i < argc; ++i) {
                    if (argv[i]) {
                        context->argv[i] = strdup(argv[i]);
                        if (!context->argv[i]) {
                            context->argv_owned = false;
                            break;
                        }
                    }
                }
                context->argv[argc] = NULL;
            }
        }
        if (context->argv_owned == false) {
            /* Cloning failed; run synchronously to avoid use-after-free. */
            free(context->argv);
            free(context);
            return entry(argc, argv);
        }
        context->entry = entry;
        context->argc = argc;
        context->status = 0;
        context->finished = 0;
        context->main_thread = pthread_self();
        context->frontend_kind = frontend_kind;
        context->use_shell_context = use_shell_context;
        context->session_stdio = vprocSessionStdioCurrent();
        context->session_vproc = vprocCurrent();
        context->runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext
            ? PSCALRuntimeGetCurrentRuntimeContext()
            : NULL;
        context->stdout_fd = dup(STDOUT_FILENO);
        context->stderr_fd = dup(STDERR_FILENO);
        if (getenv("PSCALI_VPROC_DEBUG")) {
            fprintf(stderr,
                    "[tool-thread] spawn bg stdio=%p vproc=%p stdout_fd=%d stderr_fd=%d\n",
                    (void *)context->session_stdio,
                    (void *)context->session_vproc,
                    context->stdout_fd,
                    context->stderr_fd);
        }

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, SHELL_TOOL_THREAD_STACK_SZ);
        int create_rc = pthread_create(&context->worker_thread, &attr, shellToolThreadMain, context);
        pthread_attr_destroy(&attr);
        if (create_rc != 0) {
            free(context);
            return entry(argc, argv);
        }
        pthread_detach(context->worker_thread);
        return 0;
    }

    ShellToolThreadContext context;
    memset(&context, 0, sizeof(context));
    context.entry = entry;
    context.argc = argc;
    context.argv = argv;
    context.status = 127;
    context.finished = 0;
    context.main_thread = pthread_self();
    context.frontend_kind = frontend_kind;
    context.use_shell_context = use_shell_context;
    context.session_stdio = vprocSessionStdioCurrent();
    context.session_vproc = vprocCurrent();
    context.runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext
        ? PSCALRuntimeGetCurrentRuntimeContext()
        : NULL;
    /* Foreground runs keep the caller's stdio; use -1 to skip dup2. */
    context.stdout_fd = -1;
    context.stderr_fd = -1;
    if (getenv("PSCALI_VPROC_DEBUG")) {
        fprintf(stderr,
                "[tool-thread] spawn fg stdio=%p vproc=%p\n",
                (void *)context.session_stdio,
                (void *)context.session_vproc);
    }

    /* Block SIGINT and SIGURG on Main */
    sigset_t new_mask, old_mask;
    sigemptyset(&new_mask);
    sigaddset(&new_mask, SIGINT);
    sigaddset(&new_mask, SIGURG);
    pthread_sigmask(SIG_BLOCK, &new_mask, &old_mask);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, SHELL_TOOL_THREAD_STACK_SZ);
    int create_rc = pthread_create(&context.worker_thread, &attr, shellToolThreadMain, &context);
    pthread_attr_destroy(&attr);
    if (create_rc != 0) {
        pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
        if (!use_shell_context) {
            return entry(argc, argv);
        }
        ShellRuntimeState *ctx = shellRuntimeCreateContext();
        ShellRuntimeState *prev = shellRuntimeActivateContext(ctx);
        int status = entry(argc, argv);
        shellRuntimeActivateContext(prev);
        shellRuntimeDestroyContext(ctx);
        return status;
    }

    /* Spawn waiter */
    pthread_t waiter_thread;
    if (pthread_create(&waiter_thread, NULL, shellToolWaiterMain, &context) != 0) {
        /* If waiter fails, just join worker directly (SIGINT won't work properly but we can't do much) */
        pthread_join(context.worker_thread, NULL);
        pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
        return context.status;
    }

    /* Main Loop */
    sigset_t wait_mask;
    sigemptyset(&wait_mask);
    sigaddset(&wait_mask, SIGINT);
    sigaddset(&wait_mask, SIGURG);

    while (!context.finished) {
        sigset_t pending;
        sigemptyset(&pending);
        if (pscalRuntimeConsumeSigint()) {
            pthread_kill(context.worker_thread, SIGINT);
        }
        if (sigpending(&pending) == 0 && sigismember(&pending, SIGINT)) {
            pthread_kill(context.worker_thread, SIGINT);
        }
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 200 * 1000 * 1000; /* 200ms */
        nanosleep(&ts, NULL);
        /* Poll for completion to avoid hanging if SIGURG never arrives. */
    }

    pthread_join(waiter_thread, NULL);
    pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
    return context.status;
}

static const int kShellToolRunnerUnavailable = -32768;

static int __attribute__((unused)) shellSpawnToolProcess(const char *tool_name, int argc, char **argv) {
    const char *runner = getenv("PSCALI_TOOL_RUNNER_PATH");
    if (!runner || !*runner) {
        (void)tool_name;
        return kShellToolRunnerUnavailable;
    }
#if defined(PSCAL_TARGET_IOS)
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (session && !vprocSessionStdioIsDefault(session)) {
        return kShellToolRunnerUnavailable;
    }
#endif

    size_t child_count = (size_t)argc + 1; // runner path + tool argv vector
    char **child_argv = (char **)calloc(child_count + 1, sizeof(char *));
    if (!child_argv) {
        fprintf(stderr, "%s: out of memory launching tool runner\n", tool_name);
        return 1;
    }

    child_argv[0] = (char *)runner;
    for (int i = 0; i < argc; ++i) {
        child_argv[i + 1] = argv[i];
    }

    pid_t pid = 0;
    int spawn_rc = posix_spawn(&pid, runner, NULL, NULL, child_argv, environ);
    free(child_argv);
    if (spawn_rc == EPERM || spawn_rc == ENOSYS) {
        return kShellToolRunnerUnavailable;
    }
    if (spawn_rc != 0) {
        fprintf(stderr, "%s: failed to launch tool runner: %s\n", tool_name, strerror(spawn_rc));
        return 127;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 127;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

static Value shellRunToolBuiltin(VM *vm,
                                 const char *tool_name,
                                 ShellToolEntryFn entry,
                                 FrontendKind target_kind,
                                 int arg_count,
                                 Value *args) {
    if (getenv("PSCALI_PIPE_DEBUG")) {
        fprintf(stderr, "[tool-dispatch] name=%s argc=%d\n", tool_name ? tool_name : "<null>", arg_count + 1);
    }
#if defined(PSCAL_TARGET_IOS)
    bool tool_dbg = getenv("PSCALI_TOOL_DEBUG") != NULL;
    /* Special-case exsh: on iOS we cannot exec a new process, so run exsh_main inline
     * with the provided arguments to preserve -c/script execution. */
    if (tool_name && strcmp(tool_name, "exsh") == 0 && entry) {
        int argc = arg_count + 1;
        char **argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
        if (!argv) {
            shellUpdateStatus(127);
            return makeVoid();
        }
        argv[0] = strdup("exsh");
        for (int i = 0; i < arg_count; ++i) {
            const Value *v = &args[i];
            const char *s = (v->type == TYPE_STRING && v->s_val) ? v->s_val : "";
            argv[i + 1] = strdup(s);
        }
        int status = entry(argc, argv);
        for (int i = 0; i <= argc; ++i) {
            free(argv[i]);
        }
        free(argv);
        shellUpdateStatus(status);
        return makeVoid();
    }
#endif
    FrontendKind previous_kind = frontendPushKind(target_kind);
    if (!entry) {
        runtimeError(vm, "%s: tool entrypoint unavailable", tool_name);
        shellUpdateStatus(127);
        frontendPopKind(previous_kind);
        return makeVoid();
    }

    PascalGlobalState pascal_state;
    pascalPushGlobalState(&pascal_state);

    const int saved_param_count = gParamCount;
    char **saved_param_values = gParamValues;
    bool saved_param_owned = gParamValuesOwned;

    const int argc = arg_count + 1;
    // Add one extra slot so argv[argc] stays NULL like a real C argv vector.
    char **argv = (char **)calloc((size_t)(argc + 1), sizeof(char *));
    if (!argv) {
        runtimeError(vm, "%s: out of memory", tool_name);
        shellUpdateStatus(1);
        pascalPopGlobalState(&pascal_state);
        frontendPopKind(previous_kind);
        return makeVoid();
    }

    bool ok = true;
    argv[0] = strdup(tool_name);
    if (!argv[0]) {
        ok = false;
    }

    for (int i = 0; ok && i < arg_count; ++i) {
        const Value *arg = &args[i];
        if (arg->type != TYPE_STRING) {
            runtimeError(vm, "%s: expected string arguments", tool_name);
            ok = false;
            break;
        }
        const char *src = arg->s_val ? arg->s_val : "";
        argv[i + 1] = strdup(src);
        if (!argv[i + 1]) {
            runtimeError(vm, "%s: out of memory", tool_name);
            ok = false;
        }
    }

    if (ok) {
        int status = kShellToolRunnerUnavailable;
#if !defined(PSCAL_TARGET_IOS)
        status = shellSpawnToolProcess(tool_name, argc, argv);
#endif
        if (status == kShellToolRunnerUnavailable && entry) {
            bool needs_shell_ctx = (target_kind == FRONTEND_KIND_SHELL);
            bool background = shellRuntimeCurrentBuiltinBackground();
#if defined(PSCAL_TARGET_IOS)
            /* When running inside the background worker thread, always execute
             * inline so the worker's redirections (stdout/stderr to files) are
             * honored. */
            if (gShellBackgroundWorkerActive) {
                background = true;
            }
            fprintf(stderr, "[tool-dispatch] name=%s bg=%d worker=%d argc=%d\n",
                    tool_name ? tool_name : "<null>", (int)background, (int)gShellBackgroundWorkerActive, argc);
            if (background && gShellBackgroundWorkerActive) {
                fprintf(stderr, "[tool-inline] %s argc=%d (bg worker)\n", tool_name, argc);
                for (int i = 0; i < argc; ++i) {
                    fprintf(stderr, "[tool-inline] argv[%d]=%s\n", i, argv[i] ? argv[i] : "(null)");
                }
                /* Already running inside a background worker: execute inline so redirections
                 * remain active for this thread. */
                status = shellRunToolInlineWithRestore(entry, argc, argv, tool_dbg);
                fprintf(stderr, "[tool-inline] %s exit=%d\n", tool_name, status);
            } else if (background || target_kind == FRONTEND_KIND_SHELL) {
                fprintf(stderr, "[tool-thread] %s argc=%d bg=%d\n", tool_name, argc, (int)background);
                status = shellRunToolInThread(entry, argc, argv, target_kind, needs_shell_ctx, background);
                fprintf(stderr, "[tool-thread] %s dispatched status=%d\n", tool_name, status);
            } else {
                /* Run non-shell frontends inline on iOS to avoid repeated
                 * cross-thread state swaps that can leave symbol tables in an
                 * inconsistent state. */
                status = shellRunToolInlineWithRestore(entry, argc, argv, tool_dbg);
            }
            shellInvalidateFrontendState(target_kind);
            /* The frontend reset tears down its own symbol tables; reattach the saved
             * shell tables so pascalPopGlobalState does not attempt to free
             * already-destroyed structures. */
            globalSymbols = pascal_state.globalSymbols;
            constGlobalSymbols = pascal_state.constGlobalSymbols;
            localSymbols = pascal_state.localSymbols;
            procedure_table = pascal_state.procedure_table;
            current_procedure_table = pascal_state.current_procedure_table;
            type_table = pascal_state.type_table;
            /* For inline/background runs, return the actual worker status so the shell
             * prompt reflects tool success/failure. */
            gShellRuntime.last_status = status;
#else
            if (needs_shell_ctx || background) {
                status = shellRunToolInThread(entry, argc, argv, target_kind, needs_shell_ctx, background);
            } else {
                status = entry(argc, argv);
            }
            pascalInvalidateGlobalState();
#endif
        } else if (status == kShellToolRunnerUnavailable) {
            fprintf(stderr, "%s: tool runner unavailable and no builtin entry.\n", tool_name);
            status = 127;
        }
        fflush(NULL);
        shellUpdateStatus(status);
    } else {
        shellUpdateStatus(1);
    }

    for (int i = 0; i < argc; ++i) {
        free(argv[i]);
    }
    free(argv);

    gParamCount = saved_param_count;
    gParamValues = saved_param_values;
    gParamValuesOwned = saved_param_owned;
    pascalPopGlobalState(&pascal_state);

    Value result = makeVoid();
    /* For synchronous iOS runs, the worker thread will set gShellRuntime.last_status.
     * Use that value when available to avoid a default 127 on success. */
    int final_status = shellRuntimeLastStatus();
    shellUpdateStatus(final_status);
    if (getenv("PSCALI_PIPE_DEBUG")) {
        fprintf(stderr, "[tool-dispatch] %s final status=%d\n", tool_name ? tool_name : "<null>", final_status);
    }
    frontendPopKind(previous_kind);
    return result;
}

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} ShellStringVector;

static void shellStringVectorFree(ShellStringVector *vec) {
    if (!vec) {
        return;
    }
    for (size_t i = 0; i < vec->count; ++i) {
        free(vec->items[i]);
    }
    free(vec->items);
    vec->items = NULL;
    vec->count = 0;
    vec->capacity = 0;
}

static bool shellStringVectorPush(ShellStringVector *vec, const char *value) {
    if (!vec || !value) {
        return false;
    }
    if (vec->count == vec->capacity) {
        size_t new_cap = vec->capacity ? vec->capacity * 2 : 32;
        char **resized = (char **)realloc(vec->items, new_cap * sizeof(char *));
        if (!resized) {
            return false;
        }
        vec->items = resized;
        vec->capacity = new_cap;
    }
    vec->items[vec->count] = strdup(value);
    if (!vec->items[vec->count]) {
        return false;
    }
    vec->count++;
    return true;
}

static int shellLsCompare(const void *a, const void *b) {
    const char *const *lhs = (const char *const *)a;
    const char *const *rhs = (const char *const *)b;
    return strcasecmp(*lhs, *rhs);
}

static int shellBuiltinListDirectory(const char *path, bool show_all, bool show_almost_all) {
    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return errno ? errno : 1;
    }

    ShellStringVector entries = {0};
    errno = 0;
    struct dirent *dent;
    while ((dent = readdir(dir)) != NULL) {
        if (dent->d_name[0] == '.') {
            if (!show_all) {
                if (!show_almost_all) {
                    continue;
                }
                if (strcmp(dent->d_name, ".") == 0 || strcmp(dent->d_name, "..") == 0) {
                    continue;
                }
            }
        }
        if (!shellStringVectorPush(&entries, dent->d_name)) {
            errno = ENOMEM;
            break;
        }
    }
    int err = errno;
    closedir(dir);

    if (err && err != 0) {
        shellStringVectorFree(&entries);
        fprintf(stderr, "ls: %s: %s\n", path, strerror(err));
        return err;
    }

    qsort(entries.items, entries.count, sizeof(char *), shellLsCompare);
    for (size_t i = 0; i < entries.count; ++i) {
        printf("%s\n", entries.items[i]);
    }
    shellStringVectorFree(&entries);
    return 0;
}

static int shellBuiltinListPath(const char *path, bool show_all, bool show_almost_all, bool print_header, bool needs_spacing) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return errno ? errno : 1;
    }

    if (!S_ISDIR(st.st_mode)) {
        printf("%s\n", path);
        return 0;
    }

    if (print_header) {
        if (needs_spacing) {
            printf("\n");
        }
        printf("%s:\n", path);
    }
    return shellBuiltinListDirectory(path, show_all, show_almost_all);
}

Value vmBuiltinShellLs(VM *vm, int arg_count, Value *args) {
    bool show_all = false;
    bool show_almost_all = false;
    int index = 0;
    while (index < arg_count) {
        if (args[index].type != TYPE_STRING || !args[index].s_val) {
            break;
        }
        const char *word = args[index].s_val;
        if (word[0] != '-') {
            break;
        }
        if (strcmp(word, "--") == 0) {
            index++;
            break;
        }
        for (const char *opt = word + 1; *opt; ++opt) {
            if (*opt == 'a') {
                show_all = true;
                show_almost_all = false;
            } else if (*opt == 'A') {
                if (!show_all) {
                    show_almost_all = true;
                }
            } else {
                runtimeError(vm, "ls: unsupported option -%c", *opt);
                shellUpdateStatus(1);
                return makeVoid();
            }
        }
        index++;
    }

    int path_count = arg_count - index;
    const char *default_path = ".";
    const char **paths = NULL;
    if (path_count <= 0) {
        path_count = 1;
        paths = &default_path;
    } else {
        paths = (const char **)malloc(sizeof(const char *) * (size_t)path_count);
        if (!paths) {
            runtimeError(vm, "ls: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        for (int i = 0; i < path_count; ++i) {
            const Value *arg = &args[index + i];
            if (arg->type != TYPE_STRING || !arg->s_val) {
                runtimeError(vm, "ls: expected string path arguments");
                free(paths);
                shellUpdateStatus(1);
                return makeVoid();
            }
            paths[i] = arg->s_val;
        }
    }

    int final_status = 0;
    for (int i = 0; i < path_count; ++i) {
        int rc = shellBuiltinListPath(paths[i], show_all, show_almost_all, path_count > 1, i > 0);
        if (rc != 0 && final_status == 0) {
            final_status = rc;
        }
    }
    if (paths != &default_path) {
        free(paths);
    }
    shellUpdateStatus(final_status);
    return makeVoid();
}

extern int pascal_main(int argc, char *argv[]);
extern int clike_main(int argc, char *argv[]);
extern int rea_main(int argc, char *argv[]);
extern int pscalvm_main(int argc, char *argv[]);
extern int pscaljson2bc_main(int argc, char **argv);
extern int exsh_main(int argc, char *argv[]);
#ifdef BUILD_DASCAL
extern int dascal_main(int argc, char *argv[]);
#endif
#ifdef BUILD_PSCALD
extern int pscald_main(int argc, char *argv[]);
#endif

Value vmBuiltinShellPascal(VM *vm, int arg_count, Value *args) {
    return shellRunToolBuiltin(vm, "pascal", pascal_main, FRONTEND_KIND_PASCAL, arg_count, args);
}

#ifdef BUILD_DASCAL
Value vmBuiltinShellDascal(VM *vm, int arg_count, Value *args) {
    return shellRunToolBuiltin(vm, "dascal", dascal_main, FRONTEND_KIND_PASCAL, arg_count, args);
}
#endif

Value vmBuiltinShellClike(VM *vm, int arg_count, Value *args) {
    return shellRunToolBuiltin(vm, "clike", clike_main, FRONTEND_KIND_CLIKE, arg_count, args);
}

Value vmBuiltinShellRea(VM *vm, int arg_count, Value *args) {
    return shellRunToolBuiltin(vm, "rea", rea_main, FRONTEND_KIND_REA, arg_count, args);
}

Value vmBuiltinShellPscalVm(VM *vm, int arg_count, Value *args) {
    return shellRunToolBuiltin(vm, "pscalvm", pscalvm_main, FRONTEND_KIND_PASCAL, arg_count, args);
}

Value vmBuiltinShellPscalJson2bc(VM *vm, int arg_count, Value *args) {
    return shellRunToolBuiltin(vm, "pscaljson2bc", pscaljson2bc_main, FRONTEND_KIND_PASCAL, arg_count, args);
}

Value vmBuiltinShellExshTool(VM *vm, int arg_count, Value *args) {
    return shellRunToolBuiltin(vm, "exsh", exsh_main, FRONTEND_KIND_SHELL, arg_count, args);
}

#ifdef BUILD_PSCALD
Value vmBuiltinShellPscald(VM *vm, int arg_count, Value *args) {
    return shellRunToolBuiltin(vm, "pscald", pscald_main, FRONTEND_KIND_PASCAL, arg_count, args);
}
#endif

#endif /* PSCAL_TARGET_IOS */

Value vmBuiltinShellDirs(VM *vm, int arg_count, Value *args) {
    (void)args;
    if (arg_count > 0) {
        runtimeError(vm, "dirs: unexpected arguments");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (!shellDirectoryStackEnsureInitialised(vm, "dirs")) {
        return makeVoid();
    }
    shellDirectoryStackPrint(STDOUT_FILENO);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellPushd(VM *vm, int arg_count, Value *args) {
    if (!shellDirectoryStackEnsureInitialised(vm, "pushd")) {
        return makeVoid();
    }

    if (arg_count > 1) {
        runtimeError(vm, "pushd: too many arguments");
        shellUpdateStatus(1);
        return makeVoid();
    }

    if (arg_count == 0) {
        if (gShellRuntime.dir_stack_count < 2) {
            runtimeError(vm, "pushd: no other directory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        char *new_cwd = NULL;
        char *old_cwd = NULL;
        if (!shellDirectoryStackChdir(vm, "pushd", gShellRuntime.dir_stack[1], &new_cwd, &old_cwd)) {
            return makeVoid();
        }
        char *old_top = gShellRuntime.dir_stack[0];
        char *second = gShellRuntime.dir_stack[1];
        gShellRuntime.dir_stack[0] = new_cwd;
        gShellRuntime.dir_stack[1] = old_top;
        free(second);
        gShellRuntime.dir_stack_initialised = true;
        shellDirectoryStackUpdateEnvironment(old_cwd, gShellRuntime.dir_stack[0]);
        free(old_cwd);
        shellDirectoryStackPrint(STDOUT_FILENO);
        shellUpdateStatus(0);
        return makeVoid();
    }

    if (args[0].type != TYPE_STRING || !args[0].s_val) {
        runtimeError(vm, "pushd: expected directory path");
        shellUpdateStatus(1);
        return makeVoid();
    }

    size_t needed = gShellRuntime.dir_stack_count + 1;
    if (!shellDirectoryStackEnsureCapacity(needed)) {
        runtimeError(vm, "pushd: out of memory");
        shellUpdateStatus(1);
        return makeVoid();
    }

    char *new_cwd = NULL;
    char *old_cwd = NULL;
    if (!shellDirectoryStackChdir(vm, "pushd", args[0].s_val, &new_cwd, &old_cwd)) {
        return makeVoid();
    }

    size_t count = gShellRuntime.dir_stack_count;
    memmove(&gShellRuntime.dir_stack[1], &gShellRuntime.dir_stack[0], count * sizeof(char *));
    gShellRuntime.dir_stack[0] = new_cwd;
    gShellRuntime.dir_stack_count = count + 1;
    gShellRuntime.dir_stack_initialised = true;
    shellDirectoryStackUpdateEnvironment(old_cwd, gShellRuntime.dir_stack[0]);
    free(old_cwd);
    shellDirectoryStackPrint(STDOUT_FILENO);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellPopd(VM *vm, int arg_count, Value *args) {
    (void)args;
    if (!shellDirectoryStackEnsureInitialised(vm, "popd")) {
        return makeVoid();
    }
    if (arg_count > 0) {
        runtimeError(vm, "popd: unexpected arguments");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (gShellRuntime.dir_stack_count <= 1) {
        runtimeError(vm, "popd: directory stack empty");
        shellUpdateStatus(1);
        return makeVoid();
    }

    char *new_cwd = NULL;
    char *old_cwd = NULL;
    if (!shellDirectoryStackChdir(vm, "popd", gShellRuntime.dir_stack[1], &new_cwd, &old_cwd)) {
        return makeVoid();
    }

    size_t count = gShellRuntime.dir_stack_count;
    char *removed = gShellRuntime.dir_stack[0];
    char *target_entry = gShellRuntime.dir_stack[1];
    free(removed);
    memmove(&gShellRuntime.dir_stack[0], &gShellRuntime.dir_stack[1], (count - 1) * sizeof(char *));
    gShellRuntime.dir_stack_count = count - 1;
    gShellRuntime.dir_stack[gShellRuntime.dir_stack_count] = NULL;
    free(target_entry);
    gShellRuntime.dir_stack[0] = new_cwd;
    gShellRuntime.dir_stack_initialised = true;
    shellDirectoryStackUpdateEnvironment(old_cwd, gShellRuntime.dir_stack[0]);
    free(old_cwd);
    shellDirectoryStackPrint(STDOUT_FILENO);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellFinger(VM *vm, int arg_count, Value *args) {
    const char *target_user = NULL;
    if (arg_count > 1) {
        runtimeError(vm, "finger: expected at most one username");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (arg_count == 1) {
        if (args[0].type != TYPE_STRING || !args[0].s_val || !*args[0].s_val) {
            runtimeError(vm, "finger: expected username as string");
            shellUpdateStatus(1);
            return makeVoid();
        }
        target_user = args[0].s_val;
    }

    struct passwd *pw = NULL;
    if (target_user) {
        pw = getpwnam(target_user);
        if (!pw) {
            runtimeError(vm, "finger: user '%s' not found", target_user);
            shellUpdateStatus(1);
            return makeVoid();
        }
    } else {
        pw = getpwuid(getuid());
        if (!pw) {
            runtimeError(vm, "finger: unable to determine current user");
            shellUpdateStatus(1);
            return makeVoid();
        }
    }

    const char *login = (pw->pw_name && *pw->pw_name) ? pw->pw_name : "(unknown)";
    const char *gecos = (pw->pw_gecos && *pw->pw_gecos) ? pw->pw_gecos : "";
    const char *directory = (pw->pw_dir && *pw->pw_dir) ? pw->pw_dir : "(unknown)";
    const char *shell_path = (pw->pw_shell && *pw->pw_shell) ? pw->pw_shell : "(unknown)";

    char name_buffer[256];
    const char *display_name = gecos;
    if (*display_name) {
        const char *comma = strchr(display_name, ',');
        if (comma) {
            size_t copy_len = (size_t)(comma - display_name);
            if (copy_len >= sizeof(name_buffer)) {
                copy_len = sizeof(name_buffer) - 1;
            }
            memcpy(name_buffer, display_name, copy_len);
            name_buffer[copy_len] = '\0';
            display_name = name_buffer;
        }
    } else {
        display_name = "(unknown)";
    }

    printf("Login: %s\tName: %s\n", login, display_name);
    printf("Directory: %s\n", directory);
    printf("Shell: %s\n", shell_path);
    fflush(stdout);

    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellColon(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    shellUpdateStatus(0);
    return makeVoid();
}

static int shellEchoHexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static void shellEchoWriteString(const char *text,
                                 bool interpret,
                                 bool *stop_output,
                                 bool *emit_newline) {
    if (!text || !stop_output || !emit_newline) {
        return;
    }
    const char *cursor = text;
    while (!*stop_output && cursor && *cursor) {
        char ch = *cursor++;
        if (interpret && ch == '\\') {
            char esc = *cursor;
            if (esc == '\0') {
                fputc('\\', stdout);
                break;
            }
            cursor++;
            switch (esc) {
                case 'a':
                    fputc('\a', stdout);
                    break;
                case 'b':
                    fputc('\b', stdout);
                    break;
                case 'c':
                    *emit_newline = false;
                    *stop_output = true;
                    return;
                case 'e':
                case 'E':
                    fputc(27, stdout);
                    break;
                case 'f':
                    fputc('\f', stdout);
                    break;
                case 'n':
                    fputc('\n', stdout);
                    break;
                case 'r':
                    fputc('\r', stdout);
                    break;
                case 't':
                    fputc('\t', stdout);
                    break;
                case 'v':
                    fputc('\v', stdout);
                    break;
                case '\\':
                    fputc('\\', stdout);
                    break;
                case '0': {
                    unsigned char value = 0;
                    int digits = 0;
                    while (digits < 3 && *cursor >= '0' && *cursor <= '7') {
                        value = (unsigned char)((value << 3) | (*cursor - '0'));
                        cursor++;
                        digits++;
                    }
                    fputc((char)value, stdout);
                    break;
                }
                case 'x': {
                    unsigned char value = 0;
                    int digits = 0;
                    while (digits < 2) {
                        int hex = shellEchoHexDigit(*cursor);
                        if (hex < 0) {
                            break;
                        }
                        value = (unsigned char)((value << 4) | (unsigned char)hex);
                        cursor++;
                        digits++;
                    }
                    if (digits == 0) {
                        fputc('x', stdout);
                    } else {
                        fputc((char)value, stdout);
                    }
                    break;
                }
                default:
                    fputc(esc, stdout);
                    break;
            }
        } else {
            fputc(ch, stdout);
        }
    }
}

Value vmBuiltinShellEcho(VM *vm, int arg_count, Value *args) {
    (void)vm;
    bool emit_newline = true;
    bool interpret_escapes = false;
    bool parsing_options = true;
    int first_arg = 0;

    for (int i = 0; i < arg_count; ++i) {
        if (!parsing_options) {
            first_arg = i;
            break;
        }
        Value val = args[i];
        const char *token = (val.type == TYPE_STRING && val.s_val) ? val.s_val : "";
        if (!token) {
            token = "";
        }
        if (strcmp(token, "--") == 0) {
            parsing_options = false;
            first_arg = i + 1;
            continue;
        }
        if (token[0] == '-' && token[1] != '\0') {
            bool valid = true;
            bool next_emit_newline = emit_newline;
            bool next_interpret = interpret_escapes;
            for (size_t j = 1; token[j] != '\0'; ++j) {
                char opt = token[j];
                if (opt == 'n') {
                    next_emit_newline = false;
                } else if (opt == 'e') {
                    next_interpret = true;
                } else if (opt == 'E') {
                    next_interpret = false;
                } else {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                emit_newline = next_emit_newline;
                interpret_escapes = next_interpret;
                first_arg = i + 1;
                continue;
            }
        }
        parsing_options = false;
        first_arg = i;
        break;
    }
    if (parsing_options) {
        first_arg = arg_count;
    }

    bool stop_output = false;
    for (int i = first_arg; i < arg_count && !stop_output; ++i) {
        Value val = args[i];
        if (i > first_arg) {
            fputc(' ', stdout);
        }
        const char *text = (val.type == TYPE_STRING && val.s_val) ? val.s_val : "";
        shellEchoWriteString(text, interpret_escapes, &stop_output, &emit_newline);
    }

    if (emit_newline && !stop_output) {
        fputc('\n', stdout);
    }

    fflush(stdout);

    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellTrue(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellFalse(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    shellUpdateStatus(1);
    return makeVoid();
}

Value vmBuiltinShellSource(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || args[0].type != TYPE_STRING || !args[0].s_val) {
        runtimeError(vm, "source: expected path to script");
        shellUpdateStatus(1);
        return makeVoid();
    }
    const char *path = args[0].s_val;
    char *source = shellLoadFile(path);
    if (!source) {
        runtimeError(vm, "source: unable to read '%s'", path);
        shellUpdateStatus(errno ? errno : 1);
        return makeVoid();
    }

    char **saved_params = gParamValues;
    int saved_count = gParamCount;
    bool saved_owned = gShellPositionalOwned;

    int new_count = (arg_count > 1) ? (arg_count - 1) : 0;
    char **new_params = NULL;
    bool replaced_params = false;
    if (new_count > 0) {
        new_params = (char **)calloc((size_t)new_count, sizeof(char *));
        if (!new_params) {
            free(source);
            runtimeError(vm, "source: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        for (int i = 0; i < new_count; ++i) {
            if (args[i + 1].type != TYPE_STRING || !args[i + 1].s_val) {
                shellFreeParameterArray(new_params, i);
                free(source);
                runtimeError(vm, "source: arguments must be strings");
                shellUpdateStatus(1);
                return makeVoid();
            }
            new_params[i] = strdup(args[i + 1].s_val);
            if (!new_params[i]) {
                shellFreeParameterArray(new_params, i);
                free(source);
                runtimeError(vm, "source: out of memory");
                shellUpdateStatus(1);
                return makeVoid();
            }
        }
        gParamValues = new_params;
        gParamCount = new_count;
        gShellPositionalOwned = true;
        replaced_params = true;
    }

    ShellRunOptions opts = {0};
    opts.no_cache = 1;
    opts.quiet = true;
    const char *frontend_path = shellRuntimeGetArg0();
    opts.frontend_path = frontend_path ? frontend_path : "exsh";
    opts.exit_on_signal = shellRuntimeExitOnSignal();

    bool exit_requested = false;
    int status = shellRunSource(source, path, &opts, &exit_requested);
    free(source);

    if (replaced_params) {
        if (gShellPositionalOwned) {
            shellFreeOwnedPositionalParameters();
        } else {
            gParamValues = NULL;
            gParamCount = 0;
        }
        gParamValues = saved_params;
        gParamCount = saved_count;
        gShellPositionalOwned = saved_owned;
    }

    if (exit_requested) {
        shellRuntimeRequestExit();
        if (vm) {
            vm->exit_requested = true;
        }
    }

    shellUpdateStatus(status);
    return makeVoid();
}

Value vmBuiltinShellEval(VM *vm, int arg_count, Value *args) {
    if (arg_count == 0) {
        shellUpdateStatus(0);
        return makeVoid();
    }

    size_t total_len = 0;
    for (int i = 0; i < arg_count; ++i) {
        if (args[i].type != TYPE_STRING || !args[i].s_val) {
            runtimeError(vm, "eval: arguments must be strings");
            shellUpdateStatus(1);
            return makeVoid();
        }
        total_len += strlen(args[i].s_val);
        if (i + 1 < arg_count) {
            total_len += 1; // Space separator.
        }
    }

    char *script = (char *)malloc(total_len + 1);
    if (!script) {
        runtimeError(vm, "eval: out of memory");
        shellUpdateStatus(1);
        return makeVoid();
    }

    char *cursor = script;
    for (int i = 0; i < arg_count; ++i) {
        size_t len = strlen(args[i].s_val);
        memcpy(cursor, args[i].s_val, len);
        cursor += len;
        if (i + 1 < arg_count) {
            *cursor++ = ' ';
        }
    }
    *cursor = '\0';

    ShellRunOptions opts = {0};
    opts.no_cache = 1;
    opts.quiet = true;
    const char *frontend_path = shellRuntimeGetArg0();
    opts.frontend_path = frontend_path ? frontend_path : "exsh";
    opts.exit_on_signal = shellRuntimeExitOnSignal();

    ShellSymbolTableScope table_scope;
    shellSymbolTableScopeInit(&table_scope);
    bool scope_pushed = false;
    bool can_run = true;
    if (!shellSymbolTableScopeIsActive()) {
        if (!shellSymbolTableScopePush(&table_scope)) {
            fprintf(stderr, "shell: failed to allocate symbol tables.\n");
            can_run = false;
        } else {
            scope_pushed = true;
        }
    }

    bool exit_requested = false;
    int status = EXIT_FAILURE;
    if (can_run || shellSymbolTableScopeIsActive()) {
        status = shellRunSource(script, "<eval>", &opts, &exit_requested);
    }

    if (scope_pushed) {
        shellSymbolTableScopePop(&table_scope);
    }

    free(script);

    if (!can_run && !shellSymbolTableScopeIsActive()) {
        shellUpdateStatus(1);
        return makeVoid();
    }

    if (exit_requested) {
        shellRuntimeRequestExit();
        if (vm) {
            vm->exit_requested = true;
        }
    }

    shellUpdateStatus(status);
    return makeVoid();
}

Value vmBuiltinShellLet(VM *vm, int arg_count, Value *args) {
    if (arg_count == 0) {
        runtimeError(vm, "let: expected expression");
        shellUpdateStatus(1);
        return makeVoid();
    }

    bool ok = true;
    long long last_value = 0;
    for (int i = 0; i < arg_count; ++i) {
        Value value = args[i];
        if (value.type != TYPE_STRING || !value.s_val) {
            runtimeError(vm, "let: arguments must be strings");
            shellMarkArithmeticError();
            ok = false;
            break;
        }
        long long expr_value = 0;
        if (!shellLetEvaluateExpression(vm, value.s_val, &expr_value)) {
            ok = false;
            break;
        }
        last_value = expr_value;
    }

    if (ok) {
        shellUpdateStatus(last_value != 0 ? 0 : 1);
    } else {
        shellUpdateStatus(1);
    }
    return makeVoid();
}

Value vmBuiltinShellExit(VM *vm, int arg_count, Value *args) {
    int code = shellRuntimeLastStatus();
    if (arg_count >= 1) {
        Value arg = args[0];
        if (IS_INTLIKE(arg)) {
            code = (int)AS_INTEGER(arg);
        } else if (arg.type == TYPE_STRING && arg.s_val) {
            if (*arg.s_val == '\0') {
                code = 0;
            } else {
                int parsed = 0;
                if (shellParseReturnStatus(arg.s_val, &parsed)) {
                    code = parsed;
                } else {
                    fprintf(stderr, "exit: %s: numeric argument required\n", arg.s_val);
                    code = 2;
                }
            }
        } else {
            fprintf(stderr, "exit: numeric argument required\n");
            code = 2;
        }
    }
    shellUpdateStatus(code);
    shellRuntimeRequestExit();
    if (vm) {
        vm->exit_requested = true;
        vm->current_builtin_name = "exit";
    }
    return makeVoid();
}

Value vmBuiltinShellLogout(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);

    if (!shellShoptOptionEnabled("login_shell")) {
        fprintf(stderr, "logout: not login shell: use 'exit'\n");
        shellUpdateStatus(1);
        shellRestoreCurrentVm(previous_vm);
        return makeVoid();
    }

    if (arg_count > 1) {
        fprintf(stderr, "logout: too many arguments\n");
        shellUpdateStatus(1);
        shellRestoreCurrentVm(previous_vm);
        return makeVoid();
    }

    int status = 0;
    if (arg_count == 1) {
        Value v = args[0];
        if (v.type != TYPE_STRING || !v.s_val) {
            fprintf(stderr, "logout: status must be a numeric string\n");
            shellUpdateStatus(1);
            shellRestoreCurrentVm(previous_vm);
            return makeVoid();
        }
        int parsed = 0;
        if (!shellParseReturnStatus(v.s_val, &parsed)) {
            fprintf(stderr, "logout: invalid status '%s'\n", v.s_val);
            shellUpdateStatus(1);
            shellRestoreCurrentVm(previous_vm);
            return makeVoid();
        }
        status = parsed;
    }

    shellUpdateStatus(status);
    shellRuntimeRequestExit();
    if (vm) {
        vm->exit_requested = true;
        vm->current_builtin_name = "logout";
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

Value vmBuiltinShellExecCommand(VM *vm, int arg_count, Value *args) {
    ShellCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.pipeline_index = -1;

    size_t total_args = (arg_count > 0) ? (size_t)arg_count : 0;
    cmd.argv = (char **)calloc(total_args + 2, sizeof(char *));
    if (!cmd.argv) {
        runtimeError(vm, "exec: out of memory");
        shellUpdateStatus(1);
        return makeVoid();
    }

    cmd.argv[0] = strdup("exec");
    if (!cmd.argv[0]) {
        free(cmd.argv);
        runtimeError(vm, "exec: out of memory");
        shellUpdateStatus(1);
        return makeVoid();
    }
    cmd.argc = 1;
    cmd.argv[cmd.argc] = NULL;

    bool ok = true;
    for (int i = 0; i < arg_count && ok; ++i) {
        Value val = args[i];
        if (val.type != TYPE_STRING || !val.s_val) {
            runtimeError(vm, "exec: arguments must be strings");
            shellUpdateStatus(1);
            ok = false;
            break;
        }
        char *copy = strdup(val.s_val);
        if (!copy) {
            runtimeError(vm, "exec: out of memory");
            shellUpdateStatus(1);
            ok = false;
            break;
        }
        cmd.argv[cmd.argc++] = copy;
        cmd.argv[cmd.argc] = NULL;
    }

    if (ok) {
        if (!shellExecuteExecBuiltin(vm, &cmd)) {
            shellUpdateStatus(1);
        }
    }

    shellFreeCommand(&cmd);
    return makeVoid();
}

Value vmBuiltinShellReturn(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    int status = gShellRuntime.last_status;

    if (arg_count > 1) {
        runtimeError(vm, "return: too many arguments");
        shellUpdateStatus(1);
        shellRestoreCurrentVm(previous_vm);
        return makeVoid();
    }

    if (arg_count == 1) {
        Value v = args[0];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "return: status must be a string number");
            shellUpdateStatus(1);
            shellRestoreCurrentVm(previous_vm);
            return makeVoid();
        }
        int parsed = 0;
        if (!shellParseReturnStatus(v.s_val, &parsed)) {
            runtimeError(vm, "return: invalid status '%s'", v.s_val);
            shellUpdateStatus(1);
            shellRestoreCurrentVm(previous_vm);
            return makeVoid();
        }
        status = parsed;
    }

    shellUpdateStatus(status);
    shellRuntimeAbandonConditionEvaluation();
    if (vm) {
        vm->exit_requested = true;
        vm->current_builtin_name = "return";
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

static const char *shellReadResolveIFS(void) {
    const char *ifs = shellSafeGetenv("IFS");
    if (!ifs) {
        return " \t\n";
    }
    return ifs;
}

static bool shellReadIsIFSDelimiter(const char *ifs, char ch) {
    if (!ifs) {
        return false;
    }
    for (const char *p = ifs; *p; ++p) {
        if (*p == ch) {
            return true;
        }
    }
    return false;
}

static bool shellReadIsIFSWhitespaceDelimiter(const char *ifs, char ch) {
    if (!ifs || *ifs == '\0') {
        return false;
    }
    if (!shellReadIsIFSDelimiter(ifs, ch)) {
        return false;
    }
    return isspace((unsigned char)ch) != 0;
}

static char *shellReadCopyValue(const char *text, bool raw_mode) {
    if (!text) {
        return strdup("");
    }

    if (raw_mode) {
        return strdup(text);
    }

    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1);
    if (!copy) {
        return NULL;
    }

    size_t out_index = 0;
    for (size_t i = 0; i < length; ++i) {
        char ch = text[i];
        if (ch == '\\' && i + 1 < length) {
            copy[out_index++] = text[++i];
        } else {
            copy[out_index++] = ch;
        }
    }
    copy[out_index] = '\0';
    return copy;
}

static char *shellReadExtractField(char **cursor,
                                   bool last_field,
                                   bool raw_mode,
                                   const char *ifs) {
    if (!cursor) {
        return strdup("");
    }
    char *text = *cursor;
    if (!text) {
        return strdup("");
    }

    while (*text && shellReadIsIFSWhitespaceDelimiter(ifs, *text)) {
        text++;
    }

    if (last_field) {
        char *value = shellReadCopyValue(text, raw_mode);
        if (!value) {
            return NULL;
        }
        *cursor = text + strlen(text);
        return value;
    }

    char *scan = text;
    while (*scan) {
        if (!raw_mode && *scan == '\\') {
            if (scan[1] == '\0') {
                break;
            }
            scan += 2;
            continue;
        }
        if (shellReadIsIFSDelimiter(ifs, *scan)) {
            break;
        }
        scan++;
    }

    char saved = *scan;
    *scan = '\0';
    char *value = shellReadCopyValue(text, raw_mode);
    *scan = saved;
    if (saved != '\0') {
        scan++;
        if (shellReadIsIFSWhitespaceDelimiter(ifs, saved)) {
            while (*scan && shellReadIsIFSWhitespaceDelimiter(ifs, *scan)) {
                scan++;
            }
        }
    }
    *cursor = scan;
    return value;
}

Value vmBuiltinShellPrintf(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1) {
        shellReportRecoverableError(vm, false, "printf: usage: printf [-v var] format [arguments]");
        shellUpdateStatus(2);
        return makeVoid();
    }
    if (args[0].type != TYPE_STRING || !args[0].s_val) {
        shellReportRecoverableError(vm, true, "printf: format string required");
        shellUpdateStatus(2);
        return makeVoid();
    }

    const char *fmt = args[0].s_val;
    int arg_index = 1;
    bool ok = true;

    bool repeat = false;
    do {
        bool consumed_conversion = false;
        for (size_t i = 0; fmt && fmt[i] != '\0'; ++i) {
            char c = fmt[i];
            if (c == '\\' && fmt[i + 1] != '\0') {
                char esc = fmt[++i];
                switch (esc) {
                    case 'n':
                        fputc('\n', stdout);
                        break;
                    case 'r':
                        fputc('\r', stdout);
                        break;
                    case 't':
                        fputc('\t', stdout);
                        break;
                    case '\\':
                        fputc('\\', stdout);
                        break;
                    case '"':
                        fputc('"', stdout);
                        break;
                    case 'a':
                        fputc('\a', stdout);
                        break;
                    case 'b':
                        fputc('\b', stdout);
                        break;
                    case 'f':
                        fputc('\f', stdout);
                        break;
                    case 'v':
                        fputc('\v', stdout);
                        break;
                    default:
                        fputc(esc, stdout);
                        break;
                }
                continue;
            }
            if (c == '%' && fmt[i + 1] != '\0') {
                if (fmt[i + 1] == '%') {
                    fputc('%', stdout);
                    i++;
                    continue;
                }

                size_t j = i + 1;
                char flags[8];
                size_t flag_len = 0;
                const char *flag_chars = "-+ #0'";
                while (fmt[j] && strchr(flag_chars, fmt[j]) != NULL) {
                    if (flag_len + 1 < sizeof(flags)) {
                        flags[flag_len++] = fmt[j];
                    }
                    j++;
                }
                flags[flag_len] = '\0';

                bool width_specified = false;
                bool width_from_arg = false;
                int width = 0;
                if (fmt[j] == '*') {
                    width_specified = true;
                    width_from_arg = true;
                    j++;
                } else {
                    while (isdigit((unsigned char)fmt[j])) {
                        width_specified = true;
                        width = width * 10 + (fmt[j] - '0');
                        j++;
                    }
                }

                bool precision_specified = false;
                bool precision_from_arg = false;
                int precision = 0;
                if (fmt[j] == '.') {
                    j++;
                    precision_specified = true;
                    if (fmt[j] == '*') {
                        precision_from_arg = true;
                        j++;
                    } else {
                        precision = 0;
                        while (isdigit((unsigned char)fmt[j])) {
                            precision = precision * 10 + (fmt[j] - '0');
                            j++;
                        }
                    }
                }

                bool mod_h = false;
                bool mod_hh = false;
                char length_mod[3] = {0};
                size_t length_len = 0;
                if (fmt[j] == 'h') {
                    mod_h = true;
                    length_mod[length_len++] = 'h';
                    j++;
                    if (fmt[j] == 'h') {
                        mod_hh = true;
                        mod_h = false;
                        length_mod[length_len++] = 'h';
                        j++;
                    }
                } else if (fmt[j] == 'l') {
                    length_mod[length_len++] = 'l';
                    j++;
                    if (fmt[j] == 'l') {
                        length_mod[length_len++] = 'l';
                        j++;
                    }
                } else {
                    const char *length_mods = "Ljzt";
                    while (fmt[j] && strchr(length_mods, fmt[j]) != NULL) {
                        if (length_len + 1 < sizeof(length_mod)) {
                            length_mod[length_len++] = fmt[j];
                        }
                        j++;
                    }
                }

                char spec = fmt[j];
                if (spec == '\0') {
                    shellReportRecoverableError(vm, true, "printf: incomplete format specifier");
                    ok = false;
                    break;
                }

                char fmtbuf[32];
                char buf[256];
                size_t pos = 0;
                fmtbuf[pos++] = '%';
                consumed_conversion = true;
                if (flag_len > 0 && pos + flag_len < sizeof(fmtbuf)) {
                    memcpy(&fmtbuf[pos], flags, flag_len);
                    pos += flag_len;
                }
                if (width_specified) {
                    if (width_from_arg) {
                        if (pos < sizeof(fmtbuf)) {
                            fmtbuf[pos++] = '*';
                        }
                    } else {
                        int written = snprintf(&fmtbuf[pos], sizeof(fmtbuf) - pos, "%d", width);
                        if (written > 0) {
                            if ((size_t)written >= sizeof(fmtbuf) - pos) {
                                pos = sizeof(fmtbuf) - 1;
                            } else {
                                pos += (size_t)written;
                            }
                        }
                    }
                }
                if (precision_specified && pos < sizeof(fmtbuf)) {
                    fmtbuf[pos++] = '.';
                    if (precision_from_arg) {
                        if (pos < sizeof(fmtbuf)) {
                            fmtbuf[pos++] = '*';
                        }
                    } else {
                        int written = snprintf(&fmtbuf[pos], sizeof(fmtbuf) - pos, "%d", precision);
                        if (written > 0) {
                            if ((size_t)written >= sizeof(fmtbuf) - pos) {
                                pos = sizeof(fmtbuf) - 1;
                            } else {
                                pos += (size_t)written;
                            }
                        }
                    }
                }
                if (length_len > 0 && pos + length_len < sizeof(fmtbuf)) {
                    memcpy(&fmtbuf[pos], length_mod, length_len);
                    pos += length_len;
                }
                if (pos < sizeof(fmtbuf)) {
                    fmtbuf[pos++] = spec;
                }
                fmtbuf[pos < sizeof(fmtbuf) ? pos : (sizeof(fmtbuf) - 1)] = '\0';

                int width_value = width;
                if (width_from_arg && arg_index < arg_count) {
                    Value value = args[arg_index];
                    const char *width_text = "";
                    if (value.type == TYPE_STRING && value.s_val) {
                        width_text = value.s_val;
                    }
                    if (width_text[0] == '\0') {
                        width_value = 0;
                    } else {
                        long long parsed_width = 0;
                        if (!shellParseSignedLongLong(width_text, &parsed_width)) {
                            shellReportRecoverableError(vm, false, "printf: %s: invalid number", width_text);
                            ok = false;
                            parsed_width = 0;
                        }
                        if (parsed_width > INT_MAX) {
                            parsed_width = INT_MAX;
                        } else if (parsed_width < INT_MIN) {
                            parsed_width = INT_MIN;
                        }
                        width_value = (int)parsed_width;
                    }
                    arg_index++;
                } else if (width_from_arg) {
                    width_value = 0;
                }

                int precision_value = precision;
                if (precision_from_arg && arg_index < arg_count) {
                    Value value = args[arg_index];
                    const char *precision_text = "";
                    if (value.type == TYPE_STRING && value.s_val) {
                        precision_text = value.s_val;
                    }
                    if (precision_text[0] == '\0') {
                        precision_value = 0;
                    } else {
                        long long parsed_precision = 0;
                        if (!shellParseSignedLongLong(precision_text, &parsed_precision)) {
                            shellReportRecoverableError(vm, false, "printf: %s: invalid number", precision_text);
                            ok = false;
                            parsed_precision = 0;
                        }
                        if (parsed_precision > INT_MAX) {
                            parsed_precision = INT_MAX;
                        } else if (parsed_precision < INT_MIN) {
                            parsed_precision = INT_MIN;
                        }
                        precision_value = (int)parsed_precision;
                    }
                    arg_index++;
                } else if (precision_from_arg) {
                    precision_value = 0;
                }

                const char *arg_text = "";
                if (arg_index < arg_count) {
                    Value value = args[arg_index];
                    if (value.type == TYPE_STRING && value.s_val) {
                        arg_text = value.s_val;
                    } else if (value.type == TYPE_STRING && !value.s_val) {
                        arg_text = "";
                    }
                    arg_index++;
                }

                bool has_wide_char_length = false;
                for (size_t n = 0; n < length_len; ++n) {
                    if (length_mod[n] == 'l' || length_mod[n] == 'L') {
                        has_wide_char_length = true;
                        break;
                    }
                }

            switch (spec) {
                case 'd':
                case 'i': {
                    long long iv = 0;
                    if (!shellParseSignedLongLong(arg_text, &iv)) {
                        shellReportRecoverableError(vm, false, "printf: %s: invalid number", arg_text);
                        iv = 0;
                        ok = false;
                    }
                    if (mod_hh) {
                        iv = (signed char)iv;
                    } else if (mod_h) {
                        iv = (short)iv;
                    }
                    if (width_from_arg && precision_from_arg) {
                        snprintf(buf, sizeof(buf), fmtbuf, width_value, precision_value, iv);
                    } else if (width_from_arg) {
                        snprintf(buf, sizeof(buf), fmtbuf, width_value, iv);
                    } else if (precision_from_arg) {
                        snprintf(buf, sizeof(buf), fmtbuf, precision_value, iv);
                    } else {
                        snprintf(buf, sizeof(buf), fmtbuf, iv);
                    }
                    fputs(buf, stdout);
                    break;
                }
                case 'u':
                case 'o':
                case 'x':
                case 'X': {
                    unsigned long long uv = 0;
                    if (!shellParseUnsignedLongLong(arg_text, &uv)) {
                        shellReportRecoverableError(vm, false, "printf: %s: invalid number", arg_text);
                        uv = 0;
                        ok = false;
                    }
                    if (mod_hh) {
                        uv = (unsigned char)uv;
                    } else if (mod_h) {
                        uv = (unsigned short)uv;
                    }
                    if (width_from_arg && precision_from_arg) {
                        snprintf(buf, sizeof(buf), fmtbuf, width_value, precision_value, uv);
                    } else if (width_from_arg) {
                        snprintf(buf, sizeof(buf), fmtbuf, width_value, uv);
                    } else if (precision_from_arg) {
                        snprintf(buf, sizeof(buf), fmtbuf, precision_value, uv);
                    } else {
                        snprintf(buf, sizeof(buf), fmtbuf, uv);
                    }
                    fputs(buf, stdout);
                    break;
                }
                case 'f':
                case 'F':
                case 'e':
                case 'E':
                case 'g':
                case 'G':
                case 'a':
                case 'A': {
                    double dv = 0.0;
                    if (!shellParseDouble(arg_text, &dv)) {
                        shellReportRecoverableError(vm, false, "printf: %s: invalid number", arg_text);
                        dv = 0.0;
                        ok = false;
                    }
                    if (width_from_arg && precision_from_arg) {
                        snprintf(buf, sizeof(buf), fmtbuf, width_value, precision_value, dv);
                    } else if (width_from_arg) {
                        snprintf(buf, sizeof(buf), fmtbuf, width_value, dv);
                    } else if (precision_from_arg) {
                        snprintf(buf, sizeof(buf), fmtbuf, precision_value, dv);
                    } else {
                        snprintf(buf, sizeof(buf), fmtbuf, dv);
                    }
                    fputs(buf, stdout);
                    break;
                }
                case 'c': {
                    char ch = '\0';
                    long long iv = 0;
                    if (shellParseSignedLongLong(arg_text, &iv)) {
                        ch = (char)iv;
                    } else if (arg_text && *arg_text) {
                        ch = arg_text[0];
                    }
                    const char *format = fmtbuf;
                    char safe_fmt[sizeof(fmtbuf)];
                    if (has_wide_char_length) {
                        strncpy(safe_fmt, fmtbuf, sizeof(safe_fmt));
                        safe_fmt[sizeof(safe_fmt) - 1] = '\0';
                        char *mod_pos = strstr(safe_fmt, length_mod);
                        if (mod_pos) {
                            size_t remove_len = strlen(length_mod);
                            memmove(mod_pos, mod_pos + remove_len, strlen(mod_pos + remove_len) + 1);
                            format = safe_fmt;
                        }
                    }
                    if (width_from_arg && precision_from_arg) {
                        snprintf(buf, sizeof(buf), format, width_value, precision_value, ch);
                    } else if (width_from_arg) {
                        snprintf(buf, sizeof(buf), format, width_value, ch);
                    } else if (precision_from_arg) {
                        snprintf(buf, sizeof(buf), format, precision_value, ch);
                    } else {
                        snprintf(buf, sizeof(buf), format, ch);
                    }
                    fputs(buf, stdout);
                    break;
                }
                case 's': {
                    const char *sv = arg_text ? arg_text : "";
                    const char *format = fmtbuf;
                    char safe_fmt[sizeof(fmtbuf)];
                    if (has_wide_char_length) {
                        strncpy(safe_fmt, fmtbuf, sizeof(safe_fmt));
                        safe_fmt[sizeof(safe_fmt) - 1] = '\0';
                        char *mod_pos = strstr(safe_fmt, length_mod);
                        if (mod_pos) {
                            size_t remove_len = strlen(length_mod);
                            memmove(mod_pos, mod_pos + remove_len, strlen(mod_pos + remove_len) + 1);
                            format = safe_fmt;
                        }
                    }
                    if (width_from_arg && precision_from_arg) {
                        snprintf(buf, sizeof(buf), format, width_value, precision_value, sv);
                    } else if (width_from_arg) {
                        snprintf(buf, sizeof(buf), format, width_value, sv);
                    } else if (precision_from_arg) {
                        snprintf(buf, sizeof(buf), format, precision_value, sv);
                    } else {
                        snprintf(buf, sizeof(buf), format, sv);
                    }
                    fputs(buf, stdout);
                    break;
                }
                case 'p': {
                    unsigned long long pv = 0;
                    if (!shellParseUnsignedLongLong(arg_text, &pv)) {
                        shellReportRecoverableError(vm, false, "printf: %s: invalid number", arg_text);
                        pv = 0;
                        ok = false;
                    }
                    if (width_from_arg && precision_from_arg) {
                        snprintf(buf, sizeof(buf), fmtbuf, width_value, precision_value, (void *)(uintptr_t)pv);
                    } else if (width_from_arg) {
                        snprintf(buf, sizeof(buf), fmtbuf, width_value, (void *)(uintptr_t)pv);
                    } else if (precision_from_arg) {
                        snprintf(buf, sizeof(buf), fmtbuf, precision_value, (void *)(uintptr_t)pv);
                    } else {
                        snprintf(buf, sizeof(buf), fmtbuf, (void *)(uintptr_t)pv);
                    }
                    fputs(buf, stdout);
                    break;
                }
                default: {
                    const char *sv = arg_text ? arg_text : "";
                    if (width_from_arg && precision_from_arg) {
                        snprintf(buf, sizeof(buf), fmtbuf, width_value, precision_value, sv);
                    } else if (width_from_arg) {
                        snprintf(buf, sizeof(buf), fmtbuf, width_value, sv);
                    } else if (precision_from_arg) {
                        snprintf(buf, sizeof(buf), fmtbuf, precision_value, sv);
                    } else {
                        snprintf(buf, sizeof(buf), fmtbuf, sv);
                    }
                    fputs(buf, stdout);
                    break;
                }
            }

            i = j;
            continue;
        }

        fputc(c, stdout);
    }

        if (!ok) {
            repeat = false;
        } else {
            repeat = fmt && arg_index < arg_count && consumed_conversion;
        }
    } while (repeat);

    fflush(stdout);
    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellRead(VM *vm, int arg_count, Value *args) {
    const char *prompt = NULL;
    const char *array_name = NULL;
    const char **variables = NULL;
    size_t variable_count = 0;
    bool parsing_options = true;
    bool ok = true;
    bool raw_mode = false;

    for (int i = 0; i < arg_count && ok; ++i) {
        Value val = args[i];
        if (val.type != TYPE_STRING || !val.s_val) {
            runtimeError(vm, "read: arguments must be strings");
            ok = false;
            break;
        }
        const char *token = val.s_val;
        if (parsing_options) {
            if (strcmp(token, "--") == 0) {
                parsing_options = false;
                continue;
            }
            if (token[0] == '-' && token[1] != '\0') {
                size_t option_length = strlen(token);
                bool pending_prompt = false;
                bool pending_array = false;
                for (size_t opt_index = 1; opt_index < option_length && ok; ++opt_index) {
                    char opt = token[opt_index];
                    switch (opt) {
                        case 'r':
                            raw_mode = true;
                            break;
                        case 'p':
                            pending_prompt = true;
                            break;
                        case 'a':
                            if (array_name) {
                                runtimeError(vm, "read: option -a specified multiple times");
                                ok = false;
                                break;
                            }
                            if (opt_index + 1 < option_length) {
                                array_name = &token[opt_index + 1];
                                opt_index = option_length;
                            } else {
                                pending_array = true;
                            }
                            break;
                        default:
                            runtimeError(vm, "read: unsupported option '-%c'", opt);
                            ok = false;
                            break;
                    }
                }
                if (!ok) {
                    break;
                }
                if (pending_prompt) {
                    if (i + 1 >= arg_count) {
                        runtimeError(vm, "read: option -p requires an argument");
                        ok = false;
                        break;
                    }
                    Value prompt_val = args[++i];
                    if (prompt_val.type != TYPE_STRING || !prompt_val.s_val) {
                        runtimeError(vm, "read: prompt must be a string");
                        ok = false;
                        break;
                    }
                    prompt = prompt_val.s_val;
                }
                if (!ok) {
                    break;
                }
                if (pending_array) {
                    if (i + 1 >= arg_count) {
                        runtimeError(vm, "read: option -a requires an argument");
                        ok = false;
                        break;
                    }
                    Value array_val = args[++i];
                    if (array_val.type != TYPE_STRING || !array_val.s_val || array_val.s_val[0] == '\0') {
                        runtimeError(vm, "read: array name must be a non-empty string");
                        ok = false;
                        break;
                    }
                    array_name = array_val.s_val;
                }
                continue;
            }
            parsing_options = false;
        }
        const char **resized = (const char **)realloc(variables, (variable_count + 1) * sizeof(const char *));
        if (!resized) {
            runtimeError(vm, "read: out of memory");
            ok = false;
            break;
        }
        variables = resized;
        variables[variable_count++] = token;
    }

    if (ok && variable_count == 0 && !array_name) {
        variables = (const char **)malloc(sizeof(const char *));
        if (!variables) {
            runtimeError(vm, "read: out of memory");
            ok = false;
        } else {
            variables[0] = "REPLY";
            variable_count = 1;
        }
    }

    const char *ifs = shellReadResolveIFS();

    ShellReadLineResult read_result = SHELL_READ_LINE_ERROR;
    char *line = NULL;
    size_t line_length = 0;
    if (ok) {
        if (prompt) {
            write(STDOUT_FILENO, prompt, strlen(prompt));
        }
        read_result = shellReadLineFromFd(STDIN_FILENO, &line, &line_length);
        if (read_result == SHELL_READ_LINE_OK && line_length > 0 && line[line_length - 1] == '\n') {
            line[--line_length] = '\0';
        }
        if (read_result == SHELL_READ_LINE_ERROR) {
            runtimeError(vm, "read: failed to read input");
        }
    }

    bool assign_ok = ok;
    char *cursor = line;
    if (ok && array_name) {
        char *array_cursor = line;
        char **array_values = NULL;
        size_t array_count = 0;
        size_t array_capacity = 0;
        bool has_non_whitespace = false;
        if (line) {
            for (const char *scan = line; *scan; ++scan) {
                if (!shellReadIsIFSWhitespaceDelimiter(ifs, *scan)) {
                    has_non_whitespace = true;
                    break;
                }
            }
        }
        if (read_result == SHELL_READ_LINE_OK && array_cursor) {
            while (*array_cursor) {
                char *value_copy = shellReadExtractField(&array_cursor, false, raw_mode, ifs);
                if (!value_copy) {
                    runtimeError(vm, "read: out of memory");
                    assign_ok = false;
                    break;
                }
                if (!has_non_whitespace && value_copy[0] == '\0') {
                    free(value_copy);
                    continue;
                }
                if (!shellAppendArrayValue(&array_values, &array_count, &array_capacity, value_copy)) {
                    runtimeError(vm, "read: out of memory");
                    assign_ok = false;
                    break;
                }
            }
        }
        if (assign_ok) {
            if (!shellArrayRegistryStore(array_name, array_values, NULL, array_count, SHELL_ARRAY_KIND_INDEXED)) {
                runtimeError(vm, "read: out of memory");
                assign_ok = false;
            } else {
                const ShellArrayVariable *stored = shellArrayRegistryFindConst(array_name);
                char *literal = shellBuildArrayLiteral(stored);
                if (!literal) {
                    runtimeError(vm, "read: out of memory");
                    shellArrayRegistryRemove(array_name);
                    assign_ok = false;
                } else {
                    if (setenv(array_name, literal, 1) != 0) {
                        int err = errno;
                        runtimeError(vm, "read: unable to set array '%s': %s", array_name, strerror(err));
                        shellArrayRegistryRemove(array_name);
                        assign_ok = false;
                    }
                    free(literal);
                }
            }
        }
        shellFreeArrayValues(array_values, array_count);
        array_values = NULL;
        if (read_result == SHELL_READ_LINE_OK) {
            cursor = array_cursor;
        }
    }

    if (ok && (read_result == SHELL_READ_LINE_OK || read_result == SHELL_READ_LINE_EOF)) {
        for (size_t i = 0; i < variable_count; ++i) {
            bool last = (i + 1 == variable_count);
            char *value_copy = NULL;
            if (read_result == SHELL_READ_LINE_OK) {
                value_copy = shellReadExtractField(&cursor, last, raw_mode, ifs);
            } else {
                value_copy = strdup("");
            }
            if (!value_copy) {
                runtimeError(vm, "read: out of memory");
                assign_ok = false;
                break;
            }
            if (!shellSetTrackedVariable(variables[i], value_copy, false)) {
                runtimeError(vm, "read: unable to set '%s': %s", variables[i], strerror(errno));
                free(value_copy);
                assign_ok = false;
                break;
            }
            free(value_copy);
        }
    }

    free(line);
    free(variables);

    if (!ok || !assign_ok || read_result != SHELL_READ_LINE_OK) {
        shellUpdateStatus(1);
    } else {
        shellUpdateStatus(0);
    }
    return makeVoid();
}

static void shellGetoptsSyncIndex(void) {
    char *text = shellLookupParameterValue("OPTIND", strlen("OPTIND"));
    if (text) {
        char *end = NULL;
        long parsed = strtol(text, &end, 10);
        if (end && *end == '\0' && parsed >= 1 && parsed <= INT_MAX) {
            int previous = gShellRuntime.getopts_index;
            gShellRuntime.getopts_index = (int)parsed;
            if (gShellRuntime.getopts_index != previous || gShellRuntime.getopts_offset < 1) {
                gShellRuntime.getopts_offset = 1;
            }
        }
        free(text);
    } else if (gShellRuntime.getopts_index < 1) {
        gShellRuntime.getopts_offset = 1;
        shellGetoptsUpdateOptind(1);
    }
}

Value vmBuiltinShellGetopts(VM *vm, int arg_count, Value *args) {
    if (arg_count < 2) {
        runtimeError(vm, "getopts: expected OPTSTRING NAME [ARGS...]");
        shellUpdateStatus(2);
        return makeVoid();
    }
    if (args[0].type != TYPE_STRING || !args[0].s_val) {
        runtimeError(vm, "getopts: option string must be provided");
        shellUpdateStatus(2);
        return makeVoid();
    }
    if (args[1].type != TYPE_STRING || !args[1].s_val || args[1].s_val[0] == '\0') {
        runtimeError(vm, "getopts: variable name must be a non-empty string");
        shellUpdateStatus(2);
        return makeVoid();
    }

    const char *optstring = args[0].s_val;
    bool silent_errors = false;
    if (optstring[0] == ':') {
        silent_errors = true;
        optstring++;
    }
    const char *name = args[1].s_val;

    char **argv_list = NULL;
    int argc_list = 0;
    char **override_args = NULL;
    if (arg_count > 2) {
        argc_list = arg_count - 2;
        override_args = (char **)calloc((size_t)argc_list, sizeof(char *));
        if (!override_args) {
            runtimeError(vm, "getopts: out of memory");
            shellUpdateStatus(2);
            return makeVoid();
        }
        bool valid = true;
        for (int i = 0; i < argc_list; ++i) {
            Value val = args[i + 2];
            if (val.type != TYPE_STRING || !val.s_val) {
                valid = false;
                break;
            }
            override_args[i] = val.s_val;
        }
        if (!valid) {
            free(override_args);
            runtimeError(vm, "getopts: arguments must be strings");
            shellUpdateStatus(2);
            return makeVoid();
        }
        argv_list = override_args;
    } else {
        argv_list = gParamValues;
        argc_list = gParamCount;
    }

    if (!argv_list || argc_list <= 0) {
        gShellRuntime.getopts_offset = 1;
        shellGetoptsUpdateOptind(1);
        shellUpdateStatus(1);
        return makeVoid();
    }

    shellGetoptsSyncIndex();
    int optind = gShellRuntime.getopts_index;
    if (optind < 1) {
        optind = 1;
    }

    while (optind <= argc_list) {
        const char *current = argv_list[optind - 1];
        if (!current) {
            optind++;
            gShellRuntime.getopts_offset = 1;
            continue;
        }
        if (strcmp(current, "--") == 0) {
            optind++;
            shellGetoptsSetOptarg(NULL);
            gShellRuntime.getopts_offset = 1;
            shellGetoptsUpdateOptind(optind);
            shellUpdateStatus(1);
            free(override_args);
            return makeVoid();
        }
        if (current[0] != '-' || current[1] == '\0') {
            shellGetoptsSetOptarg(NULL);
            gShellRuntime.getopts_offset = 1;
            shellGetoptsUpdateOptind(optind);
            shellUpdateStatus(1);
            free(override_args);
            return makeVoid();
        }

        int offset = gShellRuntime.getopts_offset;
        if (offset < 1) {
            offset = 1;
        }
        char option = current[offset];
        if (option == '\0') {
            optind++;
            gShellRuntime.getopts_offset = 1;
            continue;
        }
        const char *rest = current + offset + 1;
        bool has_more = *rest != '\0';
        const char *match = strchr(optstring, option);
        if (!match || option == ':') {
            char optbuf[2] = {option ? option : '?', '\0'};
            shellSetTrackedVariable(name, "?", false);
            shellGetoptsSetOptarg(optbuf);
            if (!silent_errors && option) {
                fprintf(stderr, "getopts: illegal option -- %c\n", option);
            }
            if (has_more) {
                gShellRuntime.getopts_offset = offset + 1;
            } else {
                optind++;
                gShellRuntime.getopts_offset = 1;
            }
            shellGetoptsUpdateOptind(optind);
            shellUpdateStatus(0);
            free(override_args);
            return makeVoid();
        }

        bool expects_arg = (match[1] == ':');
        char *optarg_value = NULL;
        if (expects_arg) {
            if (has_more) {
                optarg_value = strdup(rest);
                optind++;
                gShellRuntime.getopts_offset = 1;
            } else if (optind < argc_list) {
                const char *next = argv_list[optind];
                optarg_value = strdup(next ? next : "");
                optind += 2;
                gShellRuntime.getopts_offset = 1;
            } else {
                char optbuf[2] = {option, '\0'};
                if (silent_errors) {
                    shellSetTrackedVariable(name, ":", false);
                } else {
                    shellSetTrackedVariable(name, "?", false);
                    fprintf(stderr, "getopts: option requires an argument -- %c\n", option);
                }
                shellGetoptsSetOptarg(optbuf);
                optind++;
                gShellRuntime.getopts_offset = 1;
                shellGetoptsUpdateOptind(optind);
                shellUpdateStatus(0);
                free(override_args);
                return makeVoid();
            }
        } else {
            if (has_more) {
                gShellRuntime.getopts_offset = offset + 1;
            } else {
                optind++;
                gShellRuntime.getopts_offset = 1;
            }
        }

        char optchar[2] = {option, '\0'};
        shellSetTrackedVariable(name, optchar, false);
        if (optarg_value) {
            shellGetoptsSetOptarg(optarg_value);
            free(optarg_value);
        } else {
            shellGetoptsSetOptarg(NULL);
        }
        shellGetoptsUpdateOptind(optind);
        shellUpdateStatus(0);
        free(override_args);
        return makeVoid();
    }

    shellGetoptsSetOptarg(NULL);
    gShellRuntime.getopts_offset = 1;
    shellGetoptsUpdateOptind(optind);
    shellUpdateStatus(1);
    free(override_args);
    return makeVoid();
}


static bool shellMapfileStoreArray(const char *name, char **values, size_t count) {
    if (!name) {
        name = "MAPFILE";
    }
    if (!shellArrayRegistryStore(name, values, NULL, count, SHELL_ARRAY_KIND_INDEXED)) {
        return false;
    }
    const ShellArrayVariable *stored = shellArrayRegistryFindConst(name);
    if (!stored) {
        return false;
    }
    char *literal = shellBuildArrayLiteral(stored);
    if (!literal) {
        shellArrayRegistryRemove(name);
        return false;
    }
    if (setenv(name, literal, 1) != 0) {
        shellArrayRegistryRemove(name);
        free(literal);
        return false;
    }
    free(literal);
    return true;
}

Value vmBuiltinShellMapfile(VM *vm, int arg_count, Value *args) {
    bool strip_newline = false;
    const char *array_name = "MAPFILE";
    int index = 0;

    while (index < arg_count) {
        Value arg = args[index];
        if (arg.type != TYPE_STRING || !arg.s_val) {
            runtimeError(vm, "mapfile: invalid argument");
            shellUpdateStatus(2);
            return makeVoid();
        }
        if (strcmp(arg.s_val, "-t") == 0) {
            strip_newline = true;
            index++;
            continue;
        }
        if (arg.s_val[0] == '-') {
            runtimeError(vm, "mapfile: unsupported option '%s'", arg.s_val);
            shellUpdateStatus(2);
            return makeVoid();
        }
        array_name = arg.s_val;
        index++;
        break;
    }

    if (index < arg_count) {
        runtimeError(vm, "mapfile: unexpected argument '%s'", args[index].s_val);
        shellUpdateStatus(2);
        return makeVoid();
    }

    char **values = NULL;
    size_t count = 0;
    size_t capacity = 0;
    bool ok = true;

    for (;;) {
        char *line = NULL;
        size_t line_length = 0;
        ShellReadLineResult result = shellReadLineFromFd(STDIN_FILENO, &line, &line_length);
        if (result == SHELL_READ_LINE_ERROR) {
            runtimeError(vm, "mapfile: failed to read input");
            ok = false;
            free(line);
            break;
        }
        if (result == SHELL_READ_LINE_EOF) {
            free(line);
            break;
        }
        if (strip_newline) {
            while (line_length > 0 &&
                   (line[line_length - 1] == '\n' || line[line_length - 1] == '\r')) {
                line[--line_length] = '\0';
            }
        }
        if (!shellAppendArrayValue(&values, &count, &capacity, line)) {
            runtimeError(vm, "mapfile: out of memory");
            free(line);
            ok = false;
            break;
        }
    }

    if (ok) {
        if (!shellMapfileStoreArray(array_name, values, count)) {
            runtimeError(vm, "mapfile: unable to store results");
            ok = false;
        }
    }

    shellFreeArrayValues(values, count);
    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellShift(VM *vm, int arg_count, Value *args) {
    int shift_count = 1;
    if (arg_count > 1) {
        runtimeError(vm, "shift: expected optional non-negative count");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (arg_count == 1) {
        Value v = args[0];
        if (v.type != TYPE_STRING || !v.s_val || !*v.s_val) {
            runtimeError(vm, "shift: expected numeric argument");
            shellUpdateStatus(1);
            return makeVoid();
        }
        char *end = NULL;
        errno = 0;
        long parsed = strtol(v.s_val, &end, 10);
        if (errno != 0 || !end || *end != '\0' || parsed < 0 || parsed > INT_MAX) {
            runtimeError(vm, "shift: invalid count '%s'", v.s_val);
            shellUpdateStatus(1);
            return makeVoid();
        }
        shift_count = (int)parsed;
    }

    if (shift_count == 0) {
        shellUpdateStatus(0);
        return makeVoid();
    }

    if (shift_count > gParamCount || gParamCount <= 0 || !gParamValues) {
        runtimeError(vm, "shift: count out of range");
        shellUpdateStatus(1);
        return makeVoid();
    }

    for (int i = 0; i + shift_count < gParamCount; ++i) {
        gParamValues[i] = gParamValues[i + shift_count];
    }
    for (int i = gParamCount - shift_count; i < gParamCount; ++i) {
        if (i >= 0) {
            gParamValues[i] = NULL;
        }
    }
    gParamCount -= shift_count;
    if (gParamCount < 0) {
        gParamCount = 0;
    }
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellSetenv(VM *vm, int arg_count, Value *args) {
    if (arg_count == 0) {
        if (environ) {
            for (char **env = environ; *env; ++env) {
                puts(*env);
            }
        }
        shellUpdateStatus(0);
        return makeVoid();
    }
    if (arg_count < 1 || arg_count > 2) {
        runtimeError(vm, "setenv: expected NAME [VALUE]");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (args[0].type != TYPE_STRING || !args[0].s_val || args[0].s_val[0] == '\0') {
        runtimeError(vm, "setenv: variable name must be a non-empty string");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (!shellIsValidEnvName(args[0].s_val)) {
        runtimeError(vm, "setenv: invalid variable name '%s'", args[0].s_val);
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (strchr(args[0].s_val, '=')) {
        runtimeError(vm, "setenv: variable name must not contain '='");
        shellUpdateStatus(1);
        return makeVoid();
    }
    const char *value = "";
    if (arg_count > 1) {
        if (args[1].type != TYPE_STRING || !args[1].s_val) {
            runtimeError(vm, "setenv: value must be a string");
            shellUpdateStatus(1);
            return makeVoid();
        }
        value = args[1].s_val;
    }
    if (!shellSetTrackedVariable(args[0].s_val, value, false)) {
        runtimeError(vm, "setenv: unable to set '%s': %s", args[0].s_val, strerror(errno));
        shellUpdateStatus(1);
        return makeVoid();
    }
    shellUpdateStatus(0);
    return makeVoid();
}

static ShellBindOption *shellBindFindOption(const char *name) {
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < gShellBindOptionCount; ++i) {
        ShellBindOption *entry = &gShellBindOptions[i];
        if (entry->name && strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

static bool shellBindSetOptionOwned(char *name, char *value) {
    if (!name) {
        free(value);
        return false;
    }
    if (!value) {
        value = strdup("");
        if (!value) {
            free(name);
            return false;
        }
    }
    ShellBindOption *existing = shellBindFindOption(name);
    if (existing) {
        free(existing->value);
        existing->value = value;
        free(name);
        return true;
    }
    ShellBindOption *resized = (ShellBindOption *)realloc(
        gShellBindOptions, (gShellBindOptionCount + 1) * sizeof(ShellBindOption));
    if (!resized) {
        free(name);
        free(value);
        return false;
    }
    gShellBindOptions = resized;
    gShellBindOptions[gShellBindOptionCount].name = name;
    gShellBindOptions[gShellBindOptionCount].value = value;
    gShellBindOptionCount++;
    return true;
}

static void shellBindPrintOptions(void) {
    for (size_t i = 0; i < gShellBindOptionCount; ++i) {
        ShellBindOption *entry = &gShellBindOptions[i];
        if (!entry->name) {
            continue;
        }
        const char *value = entry->value ? entry->value : "";
        printf("set %s %s\n", entry->name, value);
    }
}

static ShellOptionEntry *shellShoptFindOption(const char *name) {
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < gShellOptionCount; ++i) {
        ShellOptionEntry *entry = &gShellOptions[i];
        if (entry->name && strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

static bool shellShoptOptionEnabled(const char *name) {
    ShellOptionEntry *entry = shellShoptFindOption(name);
    return entry && entry->enabled;
}

static void shellShoptPrintEntry(const ShellOptionEntry *entry) {
    if (!entry || !entry->name) {
        return;
    }
    printf("%s\t%s\n", entry->name, entry->enabled ? "on" : "off");
}

static void shellShoptPrintEntryAsCommand(const ShellOptionEntry *entry) {
    if (!entry || !entry->name) {
        return;
    }
    printf("shopt -%c %s\n", entry->enabled ? 's' : 'u', entry->name);
}

Value vmBuiltinShellBind(VM *vm, int arg_count, Value *args) {
    bool ok = true;
    bool print_bindings = false;
    int index = 0;
    bool parsing_options = true;
    bool interactive = shellRuntimeIsInteractive();
    while (index < arg_count && parsing_options && ok) {
        Value v = args[index];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "bind: arguments must be strings");
            ok = false;
            break;
        }
        const char *token = v.s_val;
        if (strcmp(token, "--") == 0) {
            parsing_options = false;
            index++;
            break;
        }
        if (token[0] == '-' && token[1] != '\0') {
            size_t len = strlen(token);
            for (size_t i = 1; i < len && ok; ++i) {
                char opt = token[i];
                if (opt == 'p') {
                    print_bindings = true;
                } else {
                    runtimeError(vm, "bind: unsupported option '-%c'", opt);
                    ok = false;
                    break;
                }
            }
            index++;
            continue;
        }
        parsing_options = false;
    }

    for (; ok && index < arg_count; ++index) {
        Value v = args[index];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "bind: arguments must be strings");
            ok = false;
            break;
        }
        const char *text = v.s_val;
        if (strncmp(text, "set", 3) == 0) {
            const char *cursor = text + 3;
            if (*cursor && !isspace((unsigned char)*cursor)) {
                continue;
            }
            while (*cursor && isspace((unsigned char)*cursor)) {
                cursor++;
            }
            if (*cursor == '\0') {
                runtimeError(vm, "bind: expected readline option name");
                ok = false;
                break;
            }
            const char *name_start = cursor;
            while (*cursor && !isspace((unsigned char)*cursor)) {
                cursor++;
            }
            size_t name_len = (size_t)(cursor - name_start);
            while (*cursor && isspace((unsigned char)*cursor)) {
                cursor++;
            }
            const char *value_start = cursor;
            const char *value_end = value_start + strlen(value_start);
            while (value_end > value_start && isspace((unsigned char)value_end[-1])) {
                value_end--;
            }
            size_t value_len = (size_t)(value_end - value_start);
            char *name_copy = strndup(name_start, name_len);
            char *value_copy = strndup(value_start, value_len);
            if (!name_copy || !value_copy) {
                free(name_copy);
                free(value_copy);
                runtimeError(vm, "bind: out of memory");
                ok = false;
                break;
            }
            if (!shellBindSetOptionOwned(name_copy, value_copy)) {
                runtimeError(vm, "bind: out of memory");
                ok = false;
                break;
            }
        }
    }

    if (ok && print_bindings) {
        shellBindPrintOptions();
    }

    bool requires_interactive = shellBindRequiresInteractive();

    if (ok && !interactive && requires_interactive) {
        const char *script = shellRuntimeGetArg0();
        if (!script || !*script) {
            script = "exsh";
        }
        int line = shellRuntimeCurrentCommandLine();
        if (line > 0) {
            fprintf(stderr, "%s: line %d: bind: warning: line editing not enabled\n", script, line);
        } else {
            fprintf(stderr, "%s: bind: warning: line editing not enabled\n", script);
        }
    }

    int status = ok ? 0 : 1;
    shellUpdateStatus(status);
    return makeVoid();
}

Value vmBuiltinShellShopt(VM *vm, int arg_count, Value *args) {
    bool ok = true;
    bool quiet = false;
    bool print_format = false;
    bool parsing_options = true;
    int mode = -1; // -1 query, 0 unset, 1 set
    bool restrict_set_options = false;
    int index = 0;

    while (index < arg_count && parsing_options && ok) {
        Value v = args[index];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "shopt: option names must be strings");
            ok = false;
            break;
        }
        const char *token = v.s_val;
        if (strcmp(token, "--") == 0) {
            parsing_options = false;
            index++;
            break;
        }
        if (token[0] == '-' && token[1] != '\0') {
            size_t len = strlen(token);
            for (size_t i = 1; i < len && ok; ++i) {
                char opt = token[i];
                switch (opt) {
                    case 's':
                        mode = 1;
                        break;
                    case 'u':
                        mode = 0;
                        break;
                    case 'q':
                        quiet = true;
                        break;
                    case 'p':
                        print_format = true;
                        break;
                    case 'o':
                        restrict_set_options = true;
                        break;
                    default:
                        runtimeError(vm, "shopt: invalid option '-%c'", opt);
                        ok = false;
                        break;
                }
            }
            index++;
            continue;
        }
        parsing_options = false;
    }

    if (!ok) {
        shellUpdateStatus(1);
        return makeVoid();
    }

    if (restrict_set_options) {
        runtimeError(vm, "shopt: -o is not supported");
        shellUpdateStatus(1);
        return makeVoid();
    }

    if (index >= arg_count) {
        if (mode == 1 || mode == 0) {
            for (size_t i = 0; i < gShellOptionCount; ++i) {
                ShellOptionEntry *entry = &gShellOptions[i];
                if ((mode == 1 && entry->enabled) || (mode == 0 && !entry->enabled)) {
                    if (print_format) {
                        shellShoptPrintEntryAsCommand(entry);
                    } else {
                        shellShoptPrintEntry(entry);
                    }
                }
            }
            shellUpdateStatus(0);
            return makeVoid();
        }
        if (quiet) {
            shellUpdateStatus(0);
            return makeVoid();
        }
        for (size_t i = 0; i < gShellOptionCount; ++i) {
            ShellOptionEntry *entry = &gShellOptions[i];
            if (print_format) {
                shellShoptPrintEntryAsCommand(entry);
            } else {
                shellShoptPrintEntry(entry);
            }
        }
        shellUpdateStatus(0);
        return makeVoid();
    }

    bool query_only = (mode == -1);
    bool all_set = true;

    for (; ok && index < arg_count; ++index) {
        Value v = args[index];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "shopt: option names must be strings");
            ok = false;
            break;
        }
        ShellOptionEntry *entry = shellShoptFindOption(v.s_val);
        if (!entry) {
            runtimeError(vm, "shopt: %s: invalid shell option name", v.s_val);
            ok = false;
            break;
        }
        if (query_only) {
            if (!entry->enabled) {
                all_set = false;
            }
            if (!quiet) {
                if (print_format) {
                    shellShoptPrintEntryAsCommand(entry);
                } else {
                    shellShoptPrintEntry(entry);
                }
            }
        } else {
            entry->enabled = (mode == 1);
        }
    }

    if (!ok) {
        shellUpdateStatus(1);
        return makeVoid();
    }

    if (query_only) {
        shellUpdateStatus(all_set ? 0 : 1);
    } else {
        shellUpdateStatus(0);
    }
    return makeVoid();
}

static void shellDeclareExtractErrorToken(const char *start,
                                          const char *end,
                                          char *buffer,
                                          size_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';
    if (!start || !end || end <= start) {
        return;
    }
    const char *cursor = end;
    while (cursor > start) {
        cursor--;
        unsigned char ch = (unsigned char)*cursor;
        if (isspace(ch)) {
            continue;
        }
        const char *token_start = cursor;
        if (isalnum(ch) || ch == '_') {
            while (token_start > start) {
                unsigned char prev = (unsigned char)token_start[-1];
                if (!isalnum(prev) && prev != '_') {
                    break;
                }
                token_start--;
            }
        } else {
            bool two_char_op = false;
            if (token_start > start) {
                unsigned char prev = (unsigned char)token_start[-1];
                if ((ch == '=' && (prev == '!' || prev == '=' || prev == '<' || prev == '>')) ||
                    (ch == '&' && prev == '&') || (ch == '|' && prev == '|') ||
                    (ch == '+' && prev == '+') || (ch == '-' && prev == '-')) {
                    token_start--;
                    two_char_op = true;
                }
            }
            if (!two_char_op && (ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '%')) {
                token_start = cursor;
            }
        }
        size_t token_len = (size_t)((cursor + 1) - token_start);
        if (token_len >= buffer_size) {
            token_len = buffer_size - 1;
        }
        memcpy(buffer, token_start, token_len);
        buffer[token_len] = '\0';
        return;
    }
}

Value vmBuiltinShellDeclare(VM *vm, int arg_count, Value *args) {
    const char *builtin_name = shellRuntimeCurrentCommandName();
    if (!builtin_name || !*builtin_name) {
        builtin_name = (vm && vm->current_builtin_name) ? vm->current_builtin_name : "declare";
    }
    bool ok = true;
    bool associative = false;
    bool global_scope = false;
    bool mark_readonly = false;
    bool integer_attribute = false;
    bool in_function_scope = shellLocalScopeDepth() > 0;
    bool request_local_scope = gShellRuntime.local_scope_active;
    if (builtin_name && strcmp(builtin_name, "typeset") == 0 && in_function_scope) {
        request_local_scope = true;
    }
    int exit_status = 0;
    int index = 0;
    while (index < arg_count) {
        Value v = args[index];
        if (v.type != TYPE_STRING || !v.s_val) {
            break;
        }
        const char *token = v.s_val;
        if (strcmp(token, "--") == 0) {
            index++;
            break;
        }
        if (token[0] != '-' || token[1] == '\0') {
            break;
        }
        for (size_t i = 1; token[i] != '\0'; ++i) {
            char opt = token[i];
            if (opt == 'A' && token[0] == '-') {
                if (!shellAssociativeArraysSupported()) {
                    shellReportRecoverableError(vm, true, "declare: -%c: invalid option", opt);
                    shellReportRecoverableError(
                        vm,
                        true,
                        "declare: usage: declare [-afFirtx] [-p] [name[=value] ...]");
                    if (exit_status == 0) {
                        exit_status = 2;
                    }
                    continue;
                }
                associative = true;
            } else if (opt == 'A' && token[0] == '+') {
                associative = false;
            } else if (opt == 'g' && token[0] == '-') {
                global_scope = true;
            } else if (opt == 'g' && token[0] == '+') {
                global_scope = false;
            } else if (opt == 'r' && token[0] == '-') {
                mark_readonly = true;
            } else if (opt == 'r' && token[0] == '+') {
                runtimeError(vm, "declare: -%c: unsupported option", opt);
                ok = false;
                break;
            } else if (opt == 'i' && token[0] == '-') {
                integer_attribute = true;
            } else if (opt == 'i' && token[0] == '+') {
                integer_attribute = false;
            } else {
                runtimeError(vm, "declare: -%c: unsupported option", opt);
                ok = false;
                break;
            }
        }
        if (!ok) {
            break;
        }
        index++;
    }

    bool use_local_scope = false;
    if (request_local_scope && !in_function_scope) {
        runtimeError(vm, "%s: can only be used in functions", builtin_name);
        ok = false;
    } else if (request_local_scope && in_function_scope && !global_scope) {
        use_local_scope = true;
    }

    for (; index < arg_count && ok; ++index) {
        Value v = args[index];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "declare: expected string argument");
            ok = false;
            break;
        }
        const char *spec = v.s_val;
        const char *eq = strchr(spec, '=');
        if (!eq) {
            if (associative) {
                if (use_local_scope) {
                    runtimeError(vm, "%s: local associative arrays are not supported", builtin_name);
                    ok = false;
                } else {
                    if (!shellArrayRegistryInitializeAssociative(spec)) {
                        runtimeError(vm, "declare: unable to initialise '%s'", spec);
                        ok = false;
                    } else if (setenv(spec, "", 1) != 0) {
                        runtimeError(vm, "declare: unable to set '%s'", spec);
                        ok = false;
                    }
                }
            } else {
                if (use_local_scope) {
                    if (!shellLocalScopeSetOnCurrentFrame(spec, "", false)) {
                        runtimeError(vm, "declare: out of memory");
                        ok = false;
                    }
                } else {
                    // Without an assignment, bash preserves the current value of global
                    // variables when applying attributes. Leave the value untouched so
                    // constructs like `typeset -i var` do not reset an existing binding.
                    if (shellReadonlyContains(spec)) {
                        runtimeError(vm, "declare: %s: readonly variable", spec);
                        ok = false;
                    } else {
                        const ShellArrayVariable *array_var = shellArrayRegistryFindConst(spec);
                        bool exists = (array_var != NULL);
                        if (!exists) {
                            const char *existing = shellSafeGetenv(spec);
                            exists = (existing != NULL);
                        }
                        if (!exists) {
                            const char *initial = integer_attribute ? "0" : "";
                            errno = 0;
                            if (!shellSetTrackedVariable(spec, initial, false)) {
                                if (errno != 0) {
                                    runtimeError(vm,
                                                "declare: unable to set '%s': %s",
                                                spec,
                                                strerror(errno));
                                } else {
                                    runtimeError(vm, "declare: unable to set '%s'", spec);
                                }
                                ok = false;
                            }
                        }
                    }
                }
            }
            if (ok && mark_readonly) {
                if (!shellReadonlyAdd(spec)) {
                    runtimeError(vm, "declare: out of memory");
                    ok = false;
                }
            }
            continue;
        }
        size_t name_len = (size_t)(eq - spec);
        char *name = (char *)malloc(name_len + 1);
        if (!name) {
            ok = false;
            break;
        }
        memcpy(name, spec, name_len);
        name[name_len] = '\0';
        const char *value_text = eq + 1;
        char *coerced_value = NULL;
        const char *value_to_store = value_text;
        if (integer_attribute) {
            const char *trim_start = value_text;
            while (trim_start && *trim_start && isspace((unsigned char)*trim_start)) {
                trim_start++;
            }
            const char *trim_end = value_text + strlen(value_text);
            while (trim_end > trim_start && isspace((unsigned char)trim_end[-1])) {
                trim_end--;
            }
            size_t trimmed_len = (size_t)(trim_end - trim_start);
            if (trimmed_len == 0) {
                coerced_value = strdup("0");
                if (!coerced_value) {
                    runtimeError(vm, "declare: out of memory");
                    free(name);
                    ok = false;
                    break;
                }
                value_to_store = coerced_value;
            } else {
                bool eval_error = false;
                char *result = shellEvaluateArithmetic(value_text, &eval_error);
                if (!result || eval_error) {
                    char expr_buffer[256];
                    if (trimmed_len >= sizeof(expr_buffer)) {
                        trimmed_len = sizeof(expr_buffer) - 1;
                    }
                    memcpy(expr_buffer, trim_start, trimmed_len);
                    expr_buffer[trimmed_len] = '\0';

                    char token_buffer[256];
                    shellDeclareExtractErrorToken(trim_start, trim_end, token_buffer, sizeof(token_buffer));
                    const char *error_fragment = (token_buffer[0] != '\0') ? token_buffer : expr_buffer;

                    const char *error_format = shellTypesetArithmeticErrorFormat();
                    shellReportRecoverableError(vm,
                                                true,
                                                error_format,
                                                builtin_name,
                                                expr_buffer,
                                                error_fragment);
                    shellMarkArithmeticError();
                    if (exit_status == 0) {
                        exit_status = 1;
                    }
                    free(result);
                    free(name);
                    ok = false;
                    break;
                }
                coerced_value = result;
                value_to_store = coerced_value;
            }
        }
        if (associative) {
            if (use_local_scope) {
                runtimeError(vm, "%s: local associative arrays are not supported", builtin_name);
                free(coerced_value);
                free(name);
                ok = false;
                break;
            }
            if (!shellArrayRegistryInitializeAssociative(name)) {
                runtimeError(vm, "declare: unable to initialise '%s'", name);
                free(coerced_value);
                free(name);
                ok = false;
                break;
            }
            if (!shellSetTrackedVariable(name, value_to_store, true)) {
                const char *readonly = shellReadonlyGetErrorName();
                if (errno == EPERM && readonly && *readonly) {
                    runtimeError(vm, "declare: %s: readonly variable", readonly);
                } else if (errno != 0) {
                    runtimeError(vm, "declare: unable to set '%s': %s", name, strerror(errno));
                } else {
                    runtimeError(vm, "declare: unable to set '%s'", name);
                }
                free(coerced_value);
                free(name);
                ok = false;
                break;
            }
        } else {
            if (use_local_scope) {
                if (!shellLocalScopeSetOnCurrentFrame(name, value_to_store, false)) {
                    runtimeError(vm, "declare: out of memory");
                    free(coerced_value);
                    free(name);
                    ok = false;
                    break;
                }
            } else if (!shellSetTrackedVariable(name, value_to_store, false)) {
                const char *readonly = shellReadonlyGetErrorName();
                if (errno == EPERM && readonly && *readonly) {
                    runtimeError(vm, "declare: %s: readonly variable", readonly);
                } else if (errno != 0) {
                    runtimeError(vm, "declare: unable to set '%s': %s", name, strerror(errno));
                } else {
                    runtimeError(vm, "declare: unable to set '%s'", name);
                }
                free(coerced_value);
                free(name);
                ok = false;
                break;
            }
        }
        if (ok && mark_readonly) {
            if (!shellReadonlyAdd(name)) {
                runtimeError(vm, "declare: out of memory");
                free(coerced_value);
                free(name);
                ok = false;
                break;
            }
        }
        free(name);
        if (coerced_value) {
            free(coerced_value);
        }
    }

    gShellRuntime.local_scope_active = false;

    if (!ok && exit_status == 0) {
        exit_status = 1;
    }
    shellUpdateStatus(exit_status);
    return makeVoid();
}

Value vmBuiltinShellReadonly(VM *vm, int arg_count, Value *args) {
    bool print_list = (arg_count == 0);
    bool parsing_options = true;
    bool processed_assignment = false;

    for (int i = 0; i < arg_count; ++i) {
        Value v = args[i];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "readonly: arguments must be strings");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *text = v.s_val;
        if (parsing_options) {
            if (strcmp(text, "--") == 0) {
                parsing_options = false;
                continue;
            }
            if (strcmp(text, "-p") == 0) {
                print_list = true;
                continue;
            }
            if (text[0] == '-' && text[1] != '\0') {
                runtimeError(vm, "readonly: unsupported option '%s'", text);
                shellUpdateStatus(1);
                return makeVoid();
            }
            parsing_options = false;
        }
        processed_assignment = true;
        const char *eq = strchr(text, '=');
        if (eq) {
            size_t name_len = (size_t)(eq - text);
            if (name_len == 0) {
                runtimeError(vm, "readonly: invalid assignment '%s'", text);
                shellUpdateStatus(1);
                return makeVoid();
            }
            char *name = strndup(text, name_len);
            if (!name) {
                runtimeError(vm, "readonly: out of memory");
                shellUpdateStatus(1);
                return makeVoid();
            }
            if (!shellIsValidEnvName(name)) {
                runtimeError(vm, "readonly: invalid variable name '%s'", name);
                free(name);
                shellUpdateStatus(1);
                return makeVoid();
            }
            const char *value = eq + 1;
            if (!shellSetTrackedVariable(name, value, false)) {
                const char *readonly = shellReadonlyGetErrorName();
                if (errno == EPERM && readonly && *readonly) {
                    runtimeError(vm, "readonly: %s: readonly variable", readonly);
                } else if (errno != 0) {
                    runtimeError(vm, "readonly: unable to set '%s': %s", name, strerror(errno));
                } else {
                    runtimeError(vm, "readonly: unable to set '%s'", name);
                }
                free(name);
                shellUpdateStatus(errno ? errno : 1);
                return makeVoid();
            }
            if (!shellReadonlyAdd(name)) {
                runtimeError(vm, "readonly: out of memory");
                free(name);
                shellUpdateStatus(1);
                return makeVoid();
            }
            free(name);
            continue;
        }
        if (!shellIsValidEnvName(text)) {
            runtimeError(vm, "readonly: invalid variable name '%s'", text);
            shellUpdateStatus(1);
            return makeVoid();
        }
        if (!shellReadonlyContains(text)) {
            const char *existing = shellSafeGetenv(text);
            if (!existing) {
                if (!shellSetTrackedVariable(text, "", false)) {
                    const char *readonly = shellReadonlyGetErrorName();
                    if (errno == EPERM && readonly && *readonly) {
                        runtimeError(vm, "readonly: %s: readonly variable", readonly);
                    } else if (errno != 0) {
                        runtimeError(vm, "readonly: unable to set '%s': %s", text, strerror(errno));
                    } else {
                        runtimeError(vm, "readonly: unable to set '%s'", text);
                    }
                    shellUpdateStatus(errno ? errno : 1);
                    return makeVoid();
                }
            }
        }
        if (!shellReadonlyAdd(text)) {
            runtimeError(vm, "readonly: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
    }

    if (print_list || (!processed_assignment && arg_count == 0)) {
        shellReadonlyPrintVariables();
    }

    shellUpdateStatus(0);
    return makeVoid();
}

static void shellCommandFreeResults(ShellCommandResult *results, size_t count) {
    if (!results) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(results[i].detail);
        results[i].detail = NULL;
    }
    free(results);
}

static bool shellCommandAppendResult(ShellCommandResult **results,
                                     size_t *count,
                                     ShellCommandResultKind kind,
                                     const char *detail) {
    if (!results || !count) {
        return false;
    }
    char *copy = NULL;
    if (detail && *detail) {
        copy = strdup(detail);
        if (!copy) {
            return false;
        }
    } else if (detail) {
        copy = strdup("");
        if (!copy) {
            return false;
        }
    }
    ShellCommandResult *resized =
        (ShellCommandResult *)realloc(*results, (*count + 1) * sizeof(ShellCommandResult));
    if (!resized) {
        free(copy);
        return false;
    }
    *results = resized;
    ShellCommandResult *entry = &resized[*count];
    entry->kind = kind;
    entry->detail = copy;
    (*count)++;
    return true;
}

static bool shellCommandExecutableExists(const char *path) {
    if (!path || !*path) {
        return false;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        return false;
    }
    return access(path, X_OK) == 0;
}

static bool shellCommandEnumerateExecutables(const char *name,
                                             const char *path_env,
                                             bool find_all,
                                             ShellCommandResult **results,
                                             size_t *count) {
    if (!name || !*name) {
        return true;
    }
    if (strchr(name, '/')) {
        if (shellCommandExecutableExists(name)) {
            if (!shellCommandAppendResult(results, count, SHELL_COMMAND_RESULT_FILE, name)) {
                return false;
            }
        }
        return true;
    }

    const char *env_value = path_env ? path_env : shellSafeGetenv("PATH");
    if (!env_value) {
        env_value = "";
    }
    char *copy = strdup(env_value);
    if (!copy) {
        return false;
    }

    bool ok = true;
    char *cursor = copy;
    while (true) {
        char *segment = cursor;
        char *sep = cursor ? strchr(cursor, ':') : NULL;
        if (sep) {
            *sep = '\0';
        }
        const char *dir = (segment && *segment) ? segment : ".";
        size_t dir_len = strlen(dir);
        size_t name_len = strlen(name);
        bool need_sep = dir_len > 0 && dir[dir_len - 1] != '/';
        size_t total_len = dir_len + (need_sep ? 1 : 0) + name_len;
        char *candidate = (char *)malloc(total_len + 1);
        if (!candidate) {
            ok = false;
            break;
        }
        size_t pos = 0;
        if (dir_len > 0) {
            memcpy(candidate + pos, dir, dir_len);
            pos += dir_len;
        }
        if (need_sep) {
            candidate[pos++] = '/';
        }
        memcpy(candidate + pos, name, name_len);
        pos += name_len;
        candidate[pos] = '\0';

        if (shellCommandExecutableExists(candidate)) {
            if (!shellCommandAppendResult(results, count, SHELL_COMMAND_RESULT_FILE, candidate)) {
                ok = false;
                free(candidate);
                break;
            }
            if (!find_all) {
                free(candidate);
                break;
            }
        }
        free(candidate);
        if (!sep) {
            break;
        }
        cursor = sep + 1;
        if (!cursor) {
            break;
        }
    }

        free(copy);
    return ok;
}

static bool shellCommandCollectInfo(const char *name,
                                    bool collect_all_paths,
                                    bool use_default_path,
                                    ShellCommandResult **out_results,
                                    size_t *out_count) {
    if (out_results) {
        *out_results = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!name || !*name) {
        return true;
    }

    ShellCommandResult *results = NULL;
    size_t count = 0;

    ShellAlias *alias = shellFindAlias(name);
    if (alias) {
        const char *value = alias->value ? alias->value : "";
        if (!shellCommandAppendResult(&results, &count, SHELL_COMMAND_RESULT_ALIAS, value)) {
            goto error;
        }
    }

    ShellFunctionEntry *function_entry = shellFindFunctionEntry(name);
    if (function_entry && function_entry->compiled) {
        if (!shellCommandAppendResult(&results, &count, SHELL_COMMAND_RESULT_FUNCTION, NULL)) {
            goto error;
        }
    }

    bool is_runtime_builtin = shellIsRuntimeBuiltin(name);
    const char *canonical = NULL;
    bool canonical_override = false;
    if (name && *name) {
        canonical = shellBuiltinCanonicalName(name);
        if (canonical && *canonical && strcmp(canonical, name) != 0 &&
            shellIsRuntimeBuiltin(canonical)) {
            canonical_override = true;
        }
    }
    bool applet_registered = false;
#if defined(PSCAL_TARGET_IOS)
    const SmallclueApplet *applet = smallclueFindApplet(name);
    if (applet) {
        applet_registered = smallclueIsRegisteredBuiltinName(name);
    }
    if (applet && applet_registered && !canonical_override) {
        if (!shellCommandAppendResult(&results, &count, SHELL_COMMAND_RESULT_APPLET, applet->name)) {
            goto error;
        }
    }
#endif

    if (is_runtime_builtin && (!applet_registered || canonical_override)) {
        const char *builtin_name = canonical ? canonical : name;
        if (!shellCommandAppendResult(&results, &count, SHELL_COMMAND_RESULT_BUILTIN, builtin_name)) {
            goto error;
        }
    }

    const char *path_env = use_default_path ? kShellCommandDefaultPath : NULL;
    if (!shellCommandEnumerateExecutables(name,
                                          path_env,
                                          collect_all_paths,
                                          &results,
                                          &count)) {
        goto error;
    }

    if (out_results) {
        *out_results = results;
    } else {
        shellCommandFreeResults(results, count);
    }
    if (out_count) {
        *out_count = count;
    }
    return true;

error:
    shellCommandFreeResults(results, count);
    if (out_results) {
        *out_results = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    return false;
}

static const char *shellCommandResultKindLabel(ShellCommandResultKind kind) {
    switch (kind) {
        case SHELL_COMMAND_RESULT_ALIAS:
            return "alias";
        case SHELL_COMMAND_RESULT_FUNCTION:
            return "function";
        case SHELL_COMMAND_RESULT_BUILTIN:
            return "builtin";
        case SHELL_COMMAND_RESULT_APPLET:
            return "smallclue applet";
        case SHELL_COMMAND_RESULT_FILE:
            return "file";
    }
    return "file";
}

static void shellCommandPrintVerbose(const char *name, const ShellCommandResult *result) {
    if (!name || !result) {
        return;
    }
    switch (result->kind) {
        case SHELL_COMMAND_RESULT_ALIAS:
            printf("%s is aliased to '%s'\n", name, result->detail ? result->detail : "");
            break;
        case SHELL_COMMAND_RESULT_FUNCTION:
            printf("%s is a function\n", name);
            break;
        case SHELL_COMMAND_RESULT_BUILTIN:
            printf("%s is a shell builtin\n", name);
            break;
        case SHELL_COMMAND_RESULT_APPLET:
            printf("%s is a smallclue applet\n", name);
            break;
        case SHELL_COMMAND_RESULT_FILE:
            printf("%s is %s\n", name, result->detail ? result->detail : "");
            break;
    }
}

static void shellCommandPrintShort(const char *name, const ShellCommandResult *result) {
    if (!name || !result) {
        return;
    }
    if (result->kind == SHELL_COMMAND_RESULT_FILE) {
        printf("%s\n", result->detail ? result->detail : "");
    } else {
        printf("%s\n", name);
    }
}

static ShellHashEntry *shellHashFindEntry(const char *name) {
    if (!name || !*name || !gShellRuntime.hash_entries) {
        return NULL;
    }
    for (size_t i = 0; i < gShellRuntime.hash_count; ++i) {
        ShellHashEntry *entry = &gShellRuntime.hash_entries[i];
        if (entry->name && strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

static bool shellHashEnsureCapacity(size_t min_capacity) {
    if (gShellRuntime.hash_capacity >= min_capacity) {
        return true;
    }
    size_t new_capacity = gShellRuntime.hash_capacity ? gShellRuntime.hash_capacity * 2 : 4;
    while (new_capacity < min_capacity) {
        size_t doubled = new_capacity * 2;
        if (doubled <= new_capacity) {
            new_capacity = min_capacity;
            break;
        }
        new_capacity = doubled;
    }
    ShellHashEntry *resized =
        (ShellHashEntry *)realloc(gShellRuntime.hash_entries, new_capacity * sizeof(ShellHashEntry));
    if (!resized) {
        return false;
    }
    for (size_t i = gShellRuntime.hash_capacity; i < new_capacity; ++i) {
        resized[i].name = NULL;
        resized[i].path = NULL;
        resized[i].hits = 0;
    }
    gShellRuntime.hash_entries = resized;
    gShellRuntime.hash_capacity = new_capacity;
    return true;
}

static void shellHashClear(void) {
    if (!gShellRuntime.hash_entries) {
        gShellRuntime.hash_count = 0;
        return;
    }
    for (size_t i = 0; i < gShellRuntime.hash_count; ++i) {
        ShellHashEntry *entry = &gShellRuntime.hash_entries[i];
        free(entry->name);
        free(entry->path);
        entry->name = NULL;
        entry->path = NULL;
        entry->hits = 0;
    }
    gShellRuntime.hash_count = 0;
}

static bool shellHashResolveExecutable(const char *name, char **out_path) {
    if (out_path) {
        *out_path = NULL;
    }
    if (!name || !*name || strchr(name, '/')) {
        return false;
    }
    ShellCommandResult *results = NULL;
    size_t count = 0;
    if (!shellCommandCollectInfo(name, false, false, &results, &count)) {
        return false;
    }
    bool success = false;
    char *path_copy = NULL;
    for (size_t i = 0; i < count; ++i) {
        ShellCommandResult *res = &results[i];
        if (res->kind == SHELL_COMMAND_RESULT_FILE && res->detail && *res->detail) {
            path_copy = strdup(res->detail);
            if (path_copy) {
                success = true;
            }
            break;
        }
    }
    shellCommandFreeResults(results, count);
    if (!success) {
        free(path_copy);
        return false;
    }
    if (out_path) {
        *out_path = path_copy;
    } else {
        free(path_copy);
    }
    return true;
}

static bool shellHashAddName(const char *name) {
    if (!name || !*name) {
        return true;
    }
    if (strchr(name, '/')) {
        return true;
    }
    ShellCommandResult *results = NULL;
    size_t count = 0;
    if (!shellCommandCollectInfo(name, false, false, &results, &count)) {
        return false;
    }
    bool hashed = false;
    bool success = true;
    for (size_t i = 0; i < count; ++i) {
        ShellCommandResult *res = &results[i];
        if (res->kind == SHELL_COMMAND_RESULT_FILE && res->detail && *res->detail) {
            ShellHashEntry *entry = shellHashFindEntry(name);
            bool new_entry = (entry == NULL);
            char *path_copy = strdup(res->detail);
            if (!path_copy) {
                success = false;
                break;
            }
            if (new_entry) {
                if (!shellHashEnsureCapacity(gShellRuntime.hash_count + 1)) {
                    free(path_copy);
                    success = false;
                    break;
                }
                entry = &gShellRuntime.hash_entries[gShellRuntime.hash_count];
                char *name_copy = strdup(name);
                if (!name_copy) {
                    free(path_copy);
                    success = false;
                    break;
                }
                entry->name = name_copy;
                entry->path = path_copy;
                entry->hits = 0;
                gShellRuntime.hash_count++;
            } else {
                free(entry->path);
                entry->path = path_copy;
                entry->hits = 0;
            }
            hashed = true;
            break;
        }
    }
    shellCommandFreeResults(results, count);
    if (!success) {
        return false;
    }
    if (hashed) {
        return true;
    }
    return false;
}

static void shellHashPrintTable(void) {
    if (gShellRuntime.hash_count == 0) {
        printf("hash: hash table empty\n");
        return;
    }
    printf("hits\tcommand\n");
    for (size_t i = 0; i < gShellRuntime.hash_count; ++i) {
        ShellHashEntry *entry = &gShellRuntime.hash_entries[i];
        const char *path = entry->path ? entry->path : "";
        printf("%4lu\t%s\n", entry->hits, path);
    }
}

static void shellHashRecordExecution(const char *name) {
    if (!name || !*name) {
        return;
    }
    if (strchr(name, '/')) {
        return;
    }
    ShellHashEntry *entry = shellHashFindEntry(name);
    if (entry) {
        if (entry->path && access(entry->path, X_OK) == 0) {
            if (entry->hits < ULONG_MAX) {
                entry->hits += 1;
            }
            return;
        }
        char *resolved = NULL;
        if (!shellHashResolveExecutable(name, &resolved)) {
            return;
        }
        free(entry->path);
        entry->path = resolved;
        entry->hits = 1;
        return;
    }
    char *resolved = NULL;
    if (!shellHashResolveExecutable(name, &resolved)) {
        return;
    }
    if (!shellHashEnsureCapacity(gShellRuntime.hash_count + 1)) {
        free(resolved);
        return;
    }
    ShellHashEntry *slot = &gShellRuntime.hash_entries[gShellRuntime.hash_count];
    char *name_copy = strdup(name);
    if (!name_copy) {
        free(resolved);
        return;
    }
    slot->name = name_copy;
    slot->path = resolved;
    slot->hits = 1;
    gShellRuntime.hash_count++;
}

Value vmBuiltinShellExport(VM *vm, int arg_count, Value *args) {
    bool print_env = (arg_count == 0);
    bool parsing_options = true;
    bool processed_assignment = false;

    for (int i = 0; i < arg_count; ++i) {
        Value v = args[i];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "export: arguments must be strings");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *text = v.s_val;
        if (parsing_options) {
            if (strcmp(text, "--") == 0) {
                parsing_options = false;
                continue;
            }
            if (strcmp(text, "-p") == 0) {
                print_env = true;
                continue;
            }
            if (text[0] == '-' && text[1] != '\0') {
                runtimeError(vm, "export: unsupported option '%s'", text);
                shellUpdateStatus(1);
                return makeVoid();
            }
            parsing_options = false;
        }
        if (parsing_options) {
            continue;
        }
        processed_assignment = true;
        const char *eq = strchr(text, '=');
        if (eq) {
            size_t name_len = (size_t)(eq - text);
            if (name_len == 0) {
                runtimeError(vm, "export: invalid assignment '%s'", text);
                shellUpdateStatus(1);
                return makeVoid();
            }
            char *name = strndup(text, name_len);
            if (!name) {
                runtimeError(vm, "export: out of memory");
                shellUpdateStatus(1);
                return makeVoid();
            }
            if (!shellIsValidEnvName(name)) {
                runtimeError(vm, "export: invalid variable name '%s'", name);
                free(name);
                shellUpdateStatus(1);
                return makeVoid();
            }
            const char *value = eq + 1;
            if (!shellSetTrackedVariable(name, value, false)) {
                runtimeError(vm, "export: unable to set '%s': %s", name, strerror(errno));
                free(name);
                shellUpdateStatus(1);
                return makeVoid();
            }
            free(name);
        } else {
            if (!shellIsValidEnvName(text)) {
                runtimeError(vm, "export: invalid variable name '%s'", text);
                shellUpdateStatus(1);
                return makeVoid();
            }
            const char *value = shellSafeGetenv(text);
            if (!value) {
                value = "";
            }
            if (!shellSetTrackedVariable(text, value, false)) {
                runtimeError(vm, "export: unable to set '%s': %s", text, strerror(errno));
                shellUpdateStatus(1);
                return makeVoid();
            }
        }
    }

    if (print_env || (!processed_assignment && arg_count == 0)) {
        shellExportPrintEnvironment();
    }

    shellUpdateStatus(0);
    return makeVoid();
}

static bool shellParseUmaskValue(const char *text, mode_t *out_mask) {
    if (!text || !*text || !out_mask) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 8);
    if (errno != 0 || !end || *end != '\0' || value < 0 || value > 0777) {
        return false;
    }
    *out_mask = (mode_t)value;
    return true;
}

static void shellUmaskFormatSymbolic(mode_t mask, char *buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        return;
    }
    const struct {
        mode_t read_bit;
        mode_t write_bit;
        mode_t exec_bit;
        char prefix;
    } classes[] = {
        {S_IRUSR, S_IWUSR, S_IXUSR, 'u'},
        {S_IRGRP, S_IWGRP, S_IXGRP, 'g'},
        {S_IROTH, S_IWOTH, S_IXOTH, 'o'},
    };

    size_t offset = 0;
    for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); ++i) {
        if (offset + 6 >= buffer_size) {
            break;
        }
        buffer[offset++] = classes[i].prefix;
        buffer[offset++] = '=';
        buffer[offset++] = (mask & classes[i].read_bit) ? '-' : 'r';
        buffer[offset++] = (mask & classes[i].write_bit) ? '-' : 'w';
        buffer[offset++] = (mask & classes[i].exec_bit) ? '-' : 'x';
        if (i + 1 < sizeof(classes) / sizeof(classes[0])) {
            buffer[offset++] = ',';
        }
    }

    if (offset >= buffer_size) {
        offset = buffer_size - 1;
    }
    buffer[offset] = '\0';
}

Value vmBuiltinShellUmask(VM *vm, int arg_count, Value *args) {
    bool symbolic = false;
    bool parsing_options = true;
    const char *mask_text = NULL;

    for (int i = 0; i < arg_count; ++i) {
        Value v = args[i];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "umask: arguments must be strings");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *text = v.s_val;
        if (parsing_options && strcmp(text, "--") == 0) {
            parsing_options = false;
            continue;
        }
        if (parsing_options && strcmp(text, "-S") == 0) {
            symbolic = true;
            continue;
        }
        if (parsing_options && text[0] == '-' && text[1] != '\0') {
            runtimeError(vm, "umask: unsupported option '%s'", text);
            shellUpdateStatus(1);
            return makeVoid();
        }
        parsing_options = false;
        if (mask_text) {
            runtimeError(vm, "umask: too many arguments");
            shellUpdateStatus(1);
            return makeVoid();
        }
        mask_text = text;
    }

    if (mask_text) {
        mode_t new_mask = 0;
        if (!shellParseUmaskValue(mask_text, &new_mask)) {
            runtimeError(vm, "umask: invalid mode '%s'", mask_text);
            shellUpdateStatus(1);
            return makeVoid();
        }
        umask(new_mask);
        shellUpdateStatus(0);
        return makeVoid();
    }

    mode_t current = umask(0);
    umask(current);

    if (symbolic) {
        char formatted[32];
        shellUmaskFormatSymbolic(current, formatted, sizeof(formatted));
        printf("%s\n", formatted);
    } else {
        printf("%04o\n", (unsigned int)current);
    }

    shellUpdateStatus(0);
    return makeVoid();
}

static void shellTimesFormatValue(char *buffer, size_t buffer_size, clock_t ticks, long ticks_per_second) {
    if (!buffer || buffer_size == 0) {
        return;
    }
    if (ticks_per_second <= 0) {
        ticks_per_second = 100;
    }
    double total_seconds = (double)ticks / (double)ticks_per_second;
    if (total_seconds < 0.0) {
        total_seconds = 0.0;
    }
    long minutes = (long)(total_seconds / 60.0);
    double seconds = total_seconds - (double)minutes * 60.0;
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    double rounded = floor(seconds * 1000.0 + 0.5) / 1000.0;
    if (rounded >= 60.0) {
        minutes += 1;
        rounded = 0.0;
    }
    snprintf(buffer, buffer_size, "%ldm%0.3fs", minutes, rounded);
    buffer[buffer_size - 1] = '\0';
}

Value vmBuiltinShellTimes(VM *vm, int arg_count, Value *args) {
    (void)args;
    (void)arg_count;
    struct tms time_info;
    clock_t result = times(&time_info);
    if (result == (clock_t)-1) {
        runtimeError(vm, "times: %s", strerror(errno));
        shellUpdateStatus(errno ? errno : 1);
        return makeVoid();
    }
    long ticks_per_second = sysconf(_SC_CLK_TCK);
    char shell_user[32];
    char shell_system[32];
    char child_user[32];
    char child_system[32];
    shellTimesFormatValue(shell_user, sizeof(shell_user), time_info.tms_utime, ticks_per_second);
    shellTimesFormatValue(shell_system, sizeof(shell_system), time_info.tms_stime, ticks_per_second);
    shellTimesFormatValue(child_user, sizeof(child_user), time_info.tms_cutime, ticks_per_second);
    shellTimesFormatValue(child_system, sizeof(child_system), time_info.tms_cstime, ticks_per_second);
    printf("%s %s\n", shell_user, shell_system);
    printf("%s %s\n", child_user, child_system);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellCommand(VM *vm, int arg_count, Value *args) {
    bool parsing_options = true;
    bool list_all = false;
    bool print_short = false;
    bool print_verbose = false;
    bool use_default_path = false;
    int first_name = arg_count;

    for (int i = 0; i < arg_count; ++i) {
        Value v = args[i];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "command: arguments must be strings");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *text = v.s_val;
        if (parsing_options) {
            if (strcmp(text, "--") == 0) {
                parsing_options = false;
                continue;
            }
            if (text[0] == '-' && text[1] != '\0') {
                for (const char *opt = text + 1; *opt; ++opt) {
                    switch (*opt) {
                        case 'a':
                            list_all = true;
                            break;
                        case 'p':
                            use_default_path = true;
                            break;
                        case 'v':
                            if (!print_verbose) {
                                print_short = true;
                            }
                            break;
                        case 'V':
                            print_verbose = true;
                            print_short = false;
                            break;
                        default:
                            runtimeError(vm, "command: unsupported option '-%c'", *opt);
                            shellUpdateStatus(1);
                            return makeVoid();
                    }
                }
                continue;
            }
            parsing_options = false;
        }
        if (parsing_options) {
            continue;
        }
        if (first_name == arg_count) {
            first_name = i;
        }
    }

    if (!print_short && !print_verbose) {
        if (first_name == arg_count) {
            shellUpdateStatus(0);
            return makeVoid();
        }
        size_t exec_argc = (size_t)(arg_count - first_name);
        ShellCommand cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.argc = exec_argc;
        cmd.argv = (char **)calloc(exec_argc + 1, sizeof(char *));
        if (!cmd.argv) {
            runtimeError(vm, "command: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        bool ok = true;
        for (size_t i = 0; i < exec_argc; ++i) {
            const char *source = args[first_name + (int)i].s_val;
            cmd.argv[i] = strdup(source ? source : "");
            if (!cmd.argv[i]) {
                ok = false;
                for (size_t j = 0; j < i; ++j) {
                    free(cmd.argv[j]);
                }
                free(cmd.argv);
                cmd.argv = NULL;
                break;
            }
        }
        if (!ok) {
            runtimeError(vm, "command: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        if (cmd.argv && cmd.argv[0] && cmd.argv[0][0] != '\0') {
            ShellCommandResult *results = NULL;
            size_t count = 0;
            if (!shellCommandCollectInfo(cmd.argv[0], false, use_default_path, &results, &count)) {
                runtimeError(vm, "command: out of memory");
                shellUpdateStatus(1);
                for (size_t j = 0; j < exec_argc; ++j) {
                    free(cmd.argv[j]);
                }
                free(cmd.argv);
                return makeVoid();
            }
            for (size_t idx = 0; idx < count; ++idx) {
                ShellCommandResult *entry = &results[idx];
                if (entry->kind == SHELL_COMMAND_RESULT_FILE && entry->detail && entry->detail[0] != '\0') {
                    char *replacement = strdup(entry->detail);
                    if (replacement) {
                        free(cmd.argv[0]);
                        cmd.argv[0] = replacement;
                    }
                    break;
                }
            }
            shellCommandFreeResults(results, count);
        }
        cmd.ignore_functions = true;
        cmd.argv[exec_argc] = NULL;
        VM *previous_vm = shellSwapCurrentVm(vm);
        Value result = shellExecuteCommand(vm, &cmd);
        shellRestoreCurrentVm(previous_vm);
        return result;
    }

    if (first_name == arg_count) {
        shellUpdateStatus(0);
        return makeVoid();
    }

    bool overall_ok = true;
    bool collect_all_paths = list_all;

    for (int i = first_name; i < arg_count; ++i) {
        const char *name = args[i].s_val;
        ShellCommandResult *results = NULL;
        size_t count = 0;
        if (!shellCommandCollectInfo(name, collect_all_paths, use_default_path, &results, &count)) {
            runtimeError(vm, "command: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }

        if (count == 0) {
            overall_ok = false;
            if (print_verbose) {
                runtimeError(vm, "command: %s: not found", name);
            }
            shellUpdateStatus(1);
            shellCommandFreeResults(results, count);
            continue;
        }

        if (print_verbose) {
            if (list_all) {
                for (size_t j = 0; j < count; ++j) {
                    shellCommandPrintVerbose(name, &results[j]);
                }
            } else {
                shellCommandPrintVerbose(name, &results[0]);
            }
        } else if (print_short) {
            if (list_all) {
                for (size_t j = 0; j < count; ++j) {
                    shellCommandPrintShort(name, &results[j]);
                }
            } else {
                shellCommandPrintShort(name, &results[0]);
            }
        }

        shellCommandFreeResults(results, count);
    }

    shellUpdateStatus(overall_ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellWhich(VM *vm, int arg_count, Value *args) {
    bool parsing_options = true;
    bool list_all = false;
    bool silent = false;
    bool use_default_path = false;
    int first_name = arg_count;

    for (int i = 0; i < arg_count; ++i) {
        Value v = args[i];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "which: arguments must be strings");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *text = v.s_val;
        if (parsing_options) {
            if (strcmp(text, "--") == 0) {
                parsing_options = false;
                continue;
            }
            if (text[0] == '-' && text[1] != '\0') {
                for (const char *opt = text + 1; *opt; ++opt) {
                    switch (*opt) {
                        case 'a':
                            list_all = true;
                            break;
                        case 's':
                            silent = true;
                            break;
                        case 'p':
                            use_default_path = true;
                            break;
                        default:
                            runtimeError(vm, "which: invalid option -- '%c'", *opt);
                            shellUpdateStatus(1);
                            return makeVoid();
                    }
                }
                continue;
            }
            parsing_options = false;
        }
        if (first_name == arg_count) {
            first_name = i;
        }
    }

    if (first_name == arg_count) {
        runtimeError(vm, "which: expected name");
        shellUpdateStatus(1);
        return makeVoid();
    }

    bool overall_ok = true;
    bool collect_all_paths = list_all;

    for (int i = first_name; i < arg_count; ++i) {
        const char *name = args[i].s_val;
        ShellCommandResult *results = NULL;
        size_t count = 0;
        if (!shellCommandCollectInfo(name, collect_all_paths, use_default_path, &results, &count)) {
            runtimeError(vm, "which: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }

        bool printed = false;
        for (size_t j = 0; j < count; ++j) {
            ShellCommandResult *res = &results[j];
            const char *detail = res->detail ? res->detail : "";
            switch (res->kind) {
                case SHELL_COMMAND_RESULT_ALIAS:
                    if (!silent) {
                        printf("%s: aliased to `%s'\n", name, detail);
                    }
                    printed = true;
                    break;
                case SHELL_COMMAND_RESULT_FUNCTION:
                    if (!silent) {
                        printf("%s: shell function\n", name);
                    }
                    printed = true;
                    break;
                case SHELL_COMMAND_RESULT_BUILTIN:
                    if (!silent) {
                        printf("%s: shell builtin\n", name);
                    }
                    printed = true;
                    break;
                case SHELL_COMMAND_RESULT_APPLET:
                    if (!silent) {
                        printf("%s: smallclue applet\n", name);
                    }
                    printed = true;
                    break;
                case SHELL_COMMAND_RESULT_FILE:
                    if (!silent) {
                        printf("%s\n", detail);
                    }
                    printed = true;
                    break;
            }
            if (printed && !list_all) {
                break;
            }
        }

        if (!printed) {
            overall_ok = false;
            if (!silent) {
                fprintf(stderr, "%s: not found\n", name);
            }
        }
        shellCommandFreeResults(results, count);
    }

    shellUpdateStatus(overall_ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellHash(VM *vm, int arg_count, Value *args) {
    bool parsing_options = true;
    bool reset_table = false;
    bool print_paths = false;
    int first_name = arg_count;

    for (int i = 0; i < arg_count; ++i) {
        Value v = args[i];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "hash: arguments must be strings");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *text = v.s_val;
        if (parsing_options) {
            if (strcmp(text, "--") == 0) {
                parsing_options = false;
                continue;
            }
            if (text[0] == '-' && text[1] != '\0') {
                for (const char *opt = text + 1; *opt; ++opt) {
                    switch (*opt) {
                        case 'r':
                            reset_table = true;
                            break;
                        case 't':
                            print_paths = true;
                            break;
                        default:
                            runtimeError(vm, "hash: invalid option -- '%c'", *opt);
                            shellUpdateStatus(1);
                            return makeVoid();
                    }
                }
                continue;
            }
            parsing_options = false;
        }
        if (parsing_options) {
            continue;
        }
        if (first_name == arg_count) {
            first_name = i;
        }
    }

    if (reset_table) {
        shellHashClear();
    }

    if (first_name == arg_count) {
        if (print_paths) {
            runtimeError(vm, "hash: -t: option requires an argument");
            shellUpdateStatus(1);
            return makeVoid();
        }
        if (!reset_table) {
            shellHashPrintTable();
        }
        shellUpdateStatus(0);
        return makeVoid();
    }

    bool overall_ok = true;
    for (int i = first_name; i < arg_count; ++i) {
        const char *name = args[i].s_val ? args[i].s_val : "";
        if (print_paths) {
            ShellHashEntry *entry = shellHashFindEntry(name);
            if (!entry || !entry->path || !*entry->path) {
                runtimeError(vm, "hash: %s: not found", name);
                overall_ok = false;
                continue;
            }
            printf("%s\n", entry->path);
            continue;
        }
        if (!shellHashAddName(name)) {
            runtimeError(vm, "hash: %s: not found", name);
            overall_ok = false;
        }
    }

    shellUpdateStatus(overall_ok ? 0 : 1);
    return makeVoid();
}

static bool shellBuiltinIsInternal(const char *name) {
    return name && name[0] == '_' && name[1] == '_';
}

typedef struct {
    bool print_pretty;
    bool list_all;
    bool list_disabled_only;
    bool disabled_prefix;
} ShellEnableListContext;

static void shellEnableListVisitor(const char *name,
                                   const char *canonical,
                                   int id,
                                   void *ctx_void) {
    (void)id;
    ShellEnableListContext *ctx = (ShellEnableListContext *)ctx_void;
    if (!ctx) {
        return;
    }
    if (!canonical || !*canonical) {
        canonical = name;
    }
    if (!name) {
        name = canonical;
    }
    if (!canonical || !*canonical) {
        return;
    }
    if (shellBuiltinIsInternal(canonical)) {
        return;
    }
    if (strcasecmp(name, canonical) != 0) {
        return;
    }
    bool disabled = shellRuntimeBuiltinDisabled(canonical);
    if (!ctx->list_all) {
        if (ctx->list_disabled_only) {
            if (!disabled) {
                return;
            }
        } else if (disabled) {
            return;
        }
    }
    if (ctx->print_pretty) {
        if (disabled) {
            printf("enable -n %s\n", canonical);
        } else {
            printf("enable %s\n", canonical);
        }
    } else if (ctx->disabled_prefix && disabled) {
        printf("enable -n %s\n", canonical);
    } else {
        printf("%s\n", canonical);
    }
}

Value vmBuiltinShellEnable(VM *vm, int arg_count, Value *args) {
    bool parsing_options = true;
    bool disable_mode = false;
    bool print_pretty = false;
    bool list_all = false;
    int operand_start = arg_count;

    for (int i = 0; i < arg_count; ++i) {
        Value v = args[i];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "enable: arguments must be strings");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *text = v.s_val;
        if (parsing_options) {
            if (strcmp(text, "--") == 0) {
                parsing_options = false;
                operand_start = i + 1;
                break;
            }
            if (text[0] == '-' && text[1] != '\0') {
                for (const char *opt = text + 1; *opt; ++opt) {
                    switch (*opt) {
                        case 'a':
                            list_all = true;
                            break;
                        case 'n':
                            disable_mode = true;
                            break;
                        case 'p':
                            print_pretty = true;
                            break;
                        default:
                            runtimeError(vm, "enable: invalid option -- '%c'", *opt);
                            shellUpdateStatus(1);
                            return makeVoid();
                    }
                }
                continue;
            }
            parsing_options = false;
        }
        if (operand_start == arg_count) {
            operand_start = i;
        }
    }

    if (operand_start >= arg_count) {
        ShellEnableListContext ctx = {
            .print_pretty = print_pretty,
            .list_all = list_all,
            .list_disabled_only = disable_mode && !list_all,
            .disabled_prefix = disable_mode && !list_all,
        };
        shellVisitBuiltins(shellEnableListVisitor, &ctx);
        shellUpdateStatus(0);
        return makeVoid();
    }

    bool ok = true;
    for (int i = operand_start; i < arg_count; ++i) {
        Value v = args[i];
        const char *name = (v.type == TYPE_STRING) ? v.s_val : NULL;
        if (!name || !*name) {
            runtimeError(vm, "enable: : not a shell builtin");
            ok = false;
            continue;
        }
        if (!shellIsBuiltinName(name)) {
            runtimeError(vm, "enable: %s: not a shell builtin", name);
            ok = false;
            continue;
        }
        if (shellBuiltinIsInternal(name)) {
            runtimeError(vm, "enable: %s: not a shell builtin", name);
            ok = false;
            continue;
        }
        bool success = false;
        if (disable_mode) {
            success = shellRuntimeDisableBuiltin(name);
        } else {
            success = shellRuntimeEnableBuiltin(name);
        }
        if (!success) {
            runtimeError(vm, "enable: out of memory");
            ok = false;
            break;
        }
    }

    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellType(VM *vm, int arg_count, Value *args) {
    bool parsing_options = true;
    bool list_all = false;
    bool print_type = false;
    bool print_path = false;
    bool use_default_path = false;
    int first_name = arg_count;

    for (int i = 0; i < arg_count; ++i) {
        Value v = args[i];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "type: arguments must be strings");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *text = v.s_val;
        if (parsing_options) {
            if (strcmp(text, "--") == 0) {
                parsing_options = false;
                continue;
            }
            if (text[0] == '-' && text[1] != '\0') {
                for (const char *opt = text + 1; *opt; ++opt) {
                    switch (*opt) {
                        case 'a':
                            list_all = true;
                            break;
                        case 't':
                            print_type = true;
                            break;
                        case 'p':
                            print_path = true;
                            break;
                        case 'P':
                            print_path = true;
                            use_default_path = true;
                            break;
                        default:
                            runtimeError(vm, "type: unsupported option '-%c'", *opt);
                            shellUpdateStatus(1);
                            return makeVoid();
                    }
                }
                continue;
            }
            parsing_options = false;
        }
        if (parsing_options) {
            continue;
        }
        if (first_name == arg_count) {
            first_name = i;
        }
    }

    if (first_name == arg_count) {
        runtimeError(vm, "type: expected name");
        shellUpdateStatus(1);
        return makeVoid();
    }

    bool descriptive = !print_type && !print_path;
    bool overall_ok = true;
    bool collect_all_paths = list_all || print_path;

    for (int i = first_name; i < arg_count; ++i) {
        const char *name = args[i].s_val;
        ShellCommandResult *results = NULL;
        size_t count = 0;
        if (!shellCommandCollectInfo(name, collect_all_paths, use_default_path, &results, &count)) {
            runtimeError(vm, "type: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }

        if (count == 0) {
            overall_ok = false;
            if (descriptive) {
                runtimeError(vm, "type: %s: not found", name);
            }
            shellUpdateStatus(1);
            shellCommandFreeResults(results, count);
            continue;
        }

        if (print_path) {
            bool printed = false;
            for (size_t j = 0; j < count; ++j) {
                if (results[j].kind == SHELL_COMMAND_RESULT_FILE) {
                    printf("%s\n", results[j].detail ? results[j].detail : "");
                    printed = true;
                    if (!list_all) {
                        break;
                    }
                }
            }
            if (!printed) {
                overall_ok = false;
                shellUpdateStatus(1);
            }
            shellCommandFreeResults(results, count);
            continue;
        }

        if (print_type) {
            if (list_all) {
                for (size_t j = 0; j < count; ++j) {
                    printf("%s\n", shellCommandResultKindLabel(results[j].kind));
                }
            } else {
                printf("%s\n", shellCommandResultKindLabel(results[0].kind));
            }
            shellCommandFreeResults(results, count);
            continue;
        }

        if (list_all) {
            for (size_t j = 0; j < count; ++j) {
                shellCommandPrintVerbose(name, &results[j]);
            }
        } else {
            shellCommandPrintVerbose(name, &results[0]);
        }
        shellCommandFreeResults(results, count);
    }

    shellUpdateStatus(overall_ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellUnset(VM *vm, int arg_count, Value *args) {
    bool ok = true;
    for (int i = 0; i < arg_count; ++i) {
        if (args[i].type != TYPE_STRING || !args[i].s_val) {
            runtimeError(vm, "unset: expected variable name");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *name = args[i].s_val;
        if (shellReadonlyContains(name)) {
            runtimeError(vm, "unset: %s: readonly variable", name);
            ok = false;
            continue;
        }
        shellUnsetTrackedVariable(name);
    }
    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellUnsetenv(VM *vm, int arg_count, Value *args) {
    return vmBuiltinShellUnset(vm, arg_count, args);
}

static bool shellParseLoopLevel(const char *text, int *out_level) {
    if (!text || !out_level) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value <= 0 || value > INT_MAX) {
        return false;
    }
    *out_level = (int)value;
    return true;
}

static bool shellIsValidEnvName(const char *name) {
    if (!name || !*name) {
        return false;
    }
    unsigned char first = (unsigned char)name[0];
    if (!(isalpha(first) || first == '_')) {
        return false;
    }
    for (const char *cursor = name + 1; *cursor; ++cursor) {
        unsigned char ch = (unsigned char)*cursor;
        if (!(isalnum(ch) || ch == '_')) {
            return false;
        }
    }
    return true;
}

static int shellCompareEnvStrings(const void *lhs, const void *rhs) {
    const char *const *a = (const char *const *)lhs;
    const char *const *b = (const char *const *)rhs;
    if (!a || !b) {
        return 0;
    }
    if (!*a) {
        return *b ? -1 : 0;
    }
    if (!*b) {
        return 1;
    }
    return strcmp(*a, *b);
}

static void shellPrintExportEntry(const char *entry) {
    if (!entry) {
        return;
    }
    const char *eq = strchr(entry, '=');
    if (!eq) {
        printf("declare -x %s\n", entry);
        return;
    }
    size_t name_len = (size_t)(eq - entry);
    const char *value = eq + 1;
    printf("declare -x %.*s=\"", (int)name_len, entry);
    for (const char *cursor = value; *cursor; ++cursor) {
        unsigned char ch = (unsigned char)*cursor;
        if (ch == '"' || ch == '\\') {
            putchar('\\');
        }
        putchar(ch);
    }
    printf("\"\n");
}

static void shellExportPrintEnvironment(void) {
    if (!environ) {
        return;
    }
    size_t env_count = 0;
    while (environ[env_count]) {
        env_count++;
    }
    if (env_count == 0) {
        return;
    }

    char **sorted = (char **)malloc(env_count * sizeof(char *));
    if (sorted) {
        for (size_t i = 0; i < env_count; ++i) {
            sorted[i] = environ[i];
        }
        qsort(sorted, env_count, sizeof(char *), shellCompareEnvStrings);
        for (size_t i = 0; i < env_count; ++i) {
            shellPrintExportEntry(sorted[i]);
        }
        free(sorted);
    } else {
        for (char **env = environ; *env; ++env) {
            shellPrintExportEntry(*env);
        }
    }
}

static bool shellParseReturnStatus(const char *text, int *out_status) {
    if (!text || !out_status || *text == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    int status = (int)((unsigned long)value & 0xFFu);
    *out_status = status;
    return true;
}

Value vmBuiltinShellSet(VM *vm, int arg_count, Value *args) {
    bool ok = true;
    bool parsing_options = true;
    int positional_start = arg_count;
    for (int i = 0; i < arg_count && ok; ++i) {
        Value v = args[i];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "set: expected string argument");
            ok = false;
            break;
        }
        const char *token = v.s_val;
        if (!parsing_options) {
            continue;
        }
        if (strcmp(token, "--") == 0) {
            positional_start = i + 1;
            parsing_options = false;
            break;
        }
        if (strcmp(token, "-e") == 0) {
            gShellRuntime.errexit_enabled = true;
            gShellRuntime.errexit_pending = false;
            gShellRuntime.errexit_defer_active = false;
        } else if (strcmp(token, "+e") == 0) {
            gShellRuntime.errexit_enabled = false;
            gShellRuntime.errexit_pending = false;
            gShellRuntime.errexit_defer_active = false;
        } else if ((strcmp(token, "-o") == 0 || strcmp(token, "+o") == 0)) {
            bool enable = (token[0] == '-');
            if (i + 1 >= arg_count) {
                runtimeError(vm, "set: missing option name for %s", token);
                ok = false;
                break;
            }
            Value name_val = args[++i];
            if (name_val.type != TYPE_STRING || !name_val.s_val) {
                runtimeError(vm, "set: option name must be a string");
                ok = false;
                break;
            }
            if (strcasecmp(name_val.s_val, "errexit") == 0) {
                gShellRuntime.errexit_enabled = enable;
                if (!enable) {
                    gShellRuntime.errexit_pending = false;
                    gShellRuntime.errexit_defer_active = false;
                }
            } else if (strcasecmp(name_val.s_val, "posix") == 0) {
                shellRuntimeSetPosixMode(enable);
            }
        } else if (token[0] == '-' || token[0] == '+') {
            // Unsupported option, ignore for now to match previous behaviour.
        } else {
            positional_start = i;
            parsing_options = false;
            break;
        }
    }

    if (ok && positional_start < arg_count) {
        int new_count = arg_count - positional_start;
        char **new_params = NULL;
        if (new_count > 0) {
            new_params = (char **)calloc((size_t)new_count, sizeof(char *));
            if (!new_params) {
                runtimeError(vm, "set: out of memory");
                ok = false;
            } else {
                for (int i = 0; i < new_count && ok; ++i) {
                    Value val = args[positional_start + i];
                    if (val.type != TYPE_STRING || !val.s_val) {
                        runtimeError(vm, "set: positional arguments must be strings");
                        ok = false;
                        break;
                    }
                    new_params[i] = strdup(val.s_val);
                    if (!new_params[i]) {
                        runtimeError(vm, "set: out of memory");
                        ok = false;
                        break;
                    }
                }
            }
        }

        if (ok) {
            if (gShellPositionalOwned) {
                shellFreeOwnedPositionalParameters();
            } else {
                gParamValues = NULL;
                gParamCount = 0;
            }
            if (new_count > 0) {
                gParamValues = new_params;
                gParamCount = new_count;
                gShellPositionalOwned = true;
                new_params = NULL;
            } else {
                gShellPositionalOwned = false;
            }
        }

        if (new_params) {
            shellFreeParameterArray(new_params, new_count);
        }
    }

    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellTrap(VM *vm, int arg_count, Value *args) {
    if (arg_count == 0) {
        shellUpdateStatus(0);
        return makeVoid();
    }

    bool ok = true;
    int signal_start = 0;
    const char *handler = NULL;
    ShellTrapAction action = SHELL_TRAP_ACTION_COMMAND;

    if (args[0].type != TYPE_STRING || !args[0].s_val) {
        runtimeError(vm, "trap: expected string arguments");
        ok = false;
    }

    if (ok) {
        handler = args[0].s_val;
        signal_start = 1;

        if (strcmp(handler, "--") == 0) {
            if (arg_count < 2 || args[1].type != TYPE_STRING || !args[1].s_val) {
                shellUpdateStatus(0);
                return makeVoid();
            }
            handler = args[1].s_val;
            signal_start = 2;
        }

        if (strcmp(handler, "-l") == 0 || strcmp(handler, "-p") == 0) {
            shellUpdateStatus(0);
            return makeVoid();
        }

        if (strcmp(handler, "-") == 0) {
            action = SHELL_TRAP_ACTION_DEFAULT;
            handler = NULL;
        } else if (*handler == '\0') {
            action = SHELL_TRAP_ACTION_IGNORE;
            handler = NULL;
        } else {
            action = SHELL_TRAP_ACTION_COMMAND;
        }
    }

    if (!ok) {
        shellUpdateStatus(1);
        return makeVoid();
    }

    if (signal_start >= arg_count) {
        shellUpdateStatus(0);
        return makeVoid();
    }

    for (int i = signal_start; i < arg_count && ok; ++i) {
        if (args[i].type != TYPE_STRING || !args[i].s_val) {
            runtimeError(vm, "trap: expected signal name");
            ok = false;
            break;
        }
        int signo = 0;
        if (!shellRuntimeParseSignal(args[i].s_val, &signo)) {
            runtimeError(vm, "trap: invalid signal '%s'", args[i].s_val);
            ok = false;
            break;
        }
        if (signo == 0) {
            if (!shellRuntimeSetExitTrap(action, handler)) {
                runtimeError(vm, "trap: failed to install EXIT handler");
                ok = false;
                break;
            }
        } else {
            if (!shellRuntimeSetSignalTrap(signo, action, handler)) {
                runtimeError(vm, "trap: failed to install handler for %s", args[i].s_val);
                ok = false;
                break;
            }
        }
    }

    if (ok) {
        shellRuntimeRefreshTrapEnabled();
    }

    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellLocal(VM *vm, int arg_count, Value *args) {
    (void)arg_count;
    (void)args;
    gShellRuntime.local_scope_active = true;
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellBreak(VM *vm, int arg_count, Value *args) {
    int levels = 1;
    if (arg_count > 0) {
        if (args[0].type != TYPE_STRING || !args[0].s_val ||
            !shellParseLoopLevel(args[0].s_val, &levels)) {
            runtimeError(vm, "break: expected positive integer");
            shellUpdateStatus(1);
            return makeVoid();
        }
    }
    gShellRuntime.break_requested = true;
    gShellRuntime.break_requested_levels = levels;
    shellLoopRequestBreakLevels(levels);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellContinue(VM *vm, int arg_count, Value *args) {
    int levels = 1;
    if (arg_count > 0) {
        if (args[0].type != TYPE_STRING || !args[0].s_val ||
            !shellParseLoopLevel(args[0].s_val, &levels)) {
            runtimeError(vm, "continue: expected positive integer");
            shellUpdateStatus(1);
            return makeVoid();
        }
    }
    gShellRuntime.continue_requested = true;
    gShellRuntime.continue_requested_levels = levels;
    shellLoopRequestContinueLevels(levels);
    shellUpdateStatus(0);
    return makeVoid();
}

static ShellAlias *gShellAliases = NULL;
static size_t gShellAliasCount = 0;

static void shellWriteAllFd(int fd, const char *buf, size_t len) {
    if (fd < 0 || !buf || len == 0) {
        return;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        off += (size_t)n;
    }
}

static void shellFdVprintf(int fd, const char *fmt, va_list ap) {
    if (!fmt) {
        return;
    }
    char stack_buf[768];
    va_list ap2;
    va_copy(ap2, ap);
    int needed = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap2);
    va_end(ap2);
    if (needed < 0) {
        return;
    }
    if ((size_t)needed < sizeof(stack_buf)) {
        shellWriteAllFd(fd, stack_buf, (size_t)needed);
        return;
    }
    size_t cap = (size_t)needed + 1;
    char *heap = (char *)malloc(cap);
    if (!heap) {
        shellWriteAllFd(fd, stack_buf, strlen(stack_buf));
        return;
    }
    vsnprintf(heap, cap, fmt, ap);
    shellWriteAllFd(fd, heap, (size_t)needed);
    free(heap);
}

static void shellFdPrintf(int fd, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    shellFdVprintf(fd, fmt, ap);
    va_end(ap);
}

static void shellReportRecoverableError(VM *vm, bool with_location, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char message[512];
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    const char *script = NULL;
    int line = 0;
    if (with_location) {
        script = shellRuntimeGetArg0();
        if (script && *script) {
            line = shellRuntimeCurrentCommandLine();
        } else {
            script = NULL;
        }
    }

    if (script && line > 0) {
        shellFdPrintf(STDERR_FILENO, "%s: line %d: %s\n", script, line, message);
    } else if (script) {
        shellFdPrintf(STDERR_FILENO, "%s: %s\n", script, message);
    } else {
        shellFdPrintf(STDERR_FILENO, "%s\n", message);
    }

    if (vm) {
        vm->abort_requested = false;
    }
}

static void shellFreeAlias(ShellAlias *alias) {
    if (!alias) {
        return;
    }
    free(alias->name);
    free(alias->value);
    alias->name = NULL;
    alias->value = NULL;
}

static ShellAlias *shellFindAlias(const char *name) {
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < gShellAliasCount; ++i) {
        if (strcmp(gShellAliases[i].name, name) == 0) {
            return &gShellAliases[i];
        }
    }
    return NULL;
}

static void shellRemoveAliasAt(size_t index) {
    if (index >= gShellAliasCount) {
        return;
    }
    shellFreeAlias(&gShellAliases[index]);
    if (index + 1 < gShellAliasCount) {
        gShellAliases[index] = gShellAliases[gShellAliasCount - 1];
    }
    gShellAliasCount--;
    if (gShellAliasCount == 0) {
        free(gShellAliases);
        gShellAliases = NULL;
    }
}

static bool shellRemoveAlias(const char *name) {
    if (!name) {
        return false;
    }
    for (size_t i = 0; i < gShellAliasCount; ++i) {
        if (strcmp(gShellAliases[i].name, name) == 0) {
            shellRemoveAliasAt(i);
            return true;
        }
    }
    return false;
}

static void shellClearAliases(void) {
    for (size_t i = 0; i < gShellAliasCount; ++i) {
        shellFreeAlias(&gShellAliases[i]);
    }
    free(gShellAliases);
    gShellAliases = NULL;
    gShellAliasCount = 0;
}

static bool shellSetAlias(const char *name, const char *value) {
    if (!name || !value) {
        return false;
    }
    ShellAlias *existing = shellFindAlias(name);
    if (existing) {
        char *copy = strdup(value);
        if (!copy) {
            return false;
        }
        free(existing->value);
        existing->value = copy;
        return true;
    }
    ShellAlias *new_aliases = realloc(gShellAliases, sizeof(ShellAlias) * (gShellAliasCount + 1));
    if (!new_aliases) {
        return false;
    }
    gShellAliases = new_aliases;
    ShellAlias *alias = &gShellAliases[gShellAliasCount++];
    alias->name = strdup(name);
    alias->value = strdup(value);
    if (!alias->name || !alias->value) {
        free(alias->name);
        free(alias->value);
        gShellAliasCount--;
        return false;
    }
    return true;
}

typedef struct {
    const char *name;
    const char *summary;
    const char *usage;
    const char *detail;
    const char *const *aliases;
    size_t alias_count;
} ShellHelpTopic;

static const char *const kShellHelpSourceAliases[] = {"."};

static const ShellHelpTopic kShellHelpTopics[] = {
    {
        "alias",
        "Define or display shell aliases.",
        "alias [name=value ...]",
        "Without arguments prints the stored alias definitions as "
        "alias name='value'. Each NAME=VALUE argument updates or creates an alias.",
        NULL,
        0
    },
    {
        "unalias",
        "Remove shell aliases.",
        "unalias [-a] [name ...]",
        "Deletes the aliases identified by NAME. With -a all aliases are removed."
        " Providing NAME alongside -a results in an error.",
        NULL,
        0
    },
    {
        "bind",
        "Configure readline behaviour.",
        "bind [-p] [spec ...]",
        "Accepts readline \"set\" directives and remembers their most recent values."
        " The -p flag prints the stored settings in \"set name value\" form. Other"
        " invocations are currently accepted as no-ops.",
        NULL,
        0
    },
    {
        "bg",
        "Resume a stopped job in the background.",
        "bg [job]",
        "Targets the most recently launched job when no job is supplied. Job"
        " specifiers may be numeric indexes or begin with '%'.",
        NULL,
        0
    },
    {
        "break",
        "Exit from the innermost loop(s).",
        "break [n]",
        "Accepts an optional positive integer count; the default of 1 exits only"
        " the innermost active loop.",
        NULL,
        0
    },
    {
        "builtin",
        "Invoke a PSCAL VM builtin directly.",
        "builtin name [args ...]",
        "Arguments are forwarded to the named VM builtin. Prefix an argument with"
        " int:, float:/double:/real:, bool:/boolean:, str:/string:/raw:, or"
        " nil: to coerce the value; other arguments are passed as strings. When"
        " the VM builtin returns a non-void value it is printed to stdout on"
        " success.",
        NULL,
        0
    },
    {
        "command",
        "Query command resolution metadata.",
        "command [-a] [-p] [-v|-V] [name ...]",
        "With -v prints the first match for each NAME, favouring aliases, functions,"
        " builtins, and executable paths. -V prints verbose descriptions. The -a"
        " flag lists every match and -p searches using the default PATH. Execution"
        " without -v or -V is not currently supported.",
        NULL,
        0
    },
    {
        "which",
        "Locate commands and builtins.",
        "which [-a] [-s] [-p] name ...",
        "Prints how each NAME resolves in the current shell. Aliases, functions,"
        " builtins, and executable files are reported in that order. With -a every"
        " match is listed, -s suppresses output and relies on the exit status, and"
        " -p performs the search using the default PATH instead of the current one.",
        NULL,
        0
    },
    {
        "enable",
        "Enable or disable shell builtins.",
        "enable [-a] [-n] [-p] [name ...]",
        "Without NAME arguments lists builtins that are currently enabled. With"
        " -n and no names the disabled builtins are printed instead, -a lists every"
        " builtin regardless of status, and -p formats output as enable commands."
        " Supplying NAME arguments toggles those builtins on by default or off"
        " when used with -n.",
        NULL,
        0
    },
    {
        "hash",
        "Display or reset cached command lookups.",
        "hash [-r] [-t] [name ...]",
        "Without arguments prints the cached command table including hit counts. "
        "With -r the table is cleared. Each NAME hashes the executable located via PATH "
        "while -t prints the cached path for each NAME instead of updating the table.",
        NULL,
        0
    },
    {
        "cd",
        "Change the current working directory.",
        "cd [dir]",
        "With no arguments cd switches to $HOME. Successful runs update the PWD"
        " environment variable.",
        NULL,
        0
    },
    {
        "dirs",
        "Display the directory stack.",
        "dirs",
        "Prints the current directory stack with the most recent entry first."
        " Options such as -c are not yet supported.",
        NULL,
        0
    },
    {
        "pushd",
        "Push a directory onto the stack and change to it.",
        "pushd [dir]",
        "With DIR changes to the target directory and pushes the previous working"
        " directory onto the stack. Without arguments swaps the top two entries.",
        NULL,
        0
    },
    {
        "popd",
        "Pop the directory stack.",
        "popd",
        "Removes the top stack entry and switches to the new top directory. Fails"
        " when the stack contains only a single entry.",
        NULL,
        0
    },
    {
        "printf",
        "Format and print data to standard output.",
        "printf format [arguments]",
        "Applies the FORMAT string to each argument using printf-style conversions."
        " Missing arguments expand to empty strings while numeric conversions honour width and precision modifiers.",
        NULL,
        0
    },
    {
        "continue",
        "Skip to the next loop iteration.",
        "continue [n]",
        "Accepts an optional positive integer count and marks the requested"
        " number of enclosing loops to continue.",
        NULL,
        0
    },
    {
        "declare",
        "Declare variables and arrays.",
        "declare [-a|-A|-r] [name[=value] ...]",
        "Without arguments prints variables with attributes. The -a flag"
        " initialises indexed arrays, -A initialises associative arrays, and"
        " -r marks the supplied names as read-only.",
        NULL,
        0
    },
    {
        "readonly",
        "Mark variables as read-only.",
        "readonly [-p] [name[=value] ...]",
        "Marks each NAME as read-only. When assignments are provided the "
        "variable is set before being protected. With no arguments or when "
        "invoked with -p the current read-only definitions are printed.",
        NULL,
        0
    },
    {
        "umask",
        "Set or display the file creation mask.",
        "umask [-S] [mode]",
        "Without MODE prints the current mask as a zero-padded octal value. "
        "With -S the mask is shown symbolically as u=,g=,o=. Supplying MODE "
        "updates the mask using octal digits 0-7.",
        NULL,
        0
    },
    {
        "eval",
        "Execute words as an inline script.",
        "eval [word ...]",
        "Concatenates the provided words with single spaces and executes the"
        " resulting text without caching bytecode.",
        NULL,
        0
    },
    {
        "exit",
        "Request that the shell terminate.",
        "exit [status]",
        "Marks the shell for exit after running cleanup handlers. If an integer"
        " value is supplied it becomes the process exit code; otherwise the"
        " status defaults to 0.",
        NULL,
        0
    },
    {
        "export",
        "Set environment variables or print the environment.",
        "export [-p] [name[=value] ...]",
        "Without arguments (or with -p) prints the environment as export"
        " assignments. Each name or NAME=VALUE argument updates the process"
        " environment. Only -p and -- are recognised options.",
        NULL,
        0
    },
    {
        "fg",
        "Move a job to the foreground.",
        "fg [job]",
        "Targets the most recently launched job when no argument is supplied."
        " Job specifiers may be numeric indexes or begin with '%'.",
        NULL,
        0
    },
    {
        "finger",
        "Display basic account information.",
        "finger [user]",
        "Prints the login, gecos name, home directory, and shell for the"
        " selected account. Defaults to the current user when no argument is"
        " provided.",
        NULL,
        0
    },
    {
        "help",
        "List builtins or describe a specific builtin.",
        "help [builtin]",
        "Without arguments prints the builtin catalog. Supplying a builtin name"
        " shows its usage summary.",
        NULL,
        0
    },
#if defined(PSCAL_TARGET_IOS)
    {
        "smallclue-help",
        "List available smallclue applets.",
        "smallclue-help",
        "Prints the iOS smallclue applet catalog with brief descriptions.",
        NULL,
        0
    },
#endif
    {
        "stdioinfo",
        "Show stdio routing and session state.",
        "stdioinfo",
        "Prints the current stdin/stdout/stderr descriptor mapping, TTY state,"
        " and on iOS the active vproc session/PTY wiring.",
        NULL,
        0
    },
    {
        "caller",
        "Report information about active call frames.",
        "caller [depth]",
        "Prints the line number and caller context for the current function by "
        "default. Providing DEPTH selects a deeper stack frame, mirroring Bash's "
        "caller builtin output.",
        NULL,
        0
    },
    {
        "history",
        "Print the interactive history list.",
        "history",
        "Writes each recorded interactive command with its history index.",
        NULL,
        0
    },
    {
        "jobs",
        "List active background jobs.",
        "jobs",
        "Reports each tracked job with its index, status, and command line.",
        NULL,
        0
    },
    {
        "lps",
        "Show synthetic vproc tasks on iOS.",
        "lps",
        "Displays the shell-managed virtual processes instead of the host process"
        " table. Columns include the synthetic PID/PGID, state, and associated"
        " command when available. On platforms with fork/exec support the external"
        " ps is still used.",
        NULL,
        0
    },
    {
        "top",
        "Live view of synthetic vprocs on iOS.",
        "top",
        "Prints a one-shot table of synthetic vproc state including pid, pgid,"
        " foreground marker, state, and utime/stime counters. Additional fields"
        " will be added as process emulation grows.",
        NULL,
        0
    },
    {
        "disown",
        "Remove jobs from the shell's job table.",
        "disown [job ...]",
        "Marks the supplied jobs as disowned so future jobs, fg, bg, and wait "
        "commands ignore them. With no arguments the most recent job is "
        "disowned. Job specifiers may be numeric indexes or begin with '%'.",
        NULL,
        0
    },
    {
        "kill",
        "Send signals to processes or jobs.",
        "kill [-s signal|-n signum|-SIGNAL] pid|job ... | kill -l [name|number ...]",
        "Delivers SIGNAL (default TERM) to each PID or job specifier. Numeric targets"
        " follow POSIX kill semantics so negative values address process groups and"
        " zero targets the current process group. With -l and no operands the builtin"
        " lists known signals; when arguments follow -l numeric values print their"
        " signal names and textual names print their corresponding signal numbers.",
        NULL,
        0
    },
    {
        "local",
        "Activate the shell's local scope flag.",
        "local",
        "Sets the runtime flag that marks the current function scope as local-aware."
        " Accepts no arguments.",
        NULL,
        0
    },
    {
        "logout",
        "Exit the shell when running as a login session.",
        "logout [status]",
        "Terminates the shell only when the login_shell shopt option is enabled."
        " With STATUS the supplied numeric value becomes the exit status."
        " Outside a login shell the builtin reports an error and leaves the"
        " session running.",
        NULL,
        0
    },
    {
        "let",
        "Evaluate arithmetic expressions and assignments.",
        "let arg [arg ...]",
        "Each ARG is evaluated with the arithmetic parser. Simple expressions return"
        " their numeric value, while assignments such as NAME=EXPR and the compound"
        " forms NAME+=EXPR, NAME-=EXPR, NAME*=EXPR, NAME/=EXPR, and NAME%=EXPR update"
        " shell variables. The exit status is 0 when the final value is non-zero and"
        " 1 otherwise.",
        NULL,
        0
    },
    {
        "pwd",
        "Print the current working directory.",
        "pwd",
        "Outputs the absolute path returned by getcwd(3).",
        NULL,
        0
    },
    {
        "chmod",
        "Change file permissions.",
        "chmod MODE file [file ...]",
        "Applies the octal MODE to each file path. Only numeric modes are supported "
        "on iOS builds.",
        NULL,
        0
    },
    {
        "read",
        "Read a line from standard input.",
        "read [-p prompt] [name ...]",
        "Reads a line, splits it into words, and assigns them to the requested"
        " environment variables. Without explicit names the value is stored in"
        " REPLY. Only the -p prompt option is supported.",
        NULL,
        0
    },
    {
        "return",
        "Return from the current shell function.",
        "return [status]",
        "Exits the innermost shell function. The optional status is parsed as an"
        " integer and limited to the range 0–255.",
        NULL,
        0
    },
    {
        "set",
        "Update shell option flags.",
        "set [--] [-e|+e] [-o errexit|+o errexit]",
        "Toggles the shell's errexit flag. Options other than -e/+e and"
        " -o/+o errexit are rejected.",
        NULL,
        0
    },
    {
        "shopt",
        "Toggle optional shell behaviours.",
        "shopt [-pqsu] [name ...]",
        "Lists available shell options, reports their state, or updates them. The"
        " implementation recognises the standard Bash shopt flags; -p prints in"
        " command form, -q suppresses output, and -s/-u enable or disable the"
        " named options.",
        NULL,
        0
    },
    {
        "setenv",
        "Set or print environment variables.",
        "setenv [name [value]]",
        "With no arguments prints the environment. NAME assigns an empty string"
        " and NAME VALUE assigns the provided string. Invalid names raise an"
        " error.",
        NULL,
        0
    },
    {
        "type",
        "Describe how the shell interprets names.",
        "type [-a] [-p|-P] [-t] name ...",
        "Without options prints the first match for each NAME. -t prints only the"
        " classification (alias, function, builtin, or file). -p prints the first"
        " executable path, -P forces the default PATH, and -a includes every"
        " result discovered during lookup.",
        NULL,
        0
    },
    {
        "shift",
        "Rotate positional parameters to the left.",
        "shift [count]",
        "Removes COUNT positional parameters (default 1). COUNT must be a"
        " non-negative integer that does not exceed the current parameter"
        " count.",
        NULL,
        0
    },
    {
        "source",
        "Execute a file in the current shell environment.",
        "source file [args ...]",
        "Loads the named file and executes it without spawning a subshell."
        " Positional parameters are temporarily replaced when arguments are"
        " supplied. The '.' builtin is an alias.",
        kShellHelpSourceAliases,
        1
    },
    {
        "trap",
        "Toggle the shell's trap flag.",
        "trap [commands ...]",
        "Calling trap with arguments enables the runtime trap flag; running it"
        " with no arguments clears the flag. Trap handlers are not yet"
        " parameterised per signal.",
        NULL,
        0
    },
    {
        "times",
        "Display accumulated CPU usage statistics.",
        "times",
        "Prints two lines of timing data: the shell's user and system time"
        " followed by the cumulative user and system time of terminated child"
        " processes. Values are reported as minutes and fractional seconds.",
        NULL,
        0
    },
    {
        "unset",
        "Remove variables from the environment.",
        "unset name [name ...]",
        "Clears each named environment variable via unsetenv(3).",
        NULL,
        0
    },
    {
        "unsetenv",
        "Alias for unset.",
        "unsetenv name [name ...]",
        "This is a synonym for unset and removes environment variables via"
        " unsetenv(3).",
        NULL,
        0
    },
    {
        "wait",
        "Wait for a job to change state.",
        "wait [job]",
        "Waits for the specified job (or the most recent one) to finish. Job"
        " specifiers may be numeric indexes or begin with '%'.",
        NULL,
        0
    },
    {
        ":",
        "Do nothing and succeed.",
        ":",
        "A no-op builtin that always reports success.",
        NULL,
        0
    }
};

static const ShellHelpTopic *shellHelpFindTopic(const char *name) {
    if (!name) {
        return NULL;
    }
    size_t topic_count = sizeof(kShellHelpTopics) / sizeof(kShellHelpTopics[0]);
    for (size_t i = 0; i < topic_count; ++i) {
        const ShellHelpTopic *topic = &kShellHelpTopics[i];
        if (strcasecmp(name, topic->name) == 0) {
            return topic;
        }
        for (size_t j = 0; j < topic->alias_count; ++j) {
            if (topic->aliases && strcasecmp(name, topic->aliases[j]) == 0) {
                return topic;
            }
        }
    }
    return NULL;
}

static void shellHelpPrintOverview(void) {
    printf("help\n");
    printf("exsh is the PSCAL shell front end, providing an interactive environment for orchestrating VM builtins and external commands.\n\n");
    printf("exsh can evaluate shell scripts, manage pipelines, and redirect input and output just like a traditional POSIX-style shell. Use '>' to overwrite files, '>>' to append, and '|' to connect commands.\n\n");
    printf("- Source ~/.exshrc to customise prompts, aliases, and startup behaviour.\n");
    printf("- Use bookmark helpers (bookmark, showmarks, jump) to save and revisit directories quickly.\n");
    printf("- Manage jobs with bg, fg, jobs, wait, and trap.\n");
    printf("- exit leaves the shell; builtin invokes PSCAL VM helpers directly.\n\n");
    printf("- exsh loads ~/.exshrc on startup when the file is present.\n\n");
    printf("- Navigate the interface with familiar terminal controls when used in supporting environments.\n");
    printf("- Edit with vim or pico, transfer data via curl, scp, or sftp, and inspect the network with ping, host, or nslookup.\n");
    printf("- Extend the runtime with PSCAL packages and builtins compiled via the toolchain.\n\n");
    printf("- Compiled scripts are cached in ~/.pscal/bc_cache; use --no-cache to force recompilation.\n\n");
    printf("Documentation: %s/exsh_overview.md.\n", PSCAL_DOCS_DIR);
    printf("Support: Report issues on the GitHub PSCAL project tracker or Discord community channels.\n\n");
    printf("Type 'help -l' for a list of functions, or 'help <function>' for help on a specific shell function.\n");
}

static void shellHelpPrintCatalog(void) {
    size_t topic_count = sizeof(kShellHelpTopics) / sizeof(kShellHelpTopics[0]);
    size_t width = strlen("Builtin");
    char display[64];

    for (size_t i = 0; i < topic_count; ++i) {
        const ShellHelpTopic *topic = &kShellHelpTopics[i];
        const char *name = topic->name;
        if (topic->alias_count > 0 && topic->aliases) {
            snprintf(display, sizeof(display), "%s (%s)", name, topic->aliases[0]);
            name = display;
        }
        size_t len = strlen(name);
        if (len > width) {
            width = len;
        }
    }

    printf("exsh builtins. Type 'help <function>' for detailed usage.\n\n");
    printf("%-*s  %s\n", (int)width, "Builtin", "Summary");
    printf("%-*s  %s\n", (int)width, "------", "-------");

    for (size_t i = 0; i < topic_count; ++i) {
        const ShellHelpTopic *topic = &kShellHelpTopics[i];
        const char *name = topic->name;
        if (topic->alias_count > 0 && topic->aliases) {
            snprintf(display, sizeof(display), "%s (%s)", name, topic->aliases[0]);
            name = display;
        }
        printf("%-*s  %s\n", (int)width, name, topic->summary);
    }
}

static void shellHelpPrintTopic(const ShellHelpTopic *topic) {
    if (!topic) {
        return;
    }
    printf("%s - %s\n", topic->name, topic->summary);
    if (topic->alias_count > 0 && topic->aliases) {
        printf("Aliases: ");
        for (size_t i = 0; i < topic->alias_count; ++i) {
            printf("%s%s", topic->aliases[i], (i + 1 < topic->alias_count) ? " " : "\n");
        }
    }
    if (topic->usage && *topic->usage) {
        printf("Usage: %s\n", topic->usage);
    }
    if (topic->detail && *topic->detail) {
        printf("\n%s\n", topic->detail);
    }
}

Value vmBuiltinShellHelp(VM *vm, int arg_count, Value *args) {
    if (arg_count == 0) {
        shellHelpPrintOverview();
        shellUpdateStatus(0);
        return makeVoid();
    }

    if (arg_count > 1) {
        runtimeError(vm, "help: expected at most one builtin name");
        shellUpdateStatus(1);
        return makeVoid();
    }

    if (args[0].type != TYPE_STRING || !args[0].s_val || args[0].s_val[0] == '\0') {
        runtimeError(vm, "help: expected builtin name as string");
        shellUpdateStatus(1);
        return makeVoid();
    }

    const char *requested = args[0].s_val;
    if (strcmp(requested, "-l") == 0) {
        shellHelpPrintCatalog();
        shellUpdateStatus(0);
        return makeVoid();
    }

    const char *canonical = shellBuiltinCanonicalName(requested);
    const ShellHelpTopic *topic = shellHelpFindTopic(canonical);
    if (!topic) {
        runtimeError(vm, "help: unknown builtin '%s'", requested);
        shellUpdateStatus(1);
        return makeVoid();
    }

    shellHelpPrintTopic(topic);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellAlias(VM *vm, int arg_count, Value *args) {
    if (arg_count == 0) {
        for (size_t i = 0; i < gShellAliasCount; ++i) {
            printf("alias %s='%s'\n", gShellAliases[i].name, gShellAliases[i].value);
        }
        shellUpdateStatus(0);
        return makeVoid();
    }
    for (int i = 0; i < arg_count; ++i) {
        if (args[i].type != TYPE_STRING || !args[i].s_val) {
            runtimeError(vm, "alias: expected name=value");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *assignment = args[i].s_val;
        const char *eq = strchr(assignment, '=');
        if (!eq || eq == assignment) {
            runtimeError(vm, "alias: invalid assignment '%s'", assignment);
            shellUpdateStatus(1);
            return makeVoid();
        }
        size_t name_len = (size_t)(eq - assignment);
        char *name = strndup(assignment, name_len);
        const char *value = eq + 1;
        if (!name) {
            runtimeError(vm, "alias: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        if (!shellSetAlias(name, value)) {
            free(name);
            runtimeError(vm, "alias: failed to store alias");
            shellUpdateStatus(1);
            return makeVoid();
        }
        free(name);
    }
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellUnalias(VM *vm, int arg_count, Value *args) {
    bool clear_all = false;
    int index = 0;

    while (index < arg_count) {
        if (args[index].type != TYPE_STRING || !args[index].s_val) {
            shellReportRecoverableError(vm, false, "unalias: usage: unalias [-a] name [name ...]");
            shellUpdateStatus(2);
            return makeVoid();
        }
        const char *arg = args[index].s_val;
        if (strcmp(arg, "--") == 0) {
            index++;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') {
            break;
        }
        if (strcmp(arg, "-a") == 0) {
            clear_all = true;
            index++;
            continue;
        }
        shellReportRecoverableError(vm, true, "unalias: %s: invalid option", arg);
        shellReportRecoverableError(vm, false, "unalias: usage: unalias [-a] name [name ...]");
        shellUpdateStatus(2);
        return makeVoid();
    }

    if (clear_all) {
        for (; index < arg_count; ++index) {
            if (args[index].type != TYPE_STRING || !args[index].s_val || args[index].s_val[0] == '\0') {
                shellReportRecoverableError(vm, false, "unalias: usage: unalias [-a] name [name ...]");
                shellUpdateStatus(2);
                return makeVoid();
            }
        }
        shellClearAliases();
        shellUpdateStatus(0);
        return makeVoid();
    }

    if (index >= arg_count) {
        shellReportRecoverableError(vm, false, "unalias: usage: unalias [-a] name [name ...]");
        shellUpdateStatus(2);
        return makeVoid();
    }

    bool ok = true;
    for (; index < arg_count; ++index) {
        if (args[index].type != TYPE_STRING || !args[index].s_val || args[index].s_val[0] == '\0') {
            shellReportRecoverableError(vm, false, "unalias: usage: unalias [-a] name [name ...]");
            shellUpdateStatus(2);
            return makeVoid();
        }
        const char *name = args[index].s_val;
        if (!shellRemoveAlias(name)) {
            shellReportRecoverableError(vm, true, "unalias: %s: not found", name);
            ok = false;
        }
    }

    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellCaller(VM *vm, int arg_count, Value *args) {
    long depth = 0;
    if (arg_count > 1) {
        runtimeError(vm, "caller: too many arguments");
        shellUpdateStatus(2);
        return makeVoid();
    }
    if (arg_count == 1) {
        if (args[0].type != TYPE_STRING || !args[0].s_val) {
            runtimeError(vm, "caller: expected numeric depth");
            shellUpdateStatus(2);
            return makeVoid();
        }
        char *end = NULL;
        depth = strtol(args[0].s_val, &end, 10);
        if (!end || *end != '\0') {
            runtimeError(vm, "caller: %s: numeric argument required", args[0].s_val);
            shellUpdateStatus(2);
            return makeVoid();
        }
        if (depth < 0) {
            runtimeError(vm, "caller: negative depth not allowed");
            shellUpdateStatus(2);
            return makeVoid();
        }
    }

    size_t stack_depth = shellRuntimeCallStackDepth();
    if (stack_depth == 0 || (size_t)depth >= stack_depth) {
        shellUpdateStatus(1);
        return makeVoid();
    }

    const ShellCallFrame *frame = shellRuntimeCallFrameAtDepth((size_t)depth);
    if (!frame) {
        shellUpdateStatus(1);
        return makeVoid();
    }

    int line = frame->line >= 0 ? frame->line : 0;
    const char *function = (frame->caller_function && *frame->caller_function)
                               ? frame->caller_function
                               : "NULL";
    const char *source = (frame->source && *frame->source) ? frame->source : NULL;

    if (depth == 0) {
        if (source) {
            printf("%d %s\n", line, source);
        } else {
            printf("%d %s\n", line, function);
        }
    } else {
        if (source) {
            printf("%d %s %s\n", line, function, source);
        } else {
            printf("%d %s\n", line, function);
        }
    }

    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellHistory(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    for (size_t i = 0; i < gShellHistory.count; ++i) {
        printf("%zu  %s\n", i + 1, gShellHistory.entries[i]);
    }
    shellUpdateStatus(0);
    return makeVoid();
}

#if defined(PSCAL_TARGET_IOS)
static ShellJob *shellFindJobByPid(pid_t pid) {
    for (size_t i = 0; i < gShellJobCount; ++i) {
        ShellJob *job = &gShellJobs[i];
        if (!job->pids) {
            continue;
        }
        for (size_t j = 0; j < job->pid_count; ++j) {
            if (job->pids[j] == pid) {
                return job;
            }
        }
    }
    return NULL;
}

static const char *shellPsStateForEntry(const ShellJob *job, const VProcSnapshot *snap) {
    if (snap && snap->exited) {
        return "exited";
    }
    if (job && job->disowned) {
        return "disowned";
    }
    if ((snap && snap->stopped) || (job && job->stopped)) {
        return "stopped";
    }
    if (job && job->running) {
        return "running";
    }
    return "running";
}

static void shellPsPrintLine(pid_t pid, pid_t pgid, const char *state, int utime_cs, int stime_cs, const char *command) {
    char pgid_buf[16];
    if (pgid > 0) {
        snprintf(pgid_buf, sizeof(pgid_buf), "%d", (int)pgid);
    } else {
        snprintf(pgid_buf, sizeof(pgid_buf), "-");
    }
    double utime_s = 0.0, stime_s = 0.0;
    vprocFormatCpuTimes(utime_cs, stime_cs, &utime_s, &stime_s);
    printf("%-6d %-6s %-9s %-6.1f %-6.1f %s\n", (int)pid, pgid_buf,
           state ? state : "running", utime_s, stime_s, command ? command : "");
}

static void shellPsPrintLineTree(const VProcSnapshot *snap,
                                 pid_t pgid,
                                 const char *state,
                                 const char *command,
                                 int depth) {
    if (!snap) {
        return;
    }
    char pgid_buf[16];
    if (pgid > 0) {
        snprintf(pgid_buf, sizeof(pgid_buf), "%d", (int)pgid);
    } else {
        snprintf(pgid_buf, sizeof(pgid_buf), "-");
    }

    double utime_s = 0.0, stime_s = 0.0;
    vprocFormatCpuTimes(snap->rusage_utime, snap->rusage_stime, &utime_s, &stime_s);

    char indent[96];
    size_t used = 0;
    for (int d = 0; d < depth && used + 2 < sizeof(indent); ++d) {
        indent[used++] = ' ';
        indent[used++] = ' ';
    }
    indent[used] = '\0';

    printf("%-6d %-6d %-6s %-6d %-9s %-6.1f %-6.1f %s%s\n",
           (int)snap->pid,
           (int)snap->parent_pid,
           pgid_buf,
           (int)snap->sid,
           state ? state : "running",
           utime_s,
           stime_s,
           indent,
           command ? command : "");
}
#endif

static int shellJobIndexCompareById(const void *left, const void *right) {
    size_t lhs = *(const size_t *)left;
    size_t rhs = *(const size_t *)right;
    size_t lhs_id = gShellJobs[lhs].id;
    size_t rhs_id = gShellJobs[rhs].id;
    if (lhs_id < rhs_id) {
        return -1;
    }
    if (lhs_id > rhs_id) {
        return 1;
    }
    return 0;
}

Value vmBuiltinShellJobs(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    shellCollectJobs();
    size_t current_index = SIZE_MAX;
    size_t previous_index = SIZE_MAX;
    for (size_t i = gShellJobCount; i > 0; --i) {
        ShellJob *job = &gShellJobs[i - 1];
        if (job->disowned) {
            continue;
        }
        if (current_index == SIZE_MAX) {
            current_index = i - 1;
        } else {
            previous_index = i - 1;
            break;
        }
    }

    size_t visible_count = 0;
    for (size_t i = 0; i < gShellJobCount; ++i) {
        if (!gShellJobs[i].disowned) {
            visible_count++;
        }
    }
    bool show_markers = shellRuntimeIsInteractive();

    size_t *indices = NULL;
    if (visible_count > 0) {
        indices = malloc(sizeof(size_t) * visible_count);
    }
    if (indices) {
        size_t idx_count = 0;
        for (size_t i = 0; i < gShellJobCount; ++i) {
            if (!gShellJobs[i].disowned) {
                indices[idx_count++] = i;
            }
        }
        if (idx_count > 1) {
            qsort(indices, idx_count, sizeof(size_t), shellJobIndexCompareById);
        }
        for (size_t i = 0; i < idx_count; ++i) {
            size_t job_index = indices[i];
            ShellJob *job = &gShellJobs[job_index];
            const char *state = job->stopped ? "Stopped" : "Running";
            const char *command = job->command ? job->command : "";
            if (show_markers) {
                char marker = ' ';
                if (job_index == current_index) {
                    marker = '+';
                } else if (job_index == previous_index) {
                    marker = '-';
                }
                shellFdPrintf(STDOUT_FILENO, "[%zu] %c %s %s\n", job->id, marker, state, command);
            } else {
                shellFdPrintf(STDOUT_FILENO, "[%zu] %s %s\n", job->id, state, command);
            }
        }
        free(indices);
    } else {
        for (size_t i = 0; i < gShellJobCount; ++i) {
            ShellJob *job = &gShellJobs[i];
            if (job->disowned) {
                continue;
            }
            const char *state = job->stopped ? "Stopped" : "Running";
            const char *command = job->command ? job->command : "";
            if (show_markers) {
                char marker = ' ';
                if (i == current_index) {
                    marker = '+';
                } else if (i == previous_index) {
                    marker = '-';
                }
                shellFdPrintf(STDOUT_FILENO, "[%zu] %c %s %s\n", job->id, marker, state, command);
            } else {
                shellFdPrintf(STDOUT_FILENO, "[%zu] %s %s\n", job->id, state, command);
            }
        }
    }
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellDisown(VM *vm, int arg_count, Value *args) {
    shellCollectJobs();
    if (arg_count == 0) {
        size_t index = 0;
        if (!shellResolveJobIndex(vm, "disown", arg_count, args, &index)) {
            shellUpdateStatus(1);
            return makeVoid();
        }
        gShellJobs[index].disowned = true;
        shellUpdateStatus(0);
        return makeVoid();
    }

    bool ok = true;
    size_t resolved_count = 0;
    size_t *indices = NULL;
    if (arg_count > 0) {
        indices = malloc(sizeof(size_t) * (size_t)arg_count);
        if (!indices) {
            runtimeError(vm, "disown: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
    }

    for (int i = 0; i < arg_count; ++i) {
        size_t index = 0;
        if (!shellParseJobSpecifier(vm, "disown", args[i], &index)) {
            ok = false;
            continue;
        }
        indices[resolved_count++] = index;
    }

    for (size_t i = 0; i < resolved_count; ++i) {
        gShellJobs[indices[i]].disowned = true;
    }

    free(indices);

    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

static bool shellKillParseLong(Value value, long *out_value) {
    if (!out_value) {
        return false;
    }
    if (IS_INTLIKE(value)) {
        *out_value = (long)AS_INTEGER(value);
        return true;
    }
    if (value.type == TYPE_STRING && value.s_val) {
        errno = 0;
        char *endptr = NULL;
        long parsed = strtol(value.s_val, &endptr, 10);
        if (errno != 0 || !endptr || *endptr != '\0') {
            return false;
        }
        *out_value = parsed;
        return true;
    }
    return false;
}

static bool shellKillParseSignal(Value value, bool numeric_only, int *out_signo) {
    if (!out_signo) {
        return false;
    }
    if (numeric_only) {
        long parsed = 0;
        if (!shellKillParseLong(value, &parsed)) {
            return false;
        }
        if (parsed < 0 || parsed >= NSIG) {
            return false;
        }
        *out_signo = (int)parsed;
        return true;
    }
    if (value.type == TYPE_STRING && value.s_val) {
        return shellRuntimeParseSignal(value.s_val, out_signo);
    }
    if (IS_INTLIKE(value)) {
        long parsed = (long)AS_INTEGER(value);
        if (parsed < 0 || parsed >= NSIG) {
            return false;
        }
        *out_signo = (int)parsed;
        return true;
    }
    if (value.type == TYPE_STRING && value.s_val) {
        return shellRuntimeParseSignal(value.s_val, out_signo);
    }
    return false;
}

static bool shellKillTryJobSpec(const char *text, size_t *out_index) {
    if (!text || !*text || !out_index) {
        return false;
    }
    while (isspace((unsigned char)*text)) {
        ++text;
    }
    if (*text != '%') {
        return false;
    }
    ++text;
    while (isspace((unsigned char)*text)) {
        ++text;
    }
    if (!*text) {
        return false;
    }
    char *endptr = NULL;
    long vis = strtol(text, &endptr, 10);
    while (endptr && *endptr && isspace((unsigned char)*endptr)) {
        ++endptr;
    }
    if (endptr && *endptr == '\0' && vis > 0) {
        return shellJobFindVisibleIndex((size_t)vis, out_index);
    }
    return false;
}

static void shellKillPrintSignalTable(void) {
    int column = 0;
    char name[32];
    for (int signo = 1; signo < NSIG; ++signo) {
        if (!shellRuntimeSignalName(signo, true, name, sizeof(name))) {
            continue;
        }
        printf("%2d) %-12s", signo, name);
        column++;
        if (column == 5) {
            printf("\n");
            column = 0;
        } else {
            printf(" ");
        }
    }
    if (column != 0) {
        printf("\n");
    }
}

static bool shellKillSignalJob(VM *vm, const char *spec_text, size_t index, int signo) {
    if (index >= gShellJobCount) {
        fprintf(stderr, "kill: invalid job index\n");
        return false;
    }
    ShellJob *job = &gShellJobs[index];
    bool sent = false;
    bool probe = (signo == 0);
    bool terminate = !probe &&
                     !(signo == SIGTSTP || signo == SIGSTOP || signo == SIGTTIN || signo == SIGTTOU || signo == SIGCONT);
    if (probe) {
        if (job->pgid > 0 && shellJobControlSendPgid(job->pgid, 0) == 0) {
            return true;
        }
        for (size_t i = 0; i < job->pid_count; ++i) {
            pid_t pid = job->pids[i];
            if (pid <= 0) {
                continue;
            }
            if (shellJobControlSendPid(pid, 0) == 0) {
                return true;
            }
        }
        fprintf(stderr, "kill: %s: no associated processes\n", spec_text ? spec_text : "job");
        return false;
    }
    if (terminate && job->pgid > 0) {
        /* Deliver to the whole synthetic group; safer for pipelines and job-spec parity. */
        if (shellJobControlSendPgid(job->pgid, signo) == 0) {
            sent = true;
        }
    }

    for (size_t i = 0; i < job->pid_count; ++i) {
        pid_t pid = job->pids[i];
        if (pid <= 0) {
            continue;
        }
        if (!terminate || !sent) {
            /* If group kill failed or we are delivering non-terminating signals, send per-pid. */
            if (shellJobControlSendPid(pid, signo) != 0) {
                fprintf(stderr, "kill: %s: %s\n",
                        spec_text ? spec_text : "job",
                        strerror(errno));
                return false;
            }
        }
        sent = true;
        int status = 0;
        ShellWaitResult wait_res;
        int err = shellWaitPidDetailed(pid, !terminate, &wait_res);
        if (err != 0) {
            continue;
        }
        if (wait_res.stopped) {
            shellJobMarkStopped(job, wait_res.status_mapped);
        } else if (WIFEXITED(wait_res.status_raw) || WIFSIGNALED(wait_res.status_raw)) {
            if (!terminate) {
                job->pids[i] = -1;
            }
        } else if (!terminate) {
            job->pids[i] = -1;
        }
    }
    if (!sent) {
        fprintf(stderr, "kill: %s: no associated processes\n",
                spec_text ? spec_text : "job");
        return false;
    }
    job->last_status = 128 + signo;
    if (signo == SIGTSTP || signo == SIGSTOP || signo == SIGTTIN || signo == SIGTTOU) {
        shellJobMarkStopped(job, job->last_status);
    } else {
        job->running = false;
        job->stopped = false;
    }
    if (terminate) {
        shellRemoveJobAt(index);
        shellCollectJobs();
        return true;
    }
    (void)synthetic; /* avoid unused warning on non-iOS builds */
    return true;
}

Value vmBuiltinShellKill(VM *vm, int arg_count, Value *args) {
    shellCollectJobs();
    bool kill_dbg = getenv("PSCALI_KILL_DEBUG") != NULL;
    if (kill_dbg) {
        fprintf(stderr, "[kill] job_count=%zu args=%d\n", gShellJobCount, arg_count);
        for (size_t i = 0; i < gShellJobCount; ++i) {
            ShellJob *job = &gShellJobs[i];
            fprintf(stderr, "[kill]   job idx=%zu id=%zu pgid=%d disowned=%d stopped=%d running=%d pid_count=%zu\n",
                    i, job->id, (int)job->pgid, (int)job->disowned, (int)job->stopped, (int)job->running, job->pid_count);
            for (size_t j = 0; j < job->pid_count; ++j) {
                fprintf(stderr, "[kill]     pid[%zu]=%d\n", j, (int)job->pids[j]);
            }
        }
    }

    bool parsing_options = true;
    bool expect_signal_value = false;
    char pending_option = '\0';
    bool list_mode = false;
    int list_start = arg_count;
    int operand_start = arg_count;
    int signal_number = SIGTERM;

    for (int i = 0; i < arg_count;) {
        Value value = args[i];
        const char *text = (value.type == TYPE_STRING) ? value.s_val : NULL;

        if (expect_signal_value) {
            bool numeric_only = (pending_option == 'n');
            int signo = 0;
            if (!shellKillParseSignal(value, numeric_only, &signo)) {
                fprintf(stderr, "kill: invalid signal\n");
                shellUpdateStatus(1);
                return makeVoid();
            }
            signal_number = signo;
            expect_signal_value = false;
            pending_option = '\0';
            parsing_options = true;
            ++i;
            continue;
        }

        if (list_mode) {
            break;
        }

        if (parsing_options && text) {
            if (strcmp(text, "--") == 0) {
                parsing_options = false;
                operand_start = i + 1;
                ++i;
                break;
            }
            if (strcmp(text, "-l") == 0) {
                list_mode = true;
                list_start = i + 1;
                ++i;
                break;
            }
            if (strcmp(text, "-s") == 0 || strcmp(text, "-n") == 0) {
                pending_option = text[1];
                expect_signal_value = true;
                ++i;
                continue;
            }
            if (text[0] == '-' && text[1] != '\0') {
                int signo = 0;
                if (shellRuntimeParseSignal(text + 1, &signo)) {
                    signal_number = signo;
                    parsing_options = false;
                    operand_start = i + 1;
                    ++i;
                    break;
                }
            }
            parsing_options = false;
            operand_start = i;
            break;
        } else if (parsing_options) {
            parsing_options = false;
            operand_start = i;
            break;
        } else {
            operand_start = i;
            break;
        }
    }

    if (expect_signal_value) {
        fprintf(stderr, "kill: option requires an argument\n");
        shellUpdateStatus(1);
        return makeVoid();
    }

    if (list_mode) {
        bool ok = true;
        if (list_start >= arg_count) {
            shellKillPrintSignalTable();
        } else {
            for (int i = list_start; i < arg_count; ++i) {
                Value value = args[i];
                long numeric = 0;
                if (shellKillParseLong(value, &numeric)) {
                    char name[32];
                    if (!shellRuntimeSignalName((int)numeric, false, name, sizeof(name))) {
                        fprintf(stderr, "kill: %ld: unknown signal\n", numeric);
                        ok = false;
                        continue;
                    }
                    printf("%s\n", name);
                    continue;
                }
                if (value.type != TYPE_STRING || !value.s_val) {
                    runtimeError(vm, "kill: invalid signal");
                    ok = false;
                    continue;
                }
                int signo = 0;
                if (!shellRuntimeParseSignal(value.s_val, &signo)) {
                    fprintf(stderr, "kill: invalid signal '%s'\n", value.s_val);
                    ok = false;
                    continue;
                }
                printf("%d\n", signo);
            }
        }
        shellUpdateStatus(ok ? 0 : 1);
        return makeVoid();
    }

    if (operand_start >= arg_count) {
        fprintf(stderr, "kill: missing operand\n");
        shellUpdateStatus(1);
        return makeVoid();
    }

    bool overall_ok = true;
    for (int i = operand_start; i < arg_count; ++i) {
        Value target = args[i];
        if (target.type == TYPE_STRING && target.s_val) {
            const char *text = target.s_val;
            while (isspace((unsigned char)*text)) {
                ++text;
            }
            if (*text == '%') {
                size_t index = 0;
                if (shellKillTryJobSpec(text, &index) && index < gShellJobCount) {
                    if (!shellKillSignalJob(vm, target.s_val, index, signal_number)) {
                        overall_ok = false;
                    }
                } else {
                    fprintf(stderr, "kill: %s: no such job\n", target.s_val);
                    overall_ok = false;
                }
                continue;
            }
        }
        long pid_value = 0;
        if (!shellKillParseLong(target, &pid_value)) {
            if (target.type == TYPE_STRING && target.s_val) {
                fprintf(stderr, "kill: invalid pid '%s'\n", target.s_val);
            } else {
                fprintf(stderr, "kill: invalid pid\n");
            }
            overall_ok = false;
            continue;
        }
        int rc = 0;
        pid_t target_pid = (pid_t)pid_value;
        if (target_pid < 0) {
            pid_t pgid = -target_pid;
            bool any = false;
            for (size_t j = 0; j < gShellJobCount; ++j) {
                ShellJob *job = &gShellJobs[j];
                if (job->pgid == pgid) {
                    for (size_t k = 0; k < job->pid_count; ++k) {
                        if (job->pids[k] > 0) {
                            any = true;
                            shellJobControlSendPid(job->pids[k], signal_number);
                        }
                    }
                }
            }
            if (!any) {
                rc = shellJobControlSendPgid(pgid, signal_number);
            }
        } else {
            rc = shellJobControlSendPid(target_pid, signal_number);
        }
        if (rc != 0) {
            fprintf(stderr, "kill: (%ld) %s\n", pid_value, strerror(errno));
            overall_ok = false;
        }
    }

    /* Pull in any state changes caused by the signals (stops, exits) so jobs/
     * lps reflect the new status immediately. */
    shellCollectJobs();
    shellUpdateStatus(overall_ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellFg(VM *vm, int arg_count, Value *args) {
    shellCollectJobs();
    shellEnsureJobControl();
    size_t index = 0;
    if (!shellResolveJobIndex(vm, "fg", arg_count, args, &index)) {
        shellUpdateStatus(1);
        return makeVoid();
    }
    ShellJob *job = &gShellJobs[index];
    bool job_control = gShellRuntime.job_control_enabled && job->pgid > 0;
    if (job_control) {
        shellJobControlSetForeground(job->pgid);
    }
    /* Prefer group continue; fallback to per-pid to cover synthetic tasks. */
    shellJobContinue(job, true);
    if (job_control) {
        if (job->command && job->command[0]) {
            shellFdPrintf(STDOUT_FILENO, "[%zu]  + continued  %s\n", job->id, job->command);
        } else {
            shellFdPrintf(STDOUT_FILENO, "[%zu]  + continued\n", job->id);
        }
    }
    shellJobMarkRunning(job);
    int final_status = job->last_status;
    for (size_t i = 0; i < job->pid_count; ++i) {
        pid_t pid = job->pids[i];
        if (pid <= 0) {
            continue;
        }
        int status = 0;
        pid_t res;
        do {
            res = vprocWaitPidShim(pid, &status, WUNTRACED);
        } while (res < 0 && errno == EINTR);
        if (res < 0) {
            continue;
        }
        if (WIFSTOPPED(status)) {
            shellJobMarkStopped(job, shellStatusFromWait(status));
            if (job_control) {
                shellJobControlRestoreForeground();
            }
            shellUpdateStatus(job->last_status);
            return makeVoid();
        }
        final_status = shellStatusFromWait(status);
        job->pids[i] = -1;
    }
    if (job_control) {
        shellJobControlRestoreForeground();
    }
    shellRemoveJobAt(index);
    shellUpdateStatus(final_status);
    return makeVoid();
}

Value vmBuiltinShellBg(VM *vm, int arg_count, Value *args) {
    shellCollectJobs();
    shellEnsureJobControl();
    size_t index = 0;
    if (!shellResolveJobIndex(vm, "bg", arg_count, args, &index)) {
        shellUpdateStatus(1);
        return makeVoid();
    }
    ShellJob *job = &gShellJobs[index];
    shellJobContinue(job, true);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellWait(VM *vm, int arg_count, Value *args) {
    shellCollectJobs();
    size_t visible_count = shellJobVisibleCount();
    if (arg_count == 1 && gShellJobCount == 0 && gShellRuntime.last_removed_job_valid) {
        long target = -1;
        Value spec = args[0];
        if (spec.type == TYPE_STRING && spec.s_val) {
            const char *text = spec.s_val;
            if (text[0] == '%') {
                text++;
            }
            if (text && *text) {
                char *end = NULL;
                long parsed = strtol(text, &end, 10);
                if (end && *end == '\0' && parsed > 0) {
                    target = parsed;
                }
            }
        } else if (IS_INTLIKE(spec)) {
            target = (long)AS_INTEGER(spec);
        }
        if (target > 0 && gShellRuntime.last_removed_job_id == (size_t)target) {
            int status = gShellRuntime.last_removed_job_status;
            gShellRuntime.last_removed_job_valid = false;
            shellUpdateStatus(status);
            return makeVoid();
        }
    }
    if (arg_count == 0) {
        if (visible_count == 0) {
            shellUpdateStatus(0);
            return makeVoid();
        }
        int final_status = 0;
        while (shellJobVisibleCount() > 0) {
            /* Match Bash's tendency to treat the most recent job as "current". */
            size_t index = 0;
            bool found = false;
            for (size_t i = gShellJobCount; i > 0; --i) {
                ShellJob *job = &gShellJobs[i - 1];
                if (!job->disowned) {
                    index = i - 1;
                    found = true;
                    break;
                }
            }
            if (!found) {
                break;
            }
            ShellJob *job = &gShellJobs[index];
            int job_status = job->last_status;
            for (size_t i = 0; i < job->pid_count; ++i) {
                pid_t pid = job->pids[i];
                if (pid <= 0) {
                    continue;
                }
                int status = 0;
                pid_t res;
                do {
                    res = vprocWaitPidShim(pid, &status, 0);
                } while (res < 0 && errno == EINTR);
                if (res < 0) {
                    continue;
                }
                job_status = shellStatusFromWait(status);
                job->pids[i] = -1;
            }
            shellRemoveJobAt(index);
            final_status = job_status;
            shellCollectJobs();
        }
        shellUpdateStatus(final_status);
        return makeVoid();
    }
    size_t index = 0;
    if (!shellResolveJobIndex(vm, "wait", arg_count, args, &index)) {
        int fallback_status = 1;
        if (arg_count == 1) {
            long target = -1;
            Value spec = args[0];
            if (spec.type == TYPE_STRING && spec.s_val) {
                const char *text = spec.s_val;
                if (text[0] == '%') {
                    text++;
                }
                if (text && *text) {
                    char *end = NULL;
                    long parsed = strtol(text, &end, 10);
                    if (end && *end == '\0' && parsed > 0) {
                        target = parsed;
                    }
                }
            } else if (IS_INTLIKE(spec)) {
                target = (long)AS_INTEGER(spec);
            }
            if (target > 0 && gShellRuntime.last_removed_job_valid &&
                gShellRuntime.last_removed_job_id == (size_t)target) {
                fallback_status = gShellRuntime.last_removed_job_status;
                gShellRuntime.last_removed_job_valid = false;
                shellUpdateStatus(fallback_status);
                return makeVoid();
            }
        }
        shellUpdateStatus(fallback_status);
        return makeVoid();
    }
    ShellJob *job = &gShellJobs[index];
    int final_status = job->last_status;
    for (size_t i = 0; i < job->pid_count; ++i) {
        pid_t pid = job->pids[i];
        if (pid <= 0) {
            continue;
        }
        int status = 0;
        pid_t res;
        do {
            res = vprocWaitPidShim(pid, &status, 0);
        } while (res < 0 && errno == EINTR);
        if (res < 0) {
            continue;
        }
        final_status = shellStatusFromWait(status);
        job->pids[i] = -1;
    }
    shellRemoveJobAt(index);
    shellUpdateStatus(final_status);
    return makeVoid();
}

Value vmBuiltinShellWaitForThread(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        runtimeError(vm, "WaitForThread: expected thread id");
        shellUpdateStatus(1);
        return makeVoid();
    }

    Value thread_value = args[0];
    Value converted = makeVoid();
    bool converted_valid = false;

    if (thread_value.type == TYPE_STRING && thread_value.s_val && thread_value.s_val[0] != '\0') {
        const char *raw = thread_value.s_val;
        const char *id_text = raw;
        const char *colon = strchr(raw, ':');
        if (colon && colon[1] != '\0') {
            id_text = colon + 1;
        }

        long long parsed = 0;
        if (!shellParseSignedLongLong(id_text, &parsed)) {
            runtimeError(vm, "WaitForThread: %s: invalid thread id", raw);
            shellUpdateStatus(1);
            return makeVoid();
        }
        converted = makeInt(parsed);
        thread_value = converted;
        converted_valid = true;
    }

    if (!(thread_value.type == TYPE_THREAD || IS_INTLIKE(thread_value))) {
        runtimeError(vm, "WaitForThread: expected thread id");
        if (converted_valid) {
            freeValue(&converted);
        }
        shellUpdateStatus(1);
        return makeVoid();
    }

    long long thread_id = asI64(thread_value);
    if (thread_id <= 0 || thread_id > INT_MAX) {
        runtimeError(vm, "WaitForThread: %lld: invalid thread id", thread_id);
        if (converted_valid) {
            freeValue(&converted);
        }
        shellUpdateStatus(1);
        return makeVoid();
    }

    VM *thread_vm = vm;
    if (vm && vm->threadOwner) {
        thread_vm = vm->threadOwner;
    }

    bool joined = thread_vm ? vmJoinThreadById(thread_vm, (int)thread_id) : false;
    if (!joined && thread_vm && thread_vm != vm) {
        joined = vmJoinThreadById(vm, (int)thread_id);
        if (joined) {
            thread_vm = vm;
        }
    }
    if (!joined) {
        runtimeError(vm, "WaitForThread: %lld: invalid thread id", thread_id);
        if (converted_valid) {
            freeValue(&converted);
        }
        shellUpdateStatus(1);
        return makeVoid();
    }

    bool status_flag = true;
    bool consumed = false;
    if (vmThreadTakeResult(thread_vm, (int)thread_id, NULL, false, &status_flag, true)) {
        consumed = true;
    } else if (thread_vm && thread_vm != vm &&
               vmThreadTakeResult(vm, (int)thread_id, NULL, false, &status_flag, true)) {
        consumed = true;
        thread_vm = vm;
    }
    if (consumed) {
        shellUpdateStatus(status_flag ? 0 : 1);
    } else {
        shellUpdateStatus(0);
    }
    if (converted_valid) {
        freeValue(&converted);
    }
    return makeVoid();
}

Value vmBuiltinShellPsThreads(VM *vm, int arg_count, Value *args) {
    (void)arg_count;
    (void)args;

    VM *root = vm && vm->threadOwner ? vm->threadOwner : vm;
    Value stats_json = vmBuiltinThreadStatsJson(root, 0, NULL);
    const char *text = (stats_json.type == TYPE_STRING && stats_json.s_val) ? stats_json.s_val : "[]";
    printf("%s\n", text);
    freeValue(&stats_json);
    shellUpdateStatus(0);
    return makeVoid();
}

#if !defined(PSCAL_TARGET_IOS)
Value vmBuiltinShellPs(VM *vm, int arg_count, Value *args) {
    (void)arg_count;
    (void)args;
    runtimeError(vm, "ps: synthetic process view is only available on iOS");
    shellUpdateStatus(1);
    return makeVoid();
}
#else
Value vmBuiltinShellPs(VM *vm, int arg_count, Value *args) {
    (void)vm;
    bool tree = false;
    for (int i = 0; i < arg_count; ++i) {
        if (args[i].type != TYPE_STRING || !args[i].s_val) {
            continue;
        }
        const char *opt = args[i].s_val;
        if (strcmp(opt, "--tree") == 0) {
            tree = true;
        } else if (strcmp(opt, "--flat") == 0) {
            tree = false;
        } else if (strcmp(opt, "--help") == 0 || strcmp(opt, "-h") == 0) {
            printf("lps [--tree|--flat]\n");
            printf("  Show PSCAL synthetic process list.\n");
            printf("  --flat (default) show flat list.\n");
            printf("  --tree render parent/child tree.\n");
            shellUpdateStatus(0);
            return makeVoid();
        } else {
            printf("lps: unknown option '%s'\n", opt);
            shellUpdateStatus(1);
            return makeVoid();
        }
    }
    shellCollectJobs();

    size_t snapshot_cap = vprocSnapshot(NULL, 0);
    VProcSnapshot *snapshots = NULL;
    if (snapshot_cap > 0) {
        snapshots = (VProcSnapshot *)calloc(snapshot_cap, sizeof(VProcSnapshot));
    }
    size_t snapshot_count = snapshots ? vprocSnapshot(snapshots, snapshot_cap) : 0;

    size_t seen_cap = snapshot_count + gShellJobCount * 2 + 4;
    pid_t *seen = (pid_t *)calloc(seen_cap ? seen_cap : 1, sizeof(pid_t));
    size_t seen_count = 0;

    if (tree) {
        printf("%-6s %-6s %-6s %-6s %-9s %-6s %-6s %s\n",
               "PID", "PPID", "PGID", "SID", "STATE", "UTIME", "STIME", "CMD");

        size_t row_cap = snapshot_count ? snapshot_count : 1;
        VProcTreeRow *rows = (VProcTreeRow *)calloc(row_cap, sizeof(VProcTreeRow));
        size_t row_count = rows ? vprocBuildTreeRows(snapshots, snapshot_count, rows, row_cap) : 0;
        if (rows && row_count > row_cap) {
            VProcTreeRow *grown = (VProcTreeRow *)realloc(rows, row_count * sizeof(VProcTreeRow));
            if (grown) {
                rows = grown;
                row_cap = row_count;
            }
            row_count = rows ? vprocBuildTreeRows(snapshots, snapshot_count, rows, row_cap) : 0;
        }

        if (rows) {
            for (size_t r = 0; r < row_count && r < row_cap; ++r) {
                const VProcTreeRow *row = &rows[r];
                if (!row || row->snapshot_index >= snapshot_count) {
                    continue;
                }
                const VProcSnapshot *snap = &snapshots[row->snapshot_index];
                if (!snap || snap->pid <= 0) {
                    continue;
                }
                ShellJob *job = shellFindJobByPid((pid_t)snap->pid);
                pid_t pgid = (job && job->pgid > 0) ? job->pgid : snap->pid;
                const char *cmd = (job && job->command) ? job->command
                                : (snap->command[0] ? snap->command
                                : (snap->comm[0] ? snap->comm
                                : ((snap->pid == vprocGetShellSelfPid()) ? "shell" : "task")));
                const char *state = shellPsStateForEntry(job, snap);
                shellPsPrintLineTree(snap, pgid, state, cmd, row->depth);
                if (seen && seen_count < seen_cap) {
                    seen[seen_count++] = (pid_t)snap->pid;
                }
            }
            free(rows);
        }
    } else {
        printf("%-6s %-6s %-9s %-6s %-6s %s\n", "PID", "PGID", "STATE", "UTIME", "STIME", "CMD");
        for (size_t i = 0; i < snapshot_count; ++i) {
            const VProcSnapshot *snap = &snapshots[i];
            if (!snap || snap->pid <= 0) {
                continue;
            }
            ShellJob *job = shellFindJobByPid((pid_t)snap->pid);
            pid_t pgid = (job && job->pgid > 0) ? job->pgid : snap->pid;
            const char *cmd = (job && job->command) ? job->command
                            : (snap->command[0] ? snap->command
                            : (snap->comm[0] ? snap->comm
                            : ((snap->pid == vprocGetShellSelfPid()) ? "shell" : "task")));
            const char *state = shellPsStateForEntry(job, snap);
            shellPsPrintLine((pid_t)snap->pid, pgid, state, snap->rusage_utime, snap->rusage_stime, cmd);
            if (seen && seen_count < seen_cap) {
                seen[seen_count++] = (pid_t)snap->pid;
            }
        }
    }

    for (size_t i = 0; i < gShellJobCount; ++i) {
        ShellJob *job = &gShellJobs[i];
        if (!job->pids || job->pid_count == 0) {
            continue;
        }
        for (size_t j = 0; j < job->pid_count; ++j) {
            pid_t pid = job->pids[j];
            if (pid <= 0) {
                continue;
            }
            bool already_seen = false;
            for (size_t k = 0; k < seen_count; ++k) {
                if (seen[k] == pid) {
                    already_seen = true;
                    break;
                }
            }
            if (already_seen) {
                continue;
            }
            pid_t pgid = job->pgid > 0 ? job->pgid : pid;
            const char *state = job->disowned ? "disowned"
                                              : (job->stopped ? "stopped"
                                                              : (job->running ? "running" : "exited"));
            const char *cmd = job->command ? job->command : "";
            shellPsPrintLine(pid, pgid, state, 0, 0, cmd);
        }
    }

    free(snapshots);
    free(seen);
    shellUpdateStatus(0);
    return makeVoid();
}

#endif

Value vmBuiltinShellBuiltin(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || args[0].type != TYPE_STRING || !args[0].s_val || args[0].s_val[0] == '\0') {
        runtimeError(vm, "builtin: expected VM builtin name");
        shellUpdateStatus(1);
        return makeVoid();
    }

    const char *name = args[0].s_val;
    if (shellRuntimeBuiltinDisabled(name)) {
        shellReportRecoverableError(vm, false, "builtin: %s: not a shell builtin", name);
        shellUpdateStatus(1);
        return makeVoid();
    }
    VmBuiltinFn handler = getVmBuiltinHandler(name);
    if (!handler) {
        runtimeError(vm, "builtin: unknown VM builtin '%s'", name);
        shellUpdateStatus(1);
        return makeVoid();
    }

    int call_argc = arg_count - 1;
    Value *call_args = NULL;
    if (call_argc > 0) {
        call_args = (Value *)calloc((size_t)call_argc, sizeof(Value));
        if (!call_args) {
            runtimeError(vm, "builtin: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        for (int i = 0; i < call_argc; ++i) {
            Value src = args[i + 1];
            if (src.type == TYPE_STRING && src.s_val) {
                call_args[i] = shellConvertBuiltinArgument(src.s_val);
            } else if (src.type == TYPE_NIL) {
                call_args[i] = makeNil();
            } else {
                call_args[i] = makeString("");
            }
        }
    }

    unsigned long status_version = gShellStatusVersion;
    int previous_status = shellRuntimeLastStatus();
    Value result = handler(vm, call_argc, call_args);

    if (call_args) {
        for (int i = 0; i < call_argc; ++i) {
            freeValue(&call_args[i]);
        }
        free(call_args);
    }

    /*
     * Shell builtins historically report success by default, with individual
     * helpers only overriding the exit status when they hit an error.  The
     * shell runtime used to leave gShellRuntime.last_status untouched before
     * dispatching the builtin which meant a prior non-zero status would leak
     * through and make every subsequent builtin appear to fail.  Scripts such
     * as the threaded Sierpinski demo rely on checking the builtin exit code,
     * so we normalise the status to success afterwards when the handler didn't
     * touch it.
     */
    bool status_untouched = (gShellStatusVersion == status_version);
    int status = shellRuntimeLastStatus();
    if (vm && vm->abort_requested && (status_untouched || status == previous_status)) {
        status = 1;
        shellUpdateStatus(1);
    } else if (status_untouched && status != 0) {
        status = 0;
        shellUpdateStatus(0);
    }

    if (status == 0 && result.type != TYPE_VOID) {
        printValueToStream(result, stdout);
        fputc('\n', stdout);
    }

    freeValue(&result);
    return makeVoid();
}

static void shellLoopFreeWordList(char **words, size_t count) {
    if (!words) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(words[i]);
    }
    free(words);
}

static bool shellLoopAppendWord(char ***list, size_t *count, size_t *capacity, const char *value) {
    if (!list || !count || !capacity) {
        return false;
    }
    if (*count == *capacity) {
        size_t new_capacity = (*capacity == 0) ? 4 : (*capacity * 2);
        char **resized = (char **)realloc(*list, new_capacity * sizeof(char *));
        if (!resized) {
            return false;
        }
        *list = resized;
        *capacity = new_capacity;
    }
    char *copy = strdup(value ? value : "");
    if (!copy) {
        return false;
    }
    (*list)[*count] = copy;
    (*count)++;
    return true;
}

static bool shellLoopExpandEncodedWords(const char *const *encoded_words,
                                        size_t encoded_count,
                                        char ***out_words,
                                        size_t *out_count) {
    if (out_words) {
        *out_words = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    char **words = NULL;
    size_t count = 0;
    size_t capacity = 0;

    for (size_t i = 0; i < encoded_count; ++i) {
        const char *spec = encoded_words[i] ? encoded_words[i] : "";
        const char *text = spec;
        const char *word_meta = NULL;
        size_t word_meta_len = 0;
        uint8_t word_flags = 0;
        if (!shellDecodeWordSpec(spec, &text, &word_flags, &word_meta, &word_meta_len)) {
            text = spec;
            word_flags = 0;
            word_meta = NULL;
            word_meta_len = 0;
        }
        bool *quoted_map = NULL;
        size_t quoted_len = 0;
        bool zero_array = false;
        char *expanded = shellExpandWord(text,
                                         word_flags,
                                         word_meta,
                                         word_meta_len,
                                         &quoted_map,
                                         &quoted_len,
                                         &zero_array);
        if (!expanded) {
            free(quoted_map);
            shellLoopFreeWordList(words, count);
            return false;
        }

        char **fields = NULL;
        size_t field_count = 0;
        bool split_ok = shellSplitExpandedWord(expanded,
                                               word_flags,
                                               quoted_map,
                                               quoted_len,
                                               zero_array,
                                               &fields,
                                               &field_count);
        free(expanded);
        free(quoted_map);
        if (!split_ok) {
            shellFreeStringArray(fields, field_count);
            shellLoopFreeWordList(words, count);
            return false;
        }

        for (size_t f = 0; f < field_count; ++f) {
            if (!shellLoopAppendWord(&words, &count, &capacity, fields[f])) {
                shellFreeStringArray(fields, field_count);
                shellLoopFreeWordList(words, count);
                return false;
            }
        }
        shellFreeStringArray(fields, field_count);
    }

    if (out_words) {
        *out_words = words;
    } else {
        shellLoopFreeWordList(words, count);
    }
    if (out_count) {
        *out_count = out_words ? count : 0;
    }
    return true;
}

static int shellLoopEvaluateTestWordsEncoded(char **encoded_words,
                                             size_t word_count,
                                             bool is_bracket) {
    char **words = NULL;
    size_t expanded = 0;
    int status = 1;
    if (encoded_words &&
        shellLoopExpandEncodedWords((const char *const *)encoded_words,
                                    word_count,
                                    &words,
                                    &expanded) &&
        expanded >= 1) {
        const char **argv = (const char **)((expanded > 1) ? &words[1] : NULL);
        int argc = (int)((expanded > 1) ? (expanded - 1) : 0);
        bool result = false;
        if (shellBuiltinTestFastEval(argv, argc, is_bracket, &result)) {
            status = result ? 0 : 1;
        }
    }
    shellLoopFreeWordList(words, expanded);
    return status;
}

static int shellLoopEvaluateArithmeticExpression(const char *expression) {
    int arith_status = 1;
    if (expression && shellBuiltinArithmeticFastEval(expression, &arith_status)) {
        return arith_status;
    }
    return arith_status;
}

static bool shellLoopEvaluateConditionSimple(ShellLoopFrame *frame, int *status_out) {
    int status = 1;
    switch (frame->cond_kind) {
        case SHELL_LOOP_CONDITION_TEST:
        case SHELL_LOOP_CONDITION_BRACKET:
            status = shellLoopEvaluateTestWordsEncoded(frame->cond_words,
                                                       frame->cond_word_count,
                                                       frame->cond_kind == SHELL_LOOP_CONDITION_BRACKET);
            break;
        case SHELL_LOOP_CONDITION_COLON:
        case SHELL_LOOP_CONDITION_TRUE:
            status = 0;
            break;
        case SHELL_LOOP_CONDITION_FALSE:
            status = 1;
            break;
        case SHELL_LOOP_CONDITION_ARITH:
            status = shellLoopEvaluateArithmeticExpression(frame->cond_arith_expression);
            break;
        case SHELL_LOOP_CONDITION_NONE:
        default:
            status = 1;
            break;
    }
    if (status_out) {
        *status_out = status;
    }
    bool ready = (status == 0);
    if (frame->kind == SHELL_LOOP_KIND_UNTIL) {
        ready = !ready;
    }
    return ready;
}

static int shellLoopExecuteBodySimple(ShellLoopFrame *frame) {
    switch (frame->body_kind) {
        case SHELL_LOOP_BODY_KIND_NONE:
        case SHELL_LOOP_BODY_KIND_COLON:
        case SHELL_LOOP_BODY_KIND_TRUE:
            return 0;
        case SHELL_LOOP_BODY_KIND_FALSE:
            return 1;
        case SHELL_LOOP_BODY_KIND_TEST:
        case SHELL_LOOP_BODY_KIND_BRACKET:
            return shellLoopEvaluateTestWordsEncoded(frame->body_words,
                                                     frame->body_word_count,
                                                     frame->body_kind == SHELL_LOOP_BODY_KIND_BRACKET);
        case SHELL_LOOP_BODY_KIND_ARITH:
            return shellLoopEvaluateArithmeticExpression(frame->body_arith_expression);
        case SHELL_LOOP_BODY_KIND_TEST_ARITH:
        case SHELL_LOOP_BODY_KIND_BRACKET_ARITH:
            (void)shellLoopEvaluateTestWordsEncoded(frame->body_words,
                                                    frame->body_word_count,
                                                    frame->body_kind == SHELL_LOOP_BODY_KIND_BRACKET_ARITH);
            return shellLoopEvaluateArithmeticExpression(frame->body_arith_expression);
        default:
            return 0;
    }
}

Value vmHostShellLoopCheckCondition(VM *vm) {
    (void)vm;
    shellRuntimeProcessPendingSignals();
    ShellLoopFrame *frame = shellLoopTop();
    if (!frame) {
        shellRuntimeEnterCondition();
        shellRuntimeLeaveCondition();
        shellUpdateStatus(0);
        return makeBoolean(true);
    }

    shellRuntimeEnterCondition();

    int status = 1;
    bool ready = shellLoopEvaluateConditionSimple(frame, &status);

    shellUpdateStatus(status);

    shellRuntimeLeaveCondition();
    shellUpdateStatus(0);

    return makeBoolean(ready);
}

Value vmHostShellLoopExecuteBody(VM *vm) {
    (void)vm;
    shellRuntimeProcessPendingSignals();
    ShellLoopFrame *frame = shellLoopTop();
    if (!frame) {
        shellUpdateStatus(0);
        return makeBoolean(true);
    }

    int status = shellLoopExecuteBodySimple(frame);
    shellUpdateStatus(status);
    Value advance = vmHostShellLoopAdvance(vm);
    return advance;
}

Value vmHostShellLoopCheckBody(VM *vm) {
    (void)vm;
    shellRuntimeProcessPendingSignals();
    ShellLoopFrame *frame = shellLoopTop();
    if (!frame) {
        shellRuntimeEnterCondition();
        shellRuntimeLeaveCondition();
        shellUpdateStatus(0);
        return makeBoolean(true);
    }

    shellRuntimeEnterCondition();
    int cond_status = 1;
    bool ready = shellLoopEvaluateConditionSimple(frame, &cond_status);
    shellUpdateStatus(cond_status);
    shellRuntimeLeaveCondition();
    shellUpdateStatus(0);

    if (!ready) {
        return makeBoolean(false);
    }

    int body_status = shellLoopExecuteBodySimple(frame);
    shellUpdateStatus(body_status);
    Value advance = vmHostShellLoopAdvance(vm);
    return advance;
}

Value vmHostShellLastStatus(VM *vm) {
    (void)vm;
    if (gShellRuntime.errexit_pending) {
        gShellRuntime.errexit_pending = false;
        gShellRuntime.errexit_defer_active = false;
        shellRuntimeConsumeExitRequested();
        if (gShellCurrentVm) {
            gShellCurrentVm->exit_requested = false;
            gShellCurrentVm->abort_requested = false;
            gShellCurrentVm->current_builtin_name = NULL;
        }
    }
    return makeInt(gShellRuntime.last_status);
}

Value vmHostShellLoopIsReady(VM *vm) {
    (void)vm;
    shellRuntimeProcessPendingSignals();
    ShellLoopFrame *frame = shellLoopTop();
    if ((gShellExitRequested || gShellRuntime.break_requested) && frame) {
        frame->break_pending = true;
        frame->continue_pending = false;
        frame->skip_body = true;
        gShellRuntime.break_requested = true;
        gShellRuntime.break_requested_levels = (int)gShellLoopStackSize;
        return makeBoolean(false);
    }
    bool ready = false;
    if (frame) {
        if (frame->break_pending) {
            ready = false;
        } else if (frame->kind == SHELL_LOOP_KIND_FOR) {
            ready = frame->for_active && !frame->skip_body;
        } else if (frame->kind == SHELL_LOOP_KIND_CFOR) {
            bool condition_ready = false;
            if (!shellLoopEvaluateCForCondition(frame, &condition_ready)) {
                frame->skip_body = true;
                frame->break_pending = true;
                shellUpdateStatus(1);
                ready = false;
            } else {
                ready = condition_ready && !frame->skip_body;
            }
        } else {
            ready = !frame->skip_body;
        }
    }
    return makeBoolean(ready);
}

Value vmHostShellLoopAdvance(VM *vm) {
    (void)vm;
    shellRuntimeProcessPendingSignals();
    ShellLoopFrame *frame = shellLoopTop();
    if (!frame) {
        return makeBoolean(false);
    }

    if (gShellExitRequested || gShellRuntime.break_requested) {
        frame->break_pending = true;
    }
    if (frame->break_pending) {
        frame->break_pending = false;
        frame->continue_pending = false;
        frame->skip_body = false;
        frame->for_active = false;
        shellMaybeResetPipeline();
        return makeBoolean(false);
    }

    if (frame->continue_pending) {
        frame->continue_pending = false;
    }

    bool should_continue = true;
    if (frame->kind == SHELL_LOOP_KIND_FOR) {
        if (frame->for_index < frame->for_count) {
            if (!shellAssignLoopVariable(frame->for_variable, frame->for_values[frame->for_index])) {
                runtimeError(vm, "shell loop: failed to assign '%s'", frame->for_variable ? frame->for_variable : "<var>");
                shellUpdateStatus(1);
                frame->skip_body = false;
                frame->for_active = false;
                shellMaybeResetPipeline();
                return makeBoolean(false);
            }
            frame->for_index++;
            frame->for_active = true;
            should_continue = true;
        } else {
            frame->for_active = false;
            should_continue = false;
        }
    } else if (frame->kind == SHELL_LOOP_KIND_CFOR) {
        if (!shellLoopExecuteCForUpdate(frame)) {
            shellUpdateStatus(1);
            frame->skip_body = false;
            frame->break_pending = true;
            shellMaybeResetPipeline();
            return makeBoolean(false);
        }
        bool condition_ready = false;
        if (!shellLoopEvaluateCForCondition(frame, &condition_ready)) {
            shellUpdateStatus(1);
            frame->skip_body = false;
            frame->break_pending = true;
            shellMaybeResetPipeline();
            return makeBoolean(false);
        }
        should_continue = condition_ready;
    }

    frame->skip_body = false;
    shellMaybeResetPipeline();
    if (!should_continue) {
        shellUpdateStatus(0);
    }
    return makeBoolean(should_continue);
}

Value vmHostShellPollJobs(VM *vm) {
    (void)vm;
    return makeInt(shellCollectJobs());
}

bool shellRuntimeConsumeExitRequested(void) {
    bool requested = gShellExitRequested;
    gShellExitRequested = false;
    return requested;
}

int shellRuntimeLastStatus(void) {
    return gShellRuntime.last_status;
}
