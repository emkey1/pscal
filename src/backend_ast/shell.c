#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "vm/vm.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

typedef struct {
    int fd;
    int flags;
    mode_t mode;
    char *path;
} ShellRedirection;

typedef struct {
    char **argv;
    size_t argc;
    ShellRedirection *redirs;
    size_t redir_count;
    bool background;
    int pipeline_index;
    bool is_pipeline_head;
    bool is_pipeline_tail;
} ShellCommand;

typedef struct {
    bool active;
    size_t stage_count;
    bool negated;
    pid_t *pids;
    int (*pipes)[2];
    size_t launched;
    bool background;
    int last_status;
} ShellPipelineContext;

typedef struct {
    int last_status;
    ShellPipelineContext pipeline;
} ShellRuntimeState;

static ShellRuntimeState gShellRuntime = {
    .last_status = 0,
    .pipeline = {0}
};

static bool gShellExitRequested = false;

typedef struct {
    pid_t pid;
    char *command;
} ShellJob;

static ShellJob *gShellJobs = NULL;
static size_t gShellJobCount = 0;

static void shellFreeRedirections(ShellCommand *cmd) {
    if (!cmd) {
        return;
    }
    for (size_t i = 0; i < cmd->redir_count; ++i) {
        free(cmd->redirs[i].path);
    }
    free(cmd->redirs);
    cmd->redirs = NULL;
    cmd->redir_count = 0;
}

static void shellFreeCommand(ShellCommand *cmd) {
    if (!cmd) {
        return;
    }
    for (size_t i = 0; i < cmd->argc; ++i) {
        free(cmd->argv[i]);
    }
    free(cmd->argv);
    cmd->argv = NULL;
    cmd->argc = 0;
    shellFreeRedirections(cmd);
}

static bool shellParseBool(const char *value, bool *out_flag) {
    if (!value || !out_flag) {
        return false;
    }
    if (strcasecmp(value, "1") == 0 || strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0) {
        *out_flag = true;
        return true;
    }
    if (strcasecmp(value, "0") == 0 || strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0) {
        *out_flag = false;
        return true;
    }
    return false;
}

static void shellUpdateStatus(int status) {
    gShellRuntime.last_status = status;
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", status);
    setenv("PSCALSHELL_LAST_STATUS", buffer, 1);
}

static bool shellIsRuntimeBuiltin(const char *name) {
    if (!name || !*name) {
        return false;
    }
    static const char *kBuiltins[] = {"cd", "pwd", "exit", "export", "unset", "alias"};
    size_t count = sizeof(kBuiltins) / sizeof(kBuiltins[0]);
    for (size_t i = 0; i < count; ++i) {
        if (strcasecmp(name, kBuiltins[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool shellInvokeBuiltin(VM *vm, ShellCommand *cmd) {
    if (!cmd || cmd->argc == 0) {
        return false;
    }
    const char *name = cmd->argv[0];
    if (!name || !shellIsRuntimeBuiltin(name)) {
        return false;
    }
    VmBuiltinFn handler = getVmBuiltinHandler(name);
    if (!handler) {
        return false;
    }
    int arg_count = (cmd->argc > 0) ? (int)cmd->argc - 1 : 0;
    Value *args = NULL;
    if (arg_count > 0) {
        args = calloc((size_t)arg_count, sizeof(Value));
        if (!args) {
            runtimeError(vm, "shell builtin '%s': out of memory", name);
            shellUpdateStatus(1);
            return true;
        }
        for (int i = 0; i < arg_count; ++i) {
            args[i] = makeString(cmd->argv[i + 1]);
        }
    }
    handler(vm, arg_count, args);
    if (args) {
        for (int i = 0; i < arg_count; ++i) {
            freeValue(&args[i]);
        }
        free(args);
    }
    return true;
}

static void shellFreeJob(ShellJob *job) {
    if (!job) {
        return;
    }
    free(job->command);
    job->command = NULL;
    job->pid = -1;
}

static void shellRegisterJob(pid_t pid, const ShellCommand *cmd) {
    if (pid <= 0 || !cmd) {
        return;
    }
    ShellJob *new_jobs = realloc(gShellJobs, sizeof(ShellJob) * (gShellJobCount + 1));
    if (!new_jobs) {
        return;
    }
    gShellJobs = new_jobs;
    ShellJob *job = &gShellJobs[gShellJobCount++];
    job->pid = pid;
    size_t len = 0;
    for (size_t i = 0; i < cmd->argc; ++i) {
        len += strlen(cmd->argv[i]) + 1;
    }
    char *summary = malloc(len + 1);
    if (!summary) {
        job->command = NULL;
        return;
    }
    summary[0] = '\0';
    for (size_t i = 0; i < cmd->argc; ++i) {
        strcat(summary, cmd->argv[i]);
        if (i + 1 < cmd->argc) {
            strcat(summary, " ");
        }
    }
    job->command = summary;
}

static int shellCollectJobs(void) {
    int reaped = 0;
    for (size_t i = 0; i < gShellJobCount;) {
        ShellJob *job = &gShellJobs[i];
        int status = 0;
        pid_t res = waitpid(job->pid, &status, WNOHANG);
        if (res == 0) {
            ++i;
            continue;
        }
        if (res < 0) {
            ++i;
            continue;
        }
        int exit_status = 0;
        if (WIFEXITED(status)) {
            exit_status = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            exit_status = 128 + WTERMSIG(status);
        }
        shellUpdateStatus(exit_status);
        shellFreeJob(job);
        if (i + 1 < gShellJobCount) {
            gShellJobs[i] = gShellJobs[gShellJobCount - 1];
        }
        gShellJobCount--;
        reaped++;
    }
    if (gShellJobCount == 0 && gShellJobs) {
        free(gShellJobs);
        gShellJobs = NULL;
    }
    return reaped;
}

static void shellParseMetadata(const char *meta, ShellCommand *cmd) {
    if (!meta || !cmd) {
        return;
    }
    char *copy = strdup(meta);
    if (!copy) {
        return;
    }
    char *cursor = copy;
    while (cursor && *cursor) {
        char *next = strchr(cursor, ';');
        if (next) {
            *next = '\0';
        }
        char *eq = strchr(cursor, '=');
        if (eq) {
            *eq = '\0';
            const char *key = cursor;
            const char *value = eq + 1;
            if (strcmp(key, "bg") == 0) {
                shellParseBool(value, &cmd->background);
            } else if (strcmp(key, "pipe") == 0) {
                cmd->pipeline_index = atoi(value);
            } else if (strcmp(key, "head") == 0) {
                shellParseBool(value, &cmd->is_pipeline_head);
            } else if (strcmp(key, "tail") == 0) {
                shellParseBool(value, &cmd->is_pipeline_tail);
            }
        }
        if (!next) {
            break;
        }
        cursor = next + 1;
    }
    free(copy);
}

static bool shellAddArg(ShellCommand *cmd, const char *arg) {
    if (!cmd || !arg) {
        return false;
    }
    char **new_argv = realloc(cmd->argv, sizeof(char*) * (cmd->argc + 2));
    if (!new_argv) {
        return false;
    }
    cmd->argv = new_argv;
    cmd->argv[cmd->argc] = strdup(arg);
    if (!cmd->argv[cmd->argc]) {
        return false;
    }
    cmd->argc++;
    cmd->argv[cmd->argc] = NULL;
    return true;
}

static bool shellAddRedirection(ShellCommand *cmd, const char *spec) {
    if (!cmd || !spec) {
        return false;
    }
    const char *payload = spec + strlen("redir:");
    char *copy = strdup(payload);
    if (!copy) {
        return false;
    }
    char *parts[3] = {NULL, NULL, NULL};
    size_t part_index = 0;
    char *cursor = copy;
    while (cursor && part_index < 3) {
        char *next = strchr(cursor, ':');
        if (next) {
            *next = '\0';
        }
        parts[part_index++] = cursor;
        if (!next) {
            break;
        }
        cursor = next + 1;
    }
    if (part_index < 3) {
        free(copy);
        return false;
    }
    const char *fd_str = parts[0];
    const char *type = parts[1];
    const char *target = parts[2];
    int fd = -1;
    if (fd_str && *fd_str) {
        fd = atoi(fd_str);
    } else if (strcmp(type, "<") == 0) {
        fd = STDIN_FILENO;
    } else {
        fd = STDOUT_FILENO;
    }
    int flags = 0;
    mode_t mode = 0;
    if (strcmp(type, "<") == 0) {
        flags = O_RDONLY;
    } else if (strcmp(type, ">") == 0) {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        mode = 0666;
    } else if (strcmp(type, ">>") == 0) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        mode = 0666;
    } else {
        free(copy);
        return false;
    }
    ShellRedirection *new_redirs = realloc(cmd->redirs, sizeof(ShellRedirection) * (cmd->redir_count + 1));
    if (!new_redirs) {
        free(copy);
        return false;
    }
    cmd->redirs = new_redirs;
    ShellRedirection *redir = &cmd->redirs[cmd->redir_count++];
    redir->fd = fd;
    redir->flags = flags;
    redir->mode = mode;
    redir->path = strdup(target ? target : "");
    free(copy);
    if (!redir->path) {
        cmd->redir_count--;
        return false;
    }
    return true;
}

static bool shellBuildCommand(VM *vm, int arg_count, Value *args, ShellCommand *out_cmd) {
    if (!out_cmd) {
        return false;
    }
    memset(out_cmd, 0, sizeof(*out_cmd));
    if (arg_count <= 0) {
        runtimeError(vm, "shell exec: missing metadata argument");
        return false;
    }
    Value meta = args[0];
    if (meta.type != TYPE_STRING || !meta.s_val) {
        runtimeError(vm, "shell exec: metadata must be a string");
        return false;
    }
    shellParseMetadata(meta.s_val, out_cmd);
    for (int i = 1; i < arg_count; ++i) {
        Value v = args[i];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "shell exec: arguments must be strings");
            shellFreeCommand(out_cmd);
            return false;
        }
        if (strncmp(v.s_val, "redir:", 6) == 0) {
            if (!shellAddRedirection(out_cmd, v.s_val)) {
                runtimeError(vm, "shell exec: invalid redirection '%s'", v.s_val);
                shellFreeCommand(out_cmd);
                return false;
            }
        } else {
            if (!shellAddArg(out_cmd, v.s_val)) {
                runtimeError(vm, "shell exec: unable to add argument");
                shellFreeCommand(out_cmd);
                return false;
            }
        }
    }
    return true;
}

static int shellSpawnProcess(const ShellCommand *cmd, int stdin_fd, int stdout_fd, pid_t *child_pid) {
    if (!cmd || cmd->argc == 0 || !cmd->argv || !cmd->argv[0]) {
        return EINVAL;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    if (stdin_fd >= 0) {
        posix_spawn_file_actions_adddup2(&actions, stdin_fd, STDIN_FILENO);
        posix_spawn_file_actions_addclose(&actions, stdin_fd);
    }
    if (stdout_fd >= 0) {
        posix_spawn_file_actions_adddup2(&actions, stdout_fd, STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, stdout_fd);
    }

    int opened_fds[16];
    size_t opened_count = 0;

    for (size_t i = 0; i < cmd->redir_count; ++i) {
        const ShellRedirection *redir = &cmd->redirs[i];
        int fd = open(redir->path, redir->flags, redir->mode);
        if (fd < 0) {
            for (size_t j = 0; j < opened_count; ++j) {
                close(opened_fds[j]);
            }
            posix_spawn_file_actions_destroy(&actions);
            return errno;
        }
        if (opened_count < sizeof(opened_fds) / sizeof(opened_fds[0])) {
            opened_fds[opened_count++] = fd;
        }
        posix_spawn_file_actions_adddup2(&actions, fd, redir->fd);
        posix_spawn_file_actions_addclose(&actions, fd);
    }

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);

    int result = posix_spawnp(child_pid, cmd->argv[0], &actions, &attr, cmd->argv, environ);

    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);

    for (size_t j = 0; j < opened_count; ++j) {
        close(opened_fds[j]);
    }

    return result;
}

static int shellWaitPid(pid_t pid, int *status_out) {
    int status = 0;
    pid_t waited = waitpid(pid, &status, 0);
    if (waited < 0) {
        return errno;
    }
    if (status_out) {
        if (WIFEXITED(status)) {
            *status_out = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            *status_out = 128 + WTERMSIG(status);
        } else {
            *status_out = status;
        }
    }
    return 0;
}

static void shellResetPipeline(void) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (!ctx->active) {
        return;
    }
    if (ctx->pipes) {
        size_t pipe_count = (ctx->stage_count > 0) ? (ctx->stage_count - 1) : 0;
        for (size_t i = 0; i < pipe_count; ++i) {
            if (ctx->pipes[i][0] >= 0) close(ctx->pipes[i][0]);
            if (ctx->pipes[i][1] >= 0) close(ctx->pipes[i][1]);
        }
        free(ctx->pipes);
        ctx->pipes = NULL;
    }
    free(ctx->pids);
    ctx->pids = NULL;
    ctx->active = false;
    ctx->stage_count = 0;
    ctx->launched = 0;
    ctx->background = false;
    ctx->last_status = 0;
}

static void shellAbortPipeline(void) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (!ctx->active) {
        return;
    }

    if (ctx->pipes) {
        size_t pipe_count = (ctx->stage_count > 0) ? (ctx->stage_count - 1) : 0;
        for (size_t i = 0; i < pipe_count; ++i) {
            if (ctx->pipes[i][0] >= 0) {
                close(ctx->pipes[i][0]);
                ctx->pipes[i][0] = -1;
            }
            if (ctx->pipes[i][1] >= 0) {
                close(ctx->pipes[i][1]);
                ctx->pipes[i][1] = -1;
            }
        }
    }

    for (size_t i = 0; i < ctx->launched; ++i) {
        pid_t pid = ctx->pids[i];
        if (pid <= 0) {
            continue;
        }
        int status = 0;
        pid_t res = -1;
        do {
            res = waitpid(pid, &status, WNOHANG);
        } while (res < 0 && errno == EINTR);
        if (res == 0) {
            kill(pid, SIGTERM);
            do {
                res = waitpid(pid, &status, 0);
            } while (res < 0 && errno == EINTR);
        }
    }

    shellResetPipeline();
}

static bool shellEnsurePipeline(size_t stages, bool negated) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    shellResetPipeline();
    ctx->stage_count = stages;
    ctx->negated = negated;
    ctx->active = true;
    ctx->launched = 0;
    ctx->last_status = 0;
    ctx->background = false;
    ctx->pids = calloc(stages, sizeof(pid_t));
    if (!ctx->pids) {
        shellResetPipeline();
        return false;
    }
    if (stages > 1) {
        ctx->pipes = calloc(stages - 1, sizeof(int[2]));
        if (!ctx->pipes) {
            shellResetPipeline();
            return false;
        }
        for (size_t i = 0; i < stages - 1; ++i) {
            ctx->pipes[i][0] = -1;
            ctx->pipes[i][1] = -1;
            if (pipe(ctx->pipes[i]) != 0) {
                shellResetPipeline();
                return false;
            }
        }
    }
    return true;
}

static int shellFinishPipeline(const ShellCommand *tail_cmd) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (!ctx->active) {
        return gShellRuntime.last_status;
    }
    int final_status = ctx->last_status;
    if (!ctx->background) {
        for (size_t i = 0; i < ctx->launched; ++i) {
            int status = 0;
            shellWaitPid(ctx->pids[i], &status);
            if (i + 1 == ctx->launched) {
                final_status = status;
            }
        }
    } else if (ctx->launched > 0) {
        shellRegisterJob(ctx->pids[ctx->launched - 1], tail_cmd);
        final_status = 0;
    }
    if (ctx->negated) {
        final_status = (final_status == 0) ? 1 : 0;
    }
    ctx->last_status = final_status;
    shellResetPipeline();
    shellUpdateStatus(final_status);
    return final_status;
}

static Value shellExecuteCommand(VM *vm, ShellCommand *cmd) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    int stdin_fd = -1;
    int stdout_fd = -1;
    if (ctx->active) {
        if (ctx->stage_count == 1 && shellInvokeBuiltin(vm, cmd)) {
            ctx->last_status = gShellRuntime.last_status;
            shellResetPipeline();
            shellFreeCommand(cmd);
            return makeVoid();
        }
        size_t idx = (size_t)cmd->pipeline_index;
        if (idx >= ctx->stage_count) {
            runtimeError(vm, "shell exec: pipeline index out of range");
            shellFreeCommand(cmd);
            shellResetPipeline();
            return makeVoid();
        }
        if (ctx->stage_count > 1) {
            if (!cmd->is_pipeline_head) {
                stdin_fd = ctx->pipes[idx - 1][0];
            }
            if (!cmd->is_pipeline_tail) {
                stdout_fd = ctx->pipes[idx][1];
            }
        }
    } else {
        if (shellInvokeBuiltin(vm, cmd)) {
            shellFreeCommand(cmd);
            return makeVoid();
        }
    }

    pid_t child = -1;
    int spawn_err = shellSpawnProcess(cmd, stdin_fd, stdout_fd, &child);
    if (spawn_err != 0) {
        runtimeError(vm, "shell exec: failed to spawn '%s': %s", cmd->argv[0], strerror(spawn_err));
        if (ctx->active) {
            shellAbortPipeline();
        }
        shellFreeCommand(cmd);
        shellUpdateStatus(127);
        return makeVoid();
    }

    if (ctx->active) {
        if (!cmd->is_pipeline_head && stdin_fd >= 0) {
            close(stdin_fd);
            if (cmd->pipeline_index > 0) {
                ctx->pipes[cmd->pipeline_index - 1][0] = -1;
            }
        }
        if (!cmd->is_pipeline_tail && stdout_fd >= 0) {
            close(stdout_fd);
            ctx->pipes[cmd->pipeline_index][1] = -1;
        }
        ctx->pids[ctx->launched++] = child;
        if (cmd->is_pipeline_tail) {
            ctx->background = cmd->background;
            shellFinishPipeline(cmd);
        }
    } else {
        int status = 0;
        if (!cmd->background) {
            shellWaitPid(child, &status);
        } else {
            shellRegisterJob(child, cmd);
        }
        shellUpdateStatus(status);
    }

    shellFreeCommand(cmd);
    return makeVoid();
}

Value vmBuiltinShellExec(VM *vm, int arg_count, Value *args) {
    shellCollectJobs();
    ShellCommand cmd;
    if (!shellBuildCommand(vm, arg_count, args, &cmd)) {
        return makeVoid();
    }
    return shellExecuteCommand(vm, &cmd);
}

Value vmBuiltinShellPipeline(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1 || args[0].type != TYPE_STRING || !args[0].s_val) {
        runtimeError(vm, "shell pipeline: expected metadata string");
        return makeVoid();
    }
    const char *meta = args[0].s_val;
    size_t stages = 0;
    bool negated = false;
    char *copy = strdup(meta);
    if (!copy) {
        runtimeError(vm, "shell pipeline: out of memory");
        return makeVoid();
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
            }
        }
        if (!next) break;
        cursor = next + 1;
    }
    free(copy);
    if (stages == 0) {
        runtimeError(vm, "shell pipeline: invalid stage count");
        return makeVoid();
    }
    if (!shellEnsurePipeline(stages, negated)) {
        runtimeError(vm, "shell pipeline: unable to allocate context");
    }
    return makeVoid();
}

Value vmBuiltinShellAnd(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    if (gShellRuntime.last_status != 0) {
        shellUpdateStatus(gShellRuntime.last_status);
    }
    return makeVoid();
}

Value vmBuiltinShellOr(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    if (gShellRuntime.last_status == 0) {
        shellUpdateStatus(0);
    }
    return makeVoid();
}

Value vmBuiltinShellSubshell(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    shellResetPipeline();
    return makeVoid();
}

Value vmBuiltinShellLoop(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    shellResetPipeline();
    return makeVoid();
}

Value vmBuiltinShellIf(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    return makeVoid();
}

Value vmBuiltinShellCd(VM *vm, int arg_count, Value *args) {
    const char *path = NULL;
    if (arg_count == 0) {
        path = getenv("HOME");
        if (!path) {
            runtimeError(vm, "cd: HOME not set");
            shellUpdateStatus(1);
            return makeVoid();
        }
    } else if (args[0].type == TYPE_STRING && args[0].s_val) {
        path = args[0].s_val;
    } else {
        runtimeError(vm, "cd: expected directory path");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (chdir(path) != 0) {
        runtimeError(vm, "cd: %s", strerror(errno));
        shellUpdateStatus(errno ? errno : 1);
        return makeVoid();
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        setenv("PWD", cwd, 1);
    }
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
    printf("%s\n", cwd);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellExit(VM *vm, int arg_count, Value *args) {
    int code = 0;
    if (arg_count >= 1 && IS_INTLIKE(args[0])) {
        code = (int)AS_INTEGER(args[0]);
    }
    shellUpdateStatus(code);
    gShellExitRequested = true;
    vm->exit_requested = true;
    vm->current_builtin_name = "exit";
    return makeVoid();
}

Value vmBuiltinShellExport(VM *vm, int arg_count, Value *args) {
    for (int i = 0; i < arg_count; ++i) {
        if (args[i].type != TYPE_STRING || !args[i].s_val) {
            runtimeError(vm, "export: expected name=value string");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *assignment = args[i].s_val;
        const char *eq = strchr(assignment, '=');
        if (!eq || eq == assignment) {
            runtimeError(vm, "export: invalid assignment '%s'", assignment);
            shellUpdateStatus(1);
            return makeVoid();
        }
        size_t name_len = (size_t)(eq - assignment);
        char *name = strndup(assignment, name_len);
        const char *value = eq + 1;
        if (!name) {
            runtimeError(vm, "export: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        setenv(name, value, 1);
        free(name);
    }
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellUnset(VM *vm, int arg_count, Value *args) {
    for (int i = 0; i < arg_count; ++i) {
        if (args[i].type != TYPE_STRING || !args[i].s_val) {
            runtimeError(vm, "unset: expected variable name");
            shellUpdateStatus(1);
            return makeVoid();
        }
        unsetenv(args[i].s_val);
    }
    shellUpdateStatus(0);
    return makeVoid();
}

typedef struct {
    char *name;
    char *value;
} ShellAlias;

static ShellAlias *gShellAliases = NULL;
static size_t gShellAliasCount = 0;

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

Value vmHostShellLastStatus(VM *vm) {
    (void)vm;
    return makeInt(gShellRuntime.last_status);
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
