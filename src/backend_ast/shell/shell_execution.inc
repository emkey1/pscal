/* Auto-generated include: command execution and pipeline management. Included from shell.c. */

#if defined(PSCAL_TARGET_IOS)
#include "ios/vproc.h"
#include "smallclue/smallclue.h"
static int gShellPagerVirtualTTYFd = -1;

static void shellPagerPushVirtualTTYOverride(int fd) {
    if (gShellPagerVirtualTTYFd >= 0) {
        close(gShellPagerVirtualTTYFd);
    }
    gShellPagerVirtualTTYFd = fd;
}

static void shellPagerPopVirtualTTYOverride(void) {
    if (gShellPagerVirtualTTYFd >= 0) {
        close(gShellPagerVirtualTTYFd);
        gShellPagerVirtualTTYFd = -1;
    }
}

static int shellPagerGetVirtualTTYOverride(void) {
    return gShellPagerVirtualTTYFd;
}

static VProc *shellCreateVProcForStage(int stdin_host,
                                       int stdout_host,
                                       int stderr_host,
                                       int cols,
                                       int rows,
                                       int pid_hint) {
    VProcOptions opts = vprocDefaultOptions();
    opts.stdin_fd = (stdin_host >= 0) ? stdin_host : -2;   /* default to /dev/null */
    opts.stdout_fd = (stdout_host >= 0) ? stdout_host : -1;
    opts.stderr_fd = (stderr_host >= 0) ? stderr_host : -1;
    opts.winsize_cols = cols;
    opts.winsize_rows = rows;
    opts.pid_hint = pid_hint;
    return vprocCreate(&opts);
}
#endif

static int shellTryRunScriptFallback(VM *vm, ShellCommand *cmd) {
    if (!cmd || cmd->argc <= 0 || !cmd->argv || !cmd->argv[0]) {
        return -1;
    }
    const char *path = cmd->argv[0];
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return -1;
    }
    if (access(path, R_OK) != 0) {
        return -1;
    }

    char *source = shellLoadFile(path);
    if (!source) {
        return -1;
    }

    char **saved_params = gParamValues;
    int saved_count = gParamCount;
    bool saved_owned = gShellPositionalOwned;

    bool replaced_params = false;
    char **new_params = NULL;
    int new_count = (cmd->argc > 1) ? (cmd->argc - 1) : 0;
    if (new_count > 0) {
        new_params = (char **)calloc((size_t)new_count, sizeof(char *));
        if (!new_params) {
            free(source);
            return -1;
        }
        for (int i = 0; i < new_count; ++i) {
            new_params[i] = strdup(cmd->argv[i + 1] ? cmd->argv[i + 1] : "");
            if (!new_params[i]) {
                shellFreeParameterArray(new_params, i);
                free(source);
                return -1;
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
    opts.exit_on_signal = shellRuntimeExitOnSignal();
    opts.suppress_warnings = true;
    const char *frontend_path = shellRuntimeGetArg0();
    opts.frontend_path = frontend_path ? frontend_path : "exsh";

    shellRuntimeSetArg0(path);
    bool exit_requested = false;
    int status = shellRunSource(source, path, &opts, &exit_requested);
    free(source);
    shellRuntimeSetArg0(frontend_path);

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
    return status;
}

static bool shellAppendCombinedRedirect(ShellCommand *cmd, const char *path, bool append) {
    if (!cmd || !path || !*path) {
        return false;
    }
    ShellRedirection redir;
    memset(&redir, 0, sizeof(redir));
    redir.fd = STDOUT_FILENO;
    redir.kind = SHELL_RUNTIME_REDIR_OPEN;
    redir.flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    redir.mode = 0666;
    redir.path = strdup(path);
    if (!redir.path) {
        return false;
    }

    ShellRedirection dup_redir;
    memset(&dup_redir, 0, sizeof(dup_redir));
    dup_redir.fd = STDERR_FILENO;
    dup_redir.kind = SHELL_RUNTIME_REDIR_DUP;
    dup_redir.dup_target_fd = STDOUT_FILENO;

    ShellRedirection *new_redirs =
        (ShellRedirection *)realloc(cmd->redirs, sizeof(ShellRedirection) * (cmd->redir_count + 2));
    if (!new_redirs) {
        free(redir.path);
        return false;
    }
    cmd->redirs = new_redirs;
    cmd->redirs[cmd->redir_count++] = redir;
    cmd->redirs[cmd->redir_count++] = dup_redir;
    return true;
}

static void shellPatchCombinedRedirectTokens(ShellCommand *cmd) {
    if (!cmd || cmd->argc == 0 || !cmd->argv) {
        return;
    }
    for (size_t i = 0; i + 1 < cmd->argc; ) {
        const char *tok = cmd->argv[i];
        if (!tok) {
            ++i;
            continue;
        }
        bool match_append = (strcmp(tok, "&>>") == 0);
        bool match_trunc = (strcmp(tok, "&>") == 0);
        if (!match_append && !match_trunc) {
            ++i;
            continue;
        }
        const char *path = cmd->argv[i + 1];
        if (!path || *path == '\0') {
            ++i;
            continue;
        }
        if (!shellAppendCombinedRedirect(cmd, path, match_append)) {
            ++i;
            continue;
        }
        /* Remove the operator and path from argv */
        free(cmd->argv[i]);
        free(cmd->argv[i + 1]);
        for (size_t j = i + 2; j < cmd->argc; ++j) {
            cmd->argv[j - 2] = cmd->argv[j];
        }
        cmd->argc -= 2;
        cmd->argv[cmd->argc] = NULL;
        cmd->argv[cmd->argc + 1] = NULL;
        /* Do not advance i so we can catch multiple operators */
    }
}

static void shellHashRecordExecution(const char *name);
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
    bool saw_command_word = false;
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
            if (!shellAddArg(out_cmd, v.s_val, &saw_command_word)) {
                runtimeError(vm, "shell exec: unable to add argument");
                shellFreeCommand(out_cmd);
                return false;
            }
        }
    }
    shellRewriteDoubleBracketTest(out_cmd);
    return true;
}

static bool shellAliasBufferAppendChar(char **buffer, size_t *length, size_t *capacity, char ch) {
    if (!buffer || !length || !capacity) {
        return false;
    }
    if (*length + 1 >= *capacity) {
        size_t new_capacity = (*capacity == 0) ? 32 : (*capacity * 2);
        while (new_capacity < (*length + 2)) {
            new_capacity *= 2;
        }
        char *resized = (char *)realloc(*buffer, new_capacity);
        if (!resized) {
            return false;
        }
        *buffer = resized;
        *capacity = new_capacity;
    }
    (*buffer)[(*length)++] = ch;
    return true;
}

static bool shellAliasFlushWord(char ***out_words,
                                size_t *out_count,
                                size_t *out_capacity,
                                char **buffer,
                                size_t *length) {
    if (!out_words || !out_count || !out_capacity || !buffer || !length) {
        return false;
    }
    if (*length == 0) {
        return true;
    }
    char *word = (char *)malloc(*length + 1);
    if (!word) {
        return false;
    }
    memcpy(word, *buffer, *length);
    word[*length] = '\0';
    if (*out_count >= *out_capacity) {
        size_t new_capacity = (*out_capacity == 0) ? 4 : (*out_capacity * 2);
        char **resized = (char **)realloc(*out_words, new_capacity * sizeof(char *));
        if (!resized) {
            free(word);
            return false;
        }
        *out_words = resized;
        *out_capacity = new_capacity;
    }
    (*out_words)[(*out_count)++] = word;
    *length = 0;
    return true;
}

static void shellAliasFreeWordList(char **words, size_t count) {
    if (!words) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(words[i]);
    }
    free(words);
}

static bool shellAliasTokenizeValue(const char *value, char ***out_words, size_t *out_count) {
    if (out_words) {
        *out_words = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!value || *value == '\0') {
        return true;
    }

    char **words = NULL;
    size_t word_count = 0;
    size_t word_capacity = 0;
    char *buffer = NULL;
    size_t buffer_len = 0;
    size_t buffer_capacity = 0;
    bool in_single = false;
    bool in_double = false;
    bool escaping = false;

    for (const char *cursor = value; *cursor; ++cursor) {
        char c = *cursor;
        if (escaping) {
            if (!shellAliasBufferAppendChar(&buffer, &buffer_len, &buffer_capacity, c)) {
                shellAliasFreeWordList(words, word_count);
                free(buffer);
                return false;
            }
            escaping = false;
            continue;
        }
        if (in_single) {
            if (c == '\'') {
                in_single = false;
            } else {
                if (!shellAliasBufferAppendChar(&buffer, &buffer_len, &buffer_capacity, c)) {
                    shellAliasFreeWordList(words, word_count);
                    free(buffer);
                    return false;
                }
            }
            continue;
        }
        if (in_double) {
            if (c == '"') {
                in_double = false;
                continue;
            }
            if (c == '\\') {
                escaping = true;
                continue;
            }
            if (!shellAliasBufferAppendChar(&buffer, &buffer_len, &buffer_capacity, c)) {
                shellAliasFreeWordList(words, word_count);
                free(buffer);
                return false;
            }
            continue;
        }
        if (c == '\\') {
            escaping = true;
            continue;
        }
        if (c == '\'') {
            in_single = true;
            continue;
        }
        if (c == '"') {
            in_double = true;
            continue;
        }
        if (isspace((unsigned char)c)) {
            if (!shellAliasFlushWord(&words, &word_count, &word_capacity, &buffer, &buffer_len)) {
                shellAliasFreeWordList(words, word_count);
                free(buffer);
                return false;
            }
            continue;
        }
        if (!shellAliasBufferAppendChar(&buffer, &buffer_len, &buffer_capacity, c)) {
            shellAliasFreeWordList(words, word_count);
            free(buffer);
            return false;
        }
    }
    if (escaping) {
        if (!shellAliasBufferAppendChar(&buffer, &buffer_len, &buffer_capacity, '\\')) {
            shellAliasFreeWordList(words, word_count);
            free(buffer);
            return false;
        }
    }
    if (!shellAliasFlushWord(&words, &word_count, &word_capacity, &buffer, &buffer_len)) {
        shellAliasFreeWordList(words, word_count);
        free(buffer);
        return false;
    }

    free(buffer);
    if (out_words) {
        *out_words = words;
    } else {
        shellAliasFreeWordList(words, word_count);
    }
    if (out_count) {
        *out_count = word_count;
    }
    return true;
}

static bool shellAliasApplyTokens(ShellCommand *cmd, char **alias_words, size_t alias_count) {
    if (!cmd || !cmd->argv) {
        shellAliasFreeWordList(alias_words, alias_count);
        return false;
    }

    size_t remaining = (cmd->argc > 0) ? (cmd->argc - 1) : 0;
    size_t new_argc = alias_count + remaining;
    char **new_argv = (char **)calloc(new_argc + 1, sizeof(char *));
    if (!new_argv) {
        shellAliasFreeWordList(alias_words, alias_count);
        return false;
    }

    size_t dest = 0;
    for (size_t i = 0; i < alias_count; ++i) {
        new_argv[dest++] = alias_words[i];
        alias_words[i] = NULL;
    }
    for (size_t i = 1; i < cmd->argc; ++i) {
        new_argv[dest++] = cmd->argv[i];
        cmd->argv[i] = NULL;
    }

    if (cmd->argc > 0 && cmd->argv[0]) {
        free(cmd->argv[0]);
    }
    free(cmd->argv);
    if (new_argc == 0) {
        free(new_argv);
        cmd->argv = NULL;
        cmd->argc = 0;
        shellAliasFreeWordList(alias_words, alias_count);
        return true;
    }
    cmd->argv = new_argv;
    cmd->argc = new_argc;
    cmd->argv[cmd->argc] = NULL;

    free(alias_words);
    return true;
}

static bool shellExpandAliasOnce(ShellCommand *cmd, const char *value) {
    if (!cmd || !value) {
        return true;
    }
    char **alias_words = NULL;
    size_t alias_count = 0;
    if (!shellAliasTokenizeValue(value, &alias_words, &alias_count)) {
        return false;
    }
    if (alias_count == 0 && cmd->argc <= 1) {
        if (cmd->argc == 1 && cmd->argv) {
            free(cmd->argv[0]);
            free(cmd->argv);
            cmd->argv = NULL;
            cmd->argc = 0;
        }
        shellAliasFreeWordList(alias_words, alias_count);
        return true;
    }
    bool ok = shellAliasApplyTokens(cmd, alias_words, alias_count);
    return ok;
}

static bool shellExpandAliasesIfNeeded(VM *vm, ShellCommand *cmd) {
    if (!cmd || cmd->argc == 0 || cmd->ignore_functions) {
        return true;
    }
    if (!shellShoptOptionEnabled("expand_aliases")) {
        return true;
    }
    int depth = 0;
    enum { kMaxAliasDepth = 16 };
    const char *expanded_names[kMaxAliasDepth];
    size_t expanded_count = 0;
    while (cmd->argc > 0 && depth < kMaxAliasDepth) {
        const char *name = cmd->argv[0];
        if (!name) {
            break;
        }
        bool already_expanded = false;
        for (size_t i = 0; i < expanded_count; ++i) {
            if (strcmp(expanded_names[i], name) == 0) {
                already_expanded = true;
                break;
            }
        }
        if (already_expanded) {
            break;
        }
        ShellAlias *alias = shellFindAlias(name);
        if (!alias || !alias->value) {
            break;
        }
        if (!shellExpandAliasOnce(cmd, alias->value)) {
            if (vm) {
                runtimeError(vm, "shell exec: failed to expand alias '%s'", name);
            }
            return false;
        }
        if (expanded_count < kMaxAliasDepth) {
            expanded_names[expanded_count++] = alias->name ? alias->name : name;
        }
        if (!cmd->argv || cmd->argc == 0 || !cmd->argv[0]) {
            break;
        }
        depth++;
    }
    if (depth >= kMaxAliasDepth) {
        if (vm) {
            const char *label = (cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<alias>";
            runtimeError(vm, "shell exec: alias expansion depth exceeded for '%s'", label);
        }
        return false;
    }
    return true;
}

typedef enum {
    SHELL_REDIR_OP_OPEN,
    SHELL_REDIR_OP_DUP,
    SHELL_REDIR_OP_HEREDOC,
    SHELL_REDIR_OP_HERE_STRING
} ShellRuntimeRedirOpType;

typedef struct {
    ShellRuntimeRedirOpType type;
    int target_fd;
    int source_fd;
    int write_fd;
    const char *here_body;
    size_t here_length;
    bool close_target;
    bool append_newline;
} ShellRuntimeRedirOp;

static int shellSpawnProcess(VM *vm,
                             const ShellCommand *cmd,
                             int stdin_fd,
                             int stdout_fd,
                             int stderr_fd,
                             pid_t *child_pid,
                             bool ignore_job_signals) {
    if (!cmd || cmd->argc == 0 || !cmd->argv || !cmd->argv[0] || !child_pid) {
        return EINVAL;
    }

    ShellRuntimeRedirOp local_ops[16];
    memset(local_ops, 0, sizeof(local_ops));
    ShellRuntimeRedirOp *ops = local_ops;
    size_t op_capacity = sizeof(local_ops) / sizeof(local_ops[0]);
    if (cmd->redir_count > op_capacity) {
        ops = (ShellRuntimeRedirOp *)calloc(cmd->redir_count, sizeof(ShellRuntimeRedirOp));
        if (!ops) {
            return ENOMEM;
        }
        op_capacity = cmd->redir_count;
    }

    size_t op_count = 0;
    int prep_error = 0;
    for (size_t i = 0; i < cmd->redir_count; ++i) {
        if (op_count >= op_capacity) {
            prep_error = ENOMEM;
            goto spawn_cleanup;
        }
        const ShellRedirection *redir = &cmd->redirs[i];
        ShellRuntimeRedirOp op;
        memset(&op, 0, sizeof(op));
        op.target_fd = redir->fd;
        switch (redir->kind) {
            case SHELL_RUNTIME_REDIR_OPEN: {
                if (!redir->path) {
                    prep_error = EINVAL;
                    goto spawn_cleanup;
                }
                int fd = open(redir->path, redir->flags, redir->mode);
                if (fd < 0) {
                    prep_error = errno;
                    goto spawn_cleanup;
                }
                op.type = SHELL_REDIR_OP_OPEN;
                op.source_fd = fd;
                break;
            }
            case SHELL_RUNTIME_REDIR_DUP: {
                op.type = SHELL_REDIR_OP_DUP;
                op.close_target = redir->close_target;
                op.source_fd = redir->dup_target_fd;
                if (!op.close_target && op.source_fd < 0) {
                    prep_error = EBADF;
                    goto spawn_cleanup;
                }
                break;
            }
            case SHELL_RUNTIME_REDIR_HEREDOC: {
                int pipefd[2];
                if (pipe(pipefd) != 0) {
                    prep_error = errno;
                    goto spawn_cleanup;
                }
                op.type = SHELL_REDIR_OP_HEREDOC;
                op.source_fd = pipefd[0];
                op.write_fd = pipefd[1];
                op.here_body = redir->here_doc ? redir->here_doc : "";
                op.here_length = redir->here_doc_length;
                op.append_newline = false;
                break;
            }
            case SHELL_RUNTIME_REDIR_HERE_STRING: {
                int pipefd[2];
                if (pipe(pipefd) != 0) {
                    prep_error = errno;
                    goto spawn_cleanup;
                }
                op.type = SHELL_REDIR_OP_HERE_STRING;
                op.source_fd = pipefd[0];
                op.write_fd = pipefd[1];
                const char *body = redir->here_string ? redir->here_string
                                                      : (redir->here_string_literal ? redir->here_string_literal : "");
                op.here_body = body;
                op.here_length = redir->here_string_length;
                if (op.here_length == 0 && body) {
                    op.here_length = strlen(body);
                }
                op.append_newline = true;
                break;
            }
            default:
                prep_error = EINVAL;
                goto spawn_cleanup;
        }
        ops[op_count++] = op;
    }

    {
    pid_t child = fork();
        if (child < 0) {
            prep_error = errno;
            goto spawn_cleanup;
        }

        if (child == 0) {
            ShellPipelineContext *ctx = &gShellRuntime.pipeline;
            pid_t desired_pgid = getpid();
            if (ctx->active && ctx->pgid > 0) {
                desired_pgid = ctx->pgid;
            }
            if (setpgid(0, desired_pgid) != 0) {
                /* best-effort; ignore errors */
            }

            if (ignore_job_signals) {
                signal(SIGINT, SIG_IGN);
                signal(SIGQUIT, SIG_IGN);
            } else {
                signal(SIGINT, SIG_DFL);
                signal(SIGQUIT, SIG_DFL);
            }
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);

            if (ctx->active && ctx->pipes) {
                size_t pipe_count = (ctx->stage_count > 0) ? (ctx->stage_count - 1) : 0;
                for (size_t i = 0; i < pipe_count; ++i) {
                    int r = ctx->pipes[i][0];
                    int w = ctx->pipes[i][1];
                    if (r >= 0 && r != stdin_fd && r != stdout_fd && r != stderr_fd) {
                        close(r);
                    }
                    if (w >= 0 && w != stdin_fd && w != stdout_fd && w != stderr_fd) {
                        close(w);
                    }
                }
            }

            if (stdin_fd >= 0 && dup2(stdin_fd, STDIN_FILENO) < 0) {
                int err = errno;
                fprintf(stderr, "exsh: failed to setup stdin: %s\n", strerror(err));
                _exit(126);
            }
            if (stdout_fd >= 0 && dup2(stdout_fd, STDOUT_FILENO) < 0) {
                int err = errno;
                fprintf(stderr, "exsh: failed to setup stdout: %s\n", strerror(err));
                _exit(126);
            }
            if (stderr_fd >= 0 && dup2(stderr_fd, STDERR_FILENO) < 0) {
                int err = errno;
                fprintf(stderr, "exsh: failed to setup stderr: %s\n", strerror(err));
                _exit(126);
            }

            for (size_t i = 0; i < op_count; ++i) {
                ShellRuntimeRedirOp *op = &ops[i];
                if ((op->type == SHELL_REDIR_OP_HEREDOC || op->type == SHELL_REDIR_OP_HERE_STRING) &&
                    op->write_fd >= 0) {
                    close(op->write_fd);
                    op->write_fd = -1;
                }
            }

            for (size_t i = 0; i < op_count; ++i) {
                ShellRuntimeRedirOp *op = &ops[i];
                switch (op->type) {
                    case SHELL_REDIR_OP_OPEN:
                    case SHELL_REDIR_OP_HEREDOC:
                    case SHELL_REDIR_OP_HERE_STRING:
                        if (dup2(op->source_fd, op->target_fd) < 0) {
                            int err = errno;
                            fprintf(stderr, "exsh: %s: %s\n", cmd->argv[0], strerror(err));
                            _exit(126);
                        }
                        break;
                    case SHELL_REDIR_OP_DUP:
                        if (op->close_target) {
                            close(op->target_fd);
                        } else if (dup2(op->source_fd, op->target_fd) < 0) {
                            int err = errno;
                            fprintf(stderr, "exsh: %s: %s\n", cmd->argv[0], strerror(err));
                            _exit(126);
                        }
                        break;
                }
            }

            if (stdin_fd >= 0 && stdin_fd != STDIN_FILENO) {
                close(stdin_fd);
            }
            if (stdout_fd >= 0 && stdout_fd != STDOUT_FILENO && stdout_fd != stderr_fd) {
                close(stdout_fd);
            }
            if (stderr_fd >= 0 && stderr_fd != STDERR_FILENO) {
                close(stderr_fd);
            }

            for (size_t i = 0; i < op_count; ++i) {
                ShellRuntimeRedirOp *op = &ops[i];
                if ((op->type == SHELL_REDIR_OP_OPEN || op->type == SHELL_REDIR_OP_HEREDOC ||
                     op->type == SHELL_REDIR_OP_HERE_STRING) &&
                    op->source_fd >= 0 && op->source_fd != op->target_fd) {
                    close(op->source_fd);
                    op->source_fd = -1;
                }
            }

            bool builtin_ran = shellInvokeBuiltin(vm ? vm : gShellCurrentVm, (ShellCommand *)cmd);
            if (builtin_ran) {
                int status = gShellRuntime.last_status;
                shellFlushStandardStreams();
                _exit(status);
            }

#if defined(PSCAL_TARGET_IOS)
            int shebang_status = shellMaybeExecShebangTool(cmd->argv[0], cmd->argv);
            if (shebang_status >= 0) {
                _exit(shebang_status);
            }
#endif
            execvp(cmd->argv[0], cmd->argv);
            int err = errno;
            int script_status = shellTryRunScriptFallback(vm ? vm : gShellCurrentVm, cmd);
            if (script_status >= 0) {
                _exit(script_status);
            }
            fprintf(stderr, "exsh: %s: %s\n", cmd->argv[0], strerror(err));
            _exit((err == ENOENT) ? 127 : 126);
        }
        for (size_t j = 0; j < op_count; ++j) {
            ShellRuntimeRedirOp *op = &ops[j];
            if (op->type == SHELL_REDIR_OP_OPEN) {
                if (op->source_fd >= 0) {
                    close(op->source_fd);
                    op->source_fd = -1;
                }
            } else if (op->type == SHELL_REDIR_OP_HEREDOC || op->type == SHELL_REDIR_OP_HERE_STRING) {
                if (op->source_fd >= 0) {
                    close(op->source_fd);
                    op->source_fd = -1;
                }
                if (op->write_fd >= 0) {
                    const char *body = op->here_body ? op->here_body : "";
                    size_t remaining = op->here_length;
                    if (remaining == 0 && body) {
                        remaining = strlen(body);
                    }
                    const char *cursor = body;
                    while (remaining > 0) {
                        ssize_t written = write(op->write_fd, cursor, remaining);
                        if (written < 0) {
                            if (errno == EINTR) {
                                continue;
                            }
                            break;
                        }
                        cursor += written;
                        remaining -= (size_t)written;
                    }
                    if (op->append_newline) {
                        const char newline = '\n';
                        while (true) {
                            ssize_t written = write(op->write_fd, &newline, 1);
                            if (written < 0) {
                                if (errno == EINTR) {
                                    continue;
                                }
                                break;
                            }
                            break;
                        }
                    }
                    close(op->write_fd);
                    op->write_fd = -1;
                }
            }
        }
        if (ops != local_ops) {
            free(ops);
        }
        *child_pid = child;
        return 0;
    }

spawn_cleanup:
    for (size_t j = 0; j < op_count; ++j) {
        if (ops[j].type == SHELL_REDIR_OP_OPEN || ops[j].type == SHELL_REDIR_OP_HEREDOC ||
            ops[j].type == SHELL_REDIR_OP_HERE_STRING) {
            if (ops[j].source_fd >= 0) {
                close(ops[j].source_fd);
                ops[j].source_fd = -1;
            }
        }
        if ((ops[j].type == SHELL_REDIR_OP_HEREDOC || ops[j].type == SHELL_REDIR_OP_HERE_STRING) &&
            ops[j].write_fd >= 0) {
            close(ops[j].write_fd);
            ops[j].write_fd = -1;
        }
    }
    if (ops != local_ops) {
        free(ops);
    }
    return prep_error;
}

typedef struct {
    const pid_t *pids;
    size_t count;
} ShellSigWatchCtx;

static void *shellSignalWatchThread(void *arg) {
    ShellSigWatchCtx *w = (ShellSigWatchCtx *)arg;
    if (!w || !w->pids || w->count == 0) {
        free(w);
        return NULL;
    }
    for (;;) {
        unsigned char ch = 0;
        ssize_t r = read(STDIN_FILENO, &ch, 1);
        if (r <= 0) {
            break;
        }
        if (ch == 3) { /* Ctrl-C */
            for (size_t i = 0; i < w->count; ++i) {
                if (w->pids[i] > 0) {
                    vprocKillShim(w->pids[i], SIGINT);
                }
            }
            break;
        } else if (ch == 26) { /* Ctrl-Z */
            for (size_t i = 0; i < w->count; ++i) {
                if (w->pids[i] > 0) {
                    vprocKillShim(w->pids[i], SIGTSTP);
                }
            }
            break;
        } else {
            /* Put the byte back so the foreground command can read it. */
            ungetc(ch, stdin);
            break;
        }
    }
    free(w);
    return NULL;
}

static int shellWaitPid(pid_t pid, int *status_out, bool allow_stop, bool *out_stopped) {
    pthread_t sig_thread;
    bool sig_thread_started = false;
    pid_t watch_list[1] = { pid };

    if (allow_stop
#if defined(PSCAL_TARGET_IOS)
        && pscalRuntimeVirtualTTYEnabled()
#endif
    ) {
        ShellSigWatchCtx *ctx = (ShellSigWatchCtx *)calloc(1, sizeof(ShellSigWatchCtx));
        if (ctx) {
            ctx->pids = watch_list;
            ctx->count = 1;
            if (pthread_create(&sig_thread, NULL, shellSignalWatchThread, ctx) == 0) {
                sig_thread_started = true;
                pthread_detach(sig_thread);
            } else {
                free(ctx);
            }
        }
    }

    if (out_stopped) {
        *out_stopped = false;
    }
    int status = 0;
    int options = allow_stop ? WUNTRACED : 0;
    pid_t waited;
    do {
        waited = vprocWaitPidShim(pid, &status, options);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        return errno;
    }
    if (out_stopped && WIFSTOPPED(status)) {
        *out_stopped = true;
    }
    if (status_out) {
        if (WIFEXITED(status)) {
            *status_out = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            *status_out = 128 + WTERMSIG(status);
        } else if (WIFSTOPPED(status)) {
            *status_out = 128 + WSTOPSIG(status);
        } else {
            *status_out = status;
        }
    }
    return 0;
}

#if !defined(SHELL_TOOL_THREAD_STACK_SZ)
#define SHELL_TOOL_THREAD_STACK_SZ (8 * 1024 * 1024)
#endif

#if defined(PSCAL_TARGET_IOS)
typedef struct {
    int argc;
    char **argv;
} ShellShebangThreadCtx;

typedef struct {
    VM *vm;
    ShellCommand *cmd;
    int synthetic_pid;
} ShellBackgroundStageCtx;

static int shellRunBuiltinInProcess(VM *vm,
                                    ShellCommand *cmd,
                                    int stdin_fd,
                                    int stdout_fd,
                                    int stderr_fd,
                                    bool apply_exec_redirs);

static ShellCommand *shellCloneCommandForBackground(const ShellCommand *src) {
    if (!src) {
        return NULL;
    }
    ShellCommand *dst = (ShellCommand *)calloc(1, sizeof(ShellCommand));
    if (!dst) {
        return NULL;
    }

    dst->argc = src->argc;
    if (dst->argc > 0) {
        dst->argv = (char **)calloc(dst->argc + 1, sizeof(char *));
        if (!dst->argv) {
            shellFreeCommand(dst);
            free(dst);
            return NULL;
        }
        for (size_t i = 0; i < dst->argc; ++i) {
            if (src->argv && src->argv[i]) {
                dst->argv[i] = strdup(src->argv[i]);
                if (!dst->argv[i]) {
                    shellFreeCommand(dst);
                    free(dst);
                    return NULL;
                }
            }
        }
        dst->argv[dst->argc] = NULL;
    }

    dst->assignment_count = src->assignment_count;
    if (dst->assignment_count > 0) {
        dst->assignments = (ShellAssignmentEntry *)calloc(dst->assignment_count, sizeof(ShellAssignmentEntry));
        if (!dst->assignments) {
            shellFreeCommand(dst);
            free(dst);
            return NULL;
        }
        for (size_t i = 0; i < dst->assignment_count; ++i) {
            dst->assignments[i].is_array_literal = src->assignments[i].is_array_literal;
            if (src->assignments[i].text) {
                dst->assignments[i].text = strdup(src->assignments[i].text);
                if (!dst->assignments[i].text) {
                    shellFreeCommand(dst);
                    free(dst);
                    return NULL;
                }
            }
        }
    }

    dst->redir_count = src->redir_count;
    if (dst->redir_count > 0) {
        dst->redirs = (ShellRedirection *)calloc(dst->redir_count, sizeof(ShellRedirection));
        if (!dst->redirs) {
            shellFreeCommand(dst);
            free(dst);
            return NULL;
        }
        for (size_t i = 0; i < dst->redir_count; ++i) {
            ShellRedirection *dr = &dst->redirs[i];
            const ShellRedirection *sr = &src->redirs[i];
            *dr = *sr;
            dr->path = sr->path ? strdup(sr->path) : NULL;
            dr->here_doc = sr->here_doc ? strdup(sr->here_doc) : NULL;
            dr->here_string_literal = sr->here_string_literal ? strdup(sr->here_string_literal) : NULL;
            dr->here_string = sr->here_string ? strdup(sr->here_string) : NULL;
            if ((sr->path && !dr->path) ||
                (sr->here_doc && !dr->here_doc) ||
                (sr->here_string_literal && !dr->here_string_literal) ||
                (sr->here_string && !dr->here_string)) {
                shellFreeCommand(dst);
                free(dst);
                return NULL;
            }
        }
    }

    dst->background = src->background;
    dst->disable_field_splitting = src->disable_field_splitting;
    dst->ignore_functions = src->ignore_functions;
    dst->pipeline_index = src->pipeline_index;
    dst->is_pipeline_head = src->is_pipeline_head;
    dst->is_pipeline_tail = src->is_pipeline_tail;
    dst->line = src->line;
    dst->column = src->column;
    return dst;
}

static void shellShebangThreadCtxFree(ShellShebangThreadCtx *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->argv) {
        for (int i = 0; i < ctx->argc; ++i) {
            free(ctx->argv[i]);
        }
        free(ctx->argv);
    }
    free(ctx);
}

static void *shellShebangBackgroundMain(void *arg) {
    ShellShebangThreadCtx *ctx = (ShellShebangThreadCtx *)arg;
    if (!ctx || !ctx->argv || ctx->argc <= 0) {
        shellShebangThreadCtxFree(ctx);
        return NULL;
    }
    shellMaybeExecShebangTool(ctx->argv[0], ctx->argv);
    shellShebangThreadCtxFree(ctx);
    return NULL;
}

typedef struct {
    int read_fd;
    int file_fd;
    int mirror_fd; /* duplicate of original stdout/stderr so the shell prompt stays visible */
    int err;
} ShellPipelineTee;

static void *shellPipelineTeeThread(void *arg) {
    ShellPipelineTee *tee = (ShellPipelineTee *)arg;
    const size_t chunk = 4096;
    char *tmp = malloc(chunk);
    if (!tmp) {
        tee->err = ENOMEM;
        close(tee->read_fd);
        close(tee->file_fd);
        return NULL;
    }
    for (;;) {
        ssize_t res = read(tee->read_fd, tmp, chunk);
        if (res < 0) {
            if (errno == EINTR) {
                continue;
            }
            tee->err = errno ? errno : EIO;
            break;
        }
        if (res == 0) {
            break;
        }
        ssize_t written = 0;
        while (written < res) {
            ssize_t to_write = (size_t)res - (size_t)written;
            ssize_t w = write(tee->file_fd, tmp + written, to_write);
            if (w < 0) {
                if (errno == EINTR) continue;
                tee->err = errno ? errno : EIO;
                goto done;
            }
            written += w;
        }
        if (tee->mirror_fd >= 0) {
            ssize_t echoed = 0;
            while (echoed < res) {
                ssize_t e = write(tee->mirror_fd, tmp + echoed, (size_t)res - (size_t)echoed);
                if (e < 0) {
                    if (errno == EINTR) continue;
                    /* Do not fail the tee on mirror errors; keep file writes authoritative. */
                    break;
                }
                echoed += e;
            }
        }
    }

done:
    close(tee->read_fd);
    close(tee->file_fd);
    if (tee->mirror_fd >= 0) {
        close(tee->mirror_fd);
    }
    free(tmp);
    return NULL;
}

static void *shellInProcessBackgroundMain(void *arg) {
    ShellBackgroundStageCtx *ctx = (ShellBackgroundStageCtx *)arg;
    if (!ctx) {
        return NULL;
    }
#if defined(PSCAL_TARGET_IOS)
    /* Ensure tool frontends are registered in this process before we spin up
     * a fresh runtime/VM for the background worker. */
    static pthread_once_t bg_builtin_once = PTHREAD_ONCE_INIT;
    pthread_once(&bg_builtin_once, registerExtendedBuiltins);
    /* Install isolated shell runtime + VM so background work does not mutate
     * the interactive shell's state or gShellCurrentVm. */
    ShellRuntimeState *bg_ctx = shellRuntimeCreateContext();
    ShellRuntimeState *prev_ctx = NULL;
    VM *prev_vm = NULL;
    VM *bg_vm = NULL;
    VM *vm = ctx->vm ? ctx->vm : gShellCurrentVm;
    if (bg_ctx) {
        prev_ctx = shellRuntimeActivateContext(bg_ctx);
        bg_vm = (VM *)calloc(1, sizeof(VM));
        if (bg_vm) {
            initVM(bg_vm);
            bg_vm->frontendContext = bg_ctx;
            prev_vm = shellSwapCurrentVm(bg_vm);
            vm = bg_vm;
        } else {
            shellRuntimeActivateContext(prev_ctx);
            prev_ctx = NULL;
            free(bg_ctx);
            bg_ctx = NULL;
        }
    }
#else
    VM *vm = ctx->vm ? ctx->vm : gShellCurrentVm;
#endif
    ShellCommand *cmd = ctx->cmd;
    size_t redir_count = cmd ? cmd->redir_count : 0;
    const char *stdout_raw = NULL;
    const char *stderr_raw = NULL;
    bool stdout_append = false;
    bool stderr_append = false;
    int stdin_fd = -1;

    if (cmd && redir_count > 0 && cmd->redirs) {
        for (size_t i = 0; i < redir_count; ++i) {
            ShellRedirection *r = &cmd->redirs[i];
            if (r->kind == SHELL_RUNTIME_REDIR_OPEN) {
                if (r->fd == STDOUT_FILENO && r->path) {
                    stdout_raw = r->path;
                    stdout_append = (r->flags & O_APPEND) != 0;
                } else if (r->fd == STDERR_FILENO && r->path) {
                    stderr_raw = r->path;
                    stderr_append = (r->flags & O_APPEND) != 0;
                }
            } else if (r->kind == SHELL_RUNTIME_REDIR_DUP) {
                if (r->fd == STDERR_FILENO && r->dup_target_fd == STDOUT_FILENO && stdout_raw) {
                    stderr_raw = stdout_raw;
                    stderr_append = stdout_append;
                }
            }
        }
    }

    char resolved_stdout[PATH_MAX];
    char resolved_stderr[PATH_MAX];
    const char *stdout_env = stdout_raw;
    const char *stderr_env = stderr_raw;
#if defined(PSCAL_TARGET_IOS)
    if (stdout_raw && pathTruncateExpand(stdout_raw, resolved_stdout, sizeof(resolved_stdout))) {
        stdout_env = resolved_stdout;
    }
    if (stderr_raw && pathTruncateExpand(stderr_raw, resolved_stderr, sizeof(resolved_stderr))) {
        stderr_env = resolved_stderr;
    }
#endif
    char *stdout_path = stdout_env ? strdup(stdout_env) : NULL;
    char *stderr_path = stderr_env ? strdup(stderr_env) : NULL;
    if (stdout_path) {
        setenv("PSCALI_BG_STDOUT", stdout_path, 1);
        setenv("PSCALI_BG_STDOUT_APPEND", stdout_append ? "1" : "0", 1);
    } else {
        unsetenv("PSCALI_BG_STDOUT");
        unsetenv("PSCALI_BG_STDOUT_APPEND");
    }
    if (stderr_path) {
        setenv("PSCALI_BG_STDERR", stderr_path, 1);
        setenv("PSCALI_BG_STDERR_APPEND", stderr_append ? "1" : "0", 1);
    } else {
        unsetenv("PSCALI_BG_STDERR");
        unsetenv("PSCALI_BG_STDERR_APPEND");
    }
    int stdout_pipe[2] = { -1, -1 };
    int stderr_pipe[2] = { -1, -1 };
    int stdout_target = -1;
    int stderr_target = -1;
    pthread_t tee_threads[2];
    ShellPipelineTee tee_ctx[2];
    int tee_count = 0;
    int stdout_mirror = -1;
    int stderr_mirror = -1;
    int vm_stdout = -1;
    int vm_stderr = -1;
    int bg_status = 0;
#if defined(PSCAL_TARGET_IOS)
    VProc *vp = NULL;
#endif

    stdout_mirror = dup(STDOUT_FILENO);
    stderr_mirror = dup(STDERR_FILENO);
    if (stdout_path && *stdout_path) {
        int flags = O_CREAT | O_WRONLY | (stdout_append ? O_APPEND : O_TRUNC);
        stdout_target = open(stdout_path, flags, 0666);
        if (stdout_target >= 0 && pipe(stdout_pipe) == 0) {
            tee_ctx[tee_count].read_fd = stdout_pipe[0];
            tee_ctx[tee_count].file_fd = stdout_target;
            tee_ctx[tee_count].mirror_fd = (stdout_mirror >= 0) ? stdout_mirror : -1;
            tee_ctx[tee_count].err = 0;
            pthread_create(&tee_threads[tee_count], NULL, shellPipelineTeeThread, &tee_ctx[tee_count]);
            ++tee_count;
        } else if (stdout_target >= 0) {
            close(stdout_target);
            stdout_target = -1;
        }
    }
    if (stderr_path && *stderr_path) {
        int flags = O_CREAT | O_WRONLY | (stderr_append ? O_APPEND : O_TRUNC);
        stderr_target = open(stderr_path, flags, 0666);
        if (stderr_target >= 0 && pipe(stderr_pipe) == 0) {
            tee_ctx[tee_count].read_fd = stderr_pipe[0];
            tee_ctx[tee_count].file_fd = stderr_target;
            tee_ctx[tee_count].mirror_fd = (stderr_mirror >= 0) ? stderr_mirror : -1;
            tee_ctx[tee_count].err = 0;
            pthread_create(&tee_threads[tee_count], NULL, shellPipelineTeeThread, &tee_ctx[tee_count]);
            ++tee_count;
        } else if (stderr_target >= 0) {
            close(stderr_target);
            stderr_target = -1;
        }
    }

    vm_stdout = (stdout_pipe[1] >= 0) ? stdout_pipe[1] : -1;
    vm_stderr = (stderr_pipe[1] >= 0) ? stderr_pipe[1] : -1;
#if defined(PSCAL_TARGET_IOS)
    vp = shellCreateVProcForStage(stdin_fd,
                                  vm_stdout,
                                  vm_stderr,
                                  pscalRuntimeDetectWindowCols(),
                                  pscalRuntimeDetectWindowRows(),
                                  ctx->synthetic_pid > 0 ? ctx->synthetic_pid : -1);
    if (!vp) {
        runtimeError(vm, "background: failed to allocate vproc");
        goto cleanup;
    }
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
#endif
    gShellBackgroundWorkerActive = true;
    bg_status = shellRunBuiltinInProcess(vm, cmd, stdin_fd, vm_stdout, vm_stderr, false);
    gShellBackgroundWorkerActive = false;

cleanup:
#if defined(PSCAL_TARGET_IOS)
    if (vp) {
        /* If the synthetic task was terminated via vprocKillShim, reflect that
         * status rather than a default 0 from the builtin dispatcher. */
        size_t snap_cap = vprocSnapshot(NULL, 0);
        if (snap_cap > 0) {
            VProcSnapshot *snaps = (VProcSnapshot *)calloc(snap_cap, sizeof(VProcSnapshot));
            if (snaps) {
                size_t snap_count = vprocSnapshot(snaps, snap_cap);
                for (size_t i = 0; i < snap_count; ++i) {
                    if (snaps[i].pid == vprocPid(vp) && snaps[i].exited) {
                        bg_status = snaps[i].status;
                        break;
                    }
                }
                free(snaps);
            }
        }
    }
    fprintf(stderr, "[pipeline-bg] command '%s' status=%d\n",
            (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>", bg_status);
    vprocDeactivate();
    if (vp) {
        vprocMarkExit(vp, bg_status);
        vprocDestroy(vp);
    }
#endif
    if (stdin_fd >= 0) close(stdin_fd);
    if (vm_stdout >= 0) close(vm_stdout);
    if (vm_stderr >= 0) close(vm_stderr);
    for (int i = 0; i < tee_count; ++i) {
        pthread_join(tee_threads[i], NULL);
    }
    if (stdout_mirror >= 0) close(stdout_mirror);
    if (stderr_mirror >= 0) close(stderr_mirror);
    unsetenv("PSCALI_BG_STDOUT");
    unsetenv("PSCALI_BG_STDOUT_APPEND");
    unsetenv("PSCALI_BG_STDERR");
    unsetenv("PSCALI_BG_STDERR_APPEND");
    shellFreeCommand(cmd);
    free(stdout_path);
    free(stderr_path);
    free(cmd);
#if defined(PSCAL_TARGET_IOS)
    if (bg_vm) {
        shellRestoreCurrentVm(prev_vm);
    }
    if (prev_ctx) {
        shellRuntimeActivateContext(prev_ctx);
    }
    if (bg_vm) {
        freeVM(bg_vm);
        free(bg_vm);
    }
    if (bg_ctx) {
        free(bg_ctx);
    }
#endif
    free(ctx);
    return NULL;
}
#endif

static int shellFinishPipeline(const ShellCommand *tail_cmd);

static void shellResetPipeline(void) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (!ctx->active) {
        return;
    }
#if defined(PSCAL_TARGET_IOS)
    if (ctx->spool_fd >= 0) {
        close(ctx->spool_fd);
        ctx->spool_fd = -1;
        ctx->spool_size = 0;
    }
    ctx->in_process = false;
#endif
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
    free(ctx->merge_stderr);
    ctx->merge_stderr = NULL;
    ctx->active = false;
    ctx->stage_count = 0;
    ctx->launched = 0;
    ctx->background = false;
    ctx->last_status = 0;
    ctx->pgid = -1;
}

static void shellMaybeResetPipeline(void) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (!ctx->active) {
        return;
    }
    if (ctx->stage_count > 1) {
        shellFinishPipeline(NULL);
        return;
    }
    shellResetPipeline();
}

static void shellAbortPipeline(void) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (!ctx->active) {
        return;
    }

#if defined(PSCAL_TARGET_IOS)
    if (ctx->spool_fd >= 0) {
        close(ctx->spool_fd);
        ctx->spool_fd = -1;
        ctx->spool_size = 0;
    }
#endif
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
            res = vprocWaitPidShim(pid, &status, WNOHANG);
        } while (res < 0 && errno == EINTR);
        if (res == 0) {
            vprocKillShim(pid, SIGTERM);
            do {
                res = vprocWaitPidShim(pid, &status, 0);
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
    ctx->pgid = -1;
#if defined(PSCAL_TARGET_IOS)
    ctx->in_process = true;
    ctx->spool_fd = -1;
    ctx->spool_size = 0;
#endif
    ctx->pids = calloc(stages, sizeof(pid_t));
    if (!ctx->pids) {
        shellResetPipeline();
        return false;
    }
    if (stages > 0) {
        ctx->merge_stderr = (bool *)calloc(stages, sizeof(bool));
        if (!ctx->merge_stderr) {
            shellResetPipeline();
            return false;
        }
    }
    if (stages > 1
#if defined(PSCAL_TARGET_IOS)
        && !ctx->in_process
#endif
    ) {
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
    pid_t job_pgid = (ctx->pgid > 0) ? ctx->pgid : ((ctx->launched > 0) ? ctx->pids[0] : -1);

#if defined(PSCAL_TARGET_IOS)
    if (ctx->in_process) {
        if (ctx->background) {
            ShellShebangThreadCtx *bg_ctx = NULL;
            if (tail_cmd && tail_cmd->argc > 0 && tail_cmd->argv) {
                bg_ctx = (ShellShebangThreadCtx *)calloc(1, sizeof(ShellShebangThreadCtx));
                if (bg_ctx) {
                    bg_ctx->argc = tail_cmd->argc;
                    bg_ctx->argv = (char **)calloc((size_t)bg_ctx->argc, sizeof(char *));
                    if (!bg_ctx->argv) {
                        shellShebangThreadCtxFree(bg_ctx);
                        bg_ctx = NULL;
                    }
                }
                if (bg_ctx) {
                    for (int i = 0; i < bg_ctx->argc; ++i) {
                        if (tail_cmd->argv[i]) {
                            bg_ctx->argv[i] = strdup(tail_cmd->argv[i]);
                            if (!bg_ctx->argv[i]) {
                                shellShebangThreadCtxFree(bg_ctx);
                                bg_ctx = NULL;
                                break;
                            }
                        }
                    }
                }
            }
            if (bg_ctx) {
                pthread_t tid;
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_setstacksize(&attr, SHELL_TOOL_THREAD_STACK_SZ);
                if (pthread_create(&tid, &attr, shellShebangBackgroundMain, bg_ctx) == 0) {
                    pthread_detach(tid);
                    final_status = 0;
                    ctx->last_status = final_status;
                    shellResetPipeline();
                    shellUpdateStatus(final_status);
                    shellRuntimeProcessPendingSignals();
                    return final_status;
                }
                pthread_attr_destroy(&attr);
                shellShebangThreadCtxFree(bg_ctx);
            }
        }
        if (ctx->negated) {
            final_status = (final_status == 0) ? 1 : 0;
        }
        ctx->last_status = final_status;
        shellResetPipeline();
        shellUpdateStatus(final_status);
        shellRuntimeProcessPendingSignals();
        return final_status;
    }
#endif

    if (!ctx->background) {
        shellEnsureJobControl();
        bool job_control = gShellRuntime.job_control_enabled && job_pgid > 0;
        bool stopped_job = false;

        if (job_control) {
            shellJobControlSetForeground(job_pgid);
        }

        for (size_t i = 0; i < ctx->launched; ++i) {
            pid_t pid = ctx->pids[i];
            if (pid <= 0) {
                continue;
            }
            if (gShellExitRequested || gShellRuntime.break_requested) {
#if defined(PSCAL_TARGET_IOS)
                if (ctx->pgid > 0) {
                    vprocKillShim(-ctx->pgid, SIGTERM);
                } else {
                    vprocKillShim(pid, SIGTERM);
                }
#else
                if (ctx->pgid > 0) {
                    kill(-ctx->pgid, SIGTERM);
                } else {
                    kill(pid, SIGTERM);
                }
#endif
                continue;
            }
            bool stopped = false;
            int status = 0;
            int err = shellWaitPid(pid, &status, job_control, &stopped);
            if (err != 0) {
                continue;
            }
            if (stopped) {
                stopped_job = true;
                final_status = status;
            } else {
                final_status = status;
                ctx->pids[i] = -1;
            }
        }

        if (job_control) {
            shellJobControlRestoreForeground();
        }

        if (!ctx->background && final_status >= 128 && final_status < 128 + NSIG) {
            shellHandlePendingSignal(final_status - 128);
        }

        shellRuntimeProcessPendingSignals();

        if (stopped_job && job_control) {
            ShellJob *job = NULL;
            if (tail_cmd) {
                job = shellRegisterJob(job_pgid, ctx->pids, ctx->launched, tail_cmd);
            }
            if (job) {
                job->stopped = true;
                job->running = false;
                job->last_status = final_status;
            }
            ctx->last_status = final_status;
            shellResetPipeline();
            shellUpdateStatus(final_status);
            return final_status;
        }
    } else if (ctx->launched > 0) {
        ShellJob *job = NULL;
        if (tail_cmd) {
            job = shellRegisterJob(job_pgid, ctx->pids, ctx->launched, tail_cmd);
        }
        if (job) {
            job->running = true;
            job->stopped = false;
            job->last_status = 0;
        }
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

static bool shellCommandIsExecBuiltin(const ShellCommand *cmd) {
    if (!cmd || cmd->argc == 0 || !cmd->argv || !cmd->argv[0]) {
        return false;
    }
    const char *name = cmd->argv[0];
    const char *canonical = shellBuiltinCanonicalName(name);
    if (!canonical) {
        canonical = name;
    }
    return strcasecmp(canonical, "exec") == 0;
}

static bool shellEnsureExecRedirBackup(int target_fd,
                                       int avoid_fd,
                                       ShellExecRedirBackup **backups,
                                       size_t *count,
                                       size_t *capacity) {
    if (target_fd < 0 || !backups || !count || !capacity) {
        return false;
    }
    for (size_t i = 0; i < *count; ++i) {
        if ((*backups)[i].target_fd == target_fd) {
            return true;
        }
    }
    ShellExecRedirBackup backup;
    backup.target_fd = target_fd;
    backup.saved_fd = -1;
    backup.saved_valid = false;
    backup.was_closed = false;
    int min_fd = target_fd + 1;
    if (avoid_fd >= 0 && avoid_fd >= min_fd) {
        min_fd = avoid_fd + 1;
    }
    int dup_fd = -1;
#ifdef F_DUPFD_CLOEXEC
    dup_fd = fcntl(target_fd, F_DUPFD_CLOEXEC, min_fd);
    if (dup_fd < 0 && errno == EINVAL) {
        dup_fd = -1;
    }
#endif
    if (dup_fd < 0) {
        dup_fd = fcntl(target_fd, F_DUPFD, min_fd);
        if (dup_fd >= 0) {
            fcntl(dup_fd, F_SETFD, FD_CLOEXEC);
        }
    }
    if (dup_fd < 0) {
        dup_fd = dup(target_fd);
        if (dup_fd >= 0) {
            fcntl(dup_fd, F_SETFD, FD_CLOEXEC);
        }
    }
    if (dup_fd >= 0 && avoid_fd >= 0 && dup_fd == avoid_fd) {
#ifdef F_DUPFD_CLOEXEC
        int alt_fd = fcntl(target_fd, F_DUPFD_CLOEXEC, avoid_fd + 1);
        if (alt_fd >= 0) {
            close(dup_fd);
            dup_fd = alt_fd;
        } else if (errno == EINVAL) {
            alt_fd = -1;
        }
#else
        int alt_fd = -1;
#endif
        if (dup_fd >= 0 && dup_fd == avoid_fd) {
            if (alt_fd < 0) {
                alt_fd = fcntl(target_fd, F_DUPFD, avoid_fd + 1);
                if (alt_fd >= 0) {
                    fcntl(alt_fd, F_SETFD, FD_CLOEXEC);
                }
            }
            if (alt_fd >= 0) {
                close(dup_fd);
                dup_fd = alt_fd;
            } else {
                close(dup_fd);
                dup_fd = -1;
            }
        }
    }
    if (dup_fd >= 0) {
        backup.saved_fd = dup_fd;
        backup.saved_valid = true;
    } else if (errno == EBADF) {
        backup.was_closed = true;
    } else {
        return false;
    }
    if (*count >= *capacity) {
        size_t new_capacity = (*capacity == 0) ? 4 : (*capacity * 2);
        ShellExecRedirBackup *resized =
            (ShellExecRedirBackup *)realloc(*backups, new_capacity * sizeof(ShellExecRedirBackup));
        if (!resized) {
            if (backup.saved_valid && backup.saved_fd >= 0) {
                close(backup.saved_fd);
            }
            return false;
        }
        *backups = resized;
        *capacity = new_capacity;
    }
    (*backups)[*count] = backup;
    (*count)++;
    return true;
}

static void shellResetStandardStream(int fd) {
    if (fd == STDIN_FILENO) {
        clearerr(stdin);
    } else if (fd == STDOUT_FILENO) {
        clearerr(stdout);
    } else if (fd == STDERR_FILENO) {
        clearerr(stderr);
    }
}

static void shellRestoreExecRedirections(ShellExecRedirBackup *backups, size_t count) {
    if (!backups) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        ShellExecRedirBackup *backup = &backups[i];
        if (backup->saved_valid && backup->saved_fd >= 0) {
            dup2(backup->saved_fd, backup->target_fd);
            shellResetStandardStream(backup->target_fd);
        } else if (backup->was_closed) {
            close(backup->target_fd);
            shellResetStandardStream(backup->target_fd);
        }
    }
}

static void shellFreeExecRedirBackups(ShellExecRedirBackup *backups, size_t count) {
    if (!backups) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        if (backups[i].saved_valid && backups[i].saved_fd >= 0) {
            close(backups[i].saved_fd);
            backups[i].saved_fd = -1;
        }
    }
    free(backups);
}

static bool shellApplyExecRedirections(VM *vm, const ShellCommand *cmd,
                                       ShellExecRedirBackup **out_backups,
                                       size_t *out_count) {
    if (out_backups) {
        *out_backups = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!cmd || cmd->redir_count == 0) {
        return true;
    }

    ShellExecRedirBackup *backups = NULL;
    size_t backup_count = 0;
    size_t backup_capacity = 0;
    bool stdout_redirected = false;

    for (size_t i = 0; i < cmd->redir_count; ++i) {
        const ShellRedirection *redir = &cmd->redirs[i];
        int target_fd = redir->fd;
        int avoid_fd = -1;
        if (redir->kind == SHELL_RUNTIME_REDIR_DUP && !redir->close_target) {
            avoid_fd = redir->dup_target_fd;
        }
        if (!shellEnsureExecRedirBackup(target_fd, avoid_fd, &backups, &backup_count, &backup_capacity)) {
            int err = errno;
            if (err == 0) {
                err = ENOMEM;
            }
            runtimeError(vm, "exec: failed to prepare redirection for fd %d: %s",
                         target_fd, strerror(err));
            shellUpdateStatus(err ? err : 1);
            goto redir_error;
        }
        switch (redir->kind) {
            case SHELL_RUNTIME_REDIR_OPEN: {
                if (!redir->path) {
                    runtimeError(vm, "exec: missing redirection target");
                    shellUpdateStatus(1);
                    goto redir_error;
                }
#if defined(PSCAL_TARGET_IOS)
                char virtual_path[PATH_MAX];
                const char *open_path = redir->path;
                if (pathTruncateExpand(redir->path, virtual_path, sizeof(virtual_path))) {
                    open_path = virtual_path;
                }
#else
                const char *open_path = redir->path;
#endif
                int fd = open(open_path, redir->flags, redir->mode);
                if (fd < 0) {
                    int err = errno;
                    runtimeError(vm, "exec: %s: %s", redir->path, strerror(err));
                    shellUpdateStatus(err ? err : 1);
                    goto redir_error;
                }
                if (dup2(fd, target_fd) < 0) {
                    int err = errno;
                    runtimeError(vm, "exec: %s: %s", redir->path, strerror(err));
                    shellUpdateStatus(err ? err : 1);
                    close(fd);
                    goto redir_error;
                }
                shellResetStandardStream(target_fd);
                close(fd);
                if (target_fd == STDOUT_FILENO) {
                    stdout_redirected = true;
                }
                break;
            }
            case SHELL_RUNTIME_REDIR_DUP: {
                if (target_fd == STDOUT_FILENO && redir->dup_target_fd == STDOUT_FILENO && stdout_redirected) {
                    target_fd = STDERR_FILENO;
                }
                if (redir->close_target) {
                    if (close(target_fd) != 0 && errno != EBADF) {
                        int err = errno;
                        runtimeError(vm, "exec: failed to close fd %d: %s", target_fd, strerror(err));
                        shellUpdateStatus(err ? err : 1);
                        goto redir_error;
                    }
                    shellResetStandardStream(target_fd);
                } else {
                    if (redir->dup_target_fd < 0) {
                        runtimeError(vm, "exec: invalid file descriptor %d", redir->dup_target_fd);
                        shellUpdateStatus(1);
                        goto redir_error;
                    }
                    if (dup2(redir->dup_target_fd, target_fd) < 0) {
                        int err = errno;
                        runtimeError(vm, "exec: failed to duplicate fd %d: %s",
                                     redir->dup_target_fd, strerror(err));
                        shellUpdateStatus(err ? err : 1);
                        goto redir_error;
                    }
                    shellResetStandardStream(target_fd);
                }
                break;
            }
            case SHELL_RUNTIME_REDIR_HEREDOC: {
                int pipefd[2];
                if (pipe(pipefd) != 0) {
                    int err = errno;
                    runtimeError(vm, "exec: failed to create heredoc pipe: %s", strerror(err));
                    shellUpdateStatus(err ? err : 1);
                    goto redir_error;
                }
                const char *body = redir->here_doc ? redir->here_doc : "";
                size_t remaining = redir->here_doc_length;
                if (remaining == 0) {
                    remaining = strlen(body);
                }
                const char *cursor = body;
                while (remaining > 0) {
                    ssize_t written = write(pipefd[1], cursor, remaining);
                    if (written < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        int err = errno;
                        runtimeError(vm, "exec: failed to write heredoc: %s", strerror(err));
                        shellUpdateStatus(err ? err : 1);
                        close(pipefd[0]);
                        close(pipefd[1]);
                        goto redir_error;
                    }
                    cursor += written;
                    remaining -= (size_t)written;
                }
                close(pipefd[1]);
                if (dup2(pipefd[0], target_fd) < 0) {
                    int err = errno;
                    runtimeError(vm, "exec: failed to apply heredoc: %s", strerror(err));
                    shellUpdateStatus(err ? err : 1);
                    close(pipefd[0]);
                    goto redir_error;
                }
                shellResetStandardStream(target_fd);
                close(pipefd[0]);
                break;
            }
            case SHELL_RUNTIME_REDIR_HERE_STRING: {
                int pipefd[2];
                if (pipe(pipefd) != 0) {
                    int err = errno;
                    runtimeError(vm, "exec: failed to create here-string pipe: %s", strerror(err));
                    shellUpdateStatus(err ? err : 1);
                    goto redir_error;
                }
                const char *body = redir->here_string ? redir->here_string
                                                       : (redir->here_string_literal ? redir->here_string_literal : "");
                size_t remaining = redir->here_string_length;
                if (remaining == 0 && body) {
                    remaining = strlen(body);
                }
                const char *cursor = body;
                while (remaining > 0) {
                    ssize_t written = write(pipefd[1], cursor, remaining);
                    if (written < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        int err = errno;
                        runtimeError(vm, "exec: failed to write here-string: %s", strerror(err));
                        shellUpdateStatus(err ? err : 1);
                        close(pipefd[0]);
                        close(pipefd[1]);
                        goto redir_error;
                    }
                    cursor += written;
                    remaining -= (size_t)written;
                }
                while (true) {
                    ssize_t written = write(pipefd[1], "\n", 1);
                    if (written < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        int err = errno;
                        runtimeError(vm, "exec: failed to terminate here-string: %s", strerror(err));
                        shellUpdateStatus(err ? err : 1);
                        close(pipefd[0]);
                        close(pipefd[1]);
                        goto redir_error;
                    }
                    break;
                }
                close(pipefd[1]);
                if (dup2(pipefd[0], target_fd) < 0) {
                    int err = errno;
                    runtimeError(vm, "exec: failed to apply here-string: %s", strerror(err));
                    shellUpdateStatus(err ? err : 1);
                    close(pipefd[0]);
                    goto redir_error;
                }
                shellResetStandardStream(target_fd);
                close(pipefd[0]);
                break;
            }
            default:
                runtimeError(vm, "exec: unsupported redirection");
                shellUpdateStatus(1);
                goto redir_error;
        }
    }

    if (out_backups) {
        *out_backups = backups;
    } else {
        shellFreeExecRedirBackups(backups, backup_count);
    }
    if (out_count) {
        *out_count = backup_count;
    }
    return true;

redir_error:
    shellRestoreExecRedirections(backups, backup_count);
    shellFreeExecRedirBackups(backups, backup_count);
    if (out_backups) {
        *out_backups = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    return false;
}

#if defined(PSCAL_TARGET_IOS)
static bool shellPipelineWriteBufferToPipe(const char *data, size_t len, int fd) {
    size_t written = 0;
    while (written < len) {
        ssize_t res = write(fd, data + written, len - written);
        if (res < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += (size_t)res;
    }
    return true;
}

static bool __attribute__((unused)) shellPipelineReadPipeToBuffer(int fd, char **out_data, size_t *out_size) {
    const size_t kChunk = 4096;
    size_t capacity = 0;
    size_t size = 0;
    char *buffer = NULL;
    for (;;) {
        if (size + kChunk > capacity) {
            size_t new_capacity = capacity ? capacity * 2 : kChunk;
            char *new_buf = realloc(buffer, new_capacity);
            if (!new_buf) {
                free(buffer);
                return false;
            }
            buffer = new_buf;
            capacity = new_capacity;
        }
        ssize_t res = read(fd, buffer + size, capacity - size);
        if (res < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buffer);
            return false;
        }
        if (res == 0) {
            break;
        }
        size += (size_t)res;
    }

    if (!buffer) {
        buffer = (char *)calloc(1, 1);
        if (!buffer) {
            return false;
        }
    }
    *out_data = buffer;
    *out_size = size;
    return true;
}

typedef struct {
    int fd;
    int spool_fd;
    size_t size;
    size_t capacity;
    int err;
    char *buffer; /* only used when spool_fd < 0 */
} ShellPipelineCapture;

static bool shellPipelineCaptureGrow(ShellPipelineCapture *cap, size_t add) {
    if (cap->spool_fd >= 0) {
        return true;
    }
    size_t need = cap->size + add;
    if (need <= cap->capacity) {
        return true;
    }
    size_t new_cap = cap->capacity ? cap->capacity * 2 : 4096;
    while (new_cap < need) {
        new_cap *= 2;
    }
    char *new_buf = realloc(cap->buffer, new_cap);
    if (!new_buf) {
        return false;
    }
    cap->buffer = new_buf;
    cap->capacity = new_cap;
    return true;
}

static void *shellPipelineCaptureThread(void *arg) {
    ShellPipelineCapture *cap = (ShellPipelineCapture *)arg;
    const size_t chunk = 4096;
    char *tmp = malloc(chunk);
    if (!tmp) {
        cap->err = ENOMEM;
        close(cap->fd);
        return NULL;
    }
    for (;;) {
        ssize_t res = read(cap->fd, tmp, chunk);
        if (res < 0) {
            if (errno == EINTR) {
                continue;
            }
            cap->err = errno ? errno : EIO;
            break;
        }
        if (res == 0) {
            break;
        }
        if (!shellPipelineCaptureGrow(cap, (size_t)res)) {
            cap->err = ENOMEM;
            break;
        }
        if (cap->spool_fd >= 0) {
            ssize_t written = write(cap->spool_fd, tmp, (size_t)res);
            if (written < 0) {
                cap->err = errno ? errno : EIO;
                break;
            }
            cap->size += (size_t)written;
        } else {
            memcpy(cap->buffer + cap->size, tmp, (size_t)res);
            cap->size += (size_t)res;
        }
    }
    free(tmp);
    close(cap->fd);
    cap->fd = -1;
    return NULL;
}

typedef struct {
    const char *data;
    size_t len;
    int fd;
    int src_fd;
    int err;
} ShellPipelineFeed;

static void *shellPipelineFeedThread(void *arg) {
    ShellPipelineFeed *feed = (ShellPipelineFeed *)arg;
    const size_t chunk = 4096;
    if (feed->src_fd >= 0) {
        char buf[4096];
        for (;;) {
            ssize_t r = read(feed->src_fd, buf, sizeof(buf));
            if (r < 0) {
                if (errno == EINTR) continue;
                feed->err = errno ? errno : EIO;
                break;
            }
            if (r == 0) {
                break;
            }
            size_t off = 0;
            while (off < (size_t)r) {
                ssize_t w = write(feed->fd, buf + off, (size_t)r - off);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    feed->err = errno ? errno : EIO;
                    off = (size_t)r;
                    break;
                }
                off += (size_t)w;
            }
        }
        close(feed->src_fd);
        feed->src_fd = -1;
    } else {
        size_t offset = 0;
        while (offset < feed->len) {
            size_t to_write = feed->len - offset;
            if (to_write > chunk) {
                to_write = chunk;
            }
            ssize_t res = write(feed->fd, feed->data + offset, to_write);
            if (res < 0) {
                if (errno == EINTR) {
                    continue;
                }
                feed->err = errno ? errno : EIO;
                break;
            }
            offset += (size_t)res;
        }
    }
    close(feed->fd);
    feed->fd = -1;
    return NULL;
}

static int shellPipelineCreateSpoolFile(void) {
    const char *tmp_root = getenv("TMPDIR");
    if (!tmp_root || !*tmp_root) {
        tmp_root = "/tmp";
    }
    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof(tmpl), "%s/pscal-pipe-%ld-XXXXXX",
             tmp_root, (long)getpid());
    int fd = mkstemp(tmpl);
    if (fd >= 0) {
        unlink(tmpl);
    }
    return fd;
}

static int shellRunBuiltinInProcess(VM *vm,
                                    ShellCommand *cmd,
                                    int stdin_fd,
                                    int stdout_fd,
                                    int stderr_fd,
                                    bool apply_exec_redirs) {
    ShellExecRedirBackup *backups = NULL;
    size_t backup_count = 0;
    size_t backup_capacity = 0;
    int status = 127;
    bool applied_redirs = false;

    if (stdin_fd >= 0) {
        if (!shellEnsureExecRedirBackup(STDIN_FILENO, -1, &backups, &backup_count, &backup_capacity)) {
            goto cleanup;
        }
        if (dup2(stdin_fd, STDIN_FILENO) < 0) {
            goto cleanup;
        }
        shellResetStandardStream(STDIN_FILENO);
    }
    if (stdout_fd >= 0) {
        if (!shellEnsureExecRedirBackup(STDOUT_FILENO, -1, &backups, &backup_count, &backup_capacity)) {
            goto cleanup;
        }
        if (dup2(stdout_fd, STDOUT_FILENO) < 0) {
            goto cleanup;
        }
        shellResetStandardStream(STDOUT_FILENO);
    }
    if (stderr_fd >= 0) {
        if (!shellEnsureExecRedirBackup(STDERR_FILENO, -1, &backups, &backup_count, &backup_capacity)) {
            goto cleanup;
        }
        if (dup2(stderr_fd, STDERR_FILENO) < 0) {
            goto cleanup;
        }
        shellResetStandardStream(STDERR_FILENO);
    }

    if (apply_exec_redirs) {
        fprintf(stderr, "[pipeline-bg] apply redirs count=%zu\n", cmd ? cmd->redir_count : 0);
        if (!shellApplyExecRedirections(vm, cmd, &backups, &backup_count)) {
            goto cleanup;
        }
        applied_redirs = true;
    }

    bool builtin_ran = shellInvokeBuiltin(vm, cmd);
#if defined(PSCAL_TARGET_IOS)
    if (!builtin_ran) {
        builtin_ran = shellDispatchIOSFrontendBuiltin(vm, cmd);
    }
#endif
#if defined(PSCAL_TARGET_IOS)
    if (!builtin_ran) {
        /* Try tool-runner execution for shebang-wrapped scripts even in
         * pipeline mode. This mirrors the exec path but stays in-process.
         * Save and clear pipeline state so nested shebang execution does
         * not stomp the outer pipeline bookkeeping. */
        bool was_background = gShellRuntime.pipeline.background;
        ShellPipelineContext saved_pipeline = gShellRuntime.pipeline;
        memset(&gShellRuntime.pipeline, 0, sizeof(gShellRuntime.pipeline));
        if (was_background && saved_pipeline.stage_count == 1) {
            /* Background shebang: run in a detached thread so the shell prompt
             * can return immediately when in-process execution is required. */
            fprintf(stderr, "[shebang-bg] cloning argv for '%s' (argc=%zu)\n",
                    cmd->argv[0] ? cmd->argv[0] : "<null>",
                    cmd->argc);
            ShellShebangThreadCtx *bg_ctx =
                (ShellShebangThreadCtx *)calloc(1, sizeof(ShellShebangThreadCtx));
            if (bg_ctx) {
                bg_ctx->argc = cmd->argc;
                bg_ctx->argv = (char **)calloc((size_t)bg_ctx->argc, sizeof(char *));
                if (bg_ctx->argv) {
                    bool ok = true;
                    for (int i = 0; i < bg_ctx->argc; ++i) {
                        if (cmd->argv[i]) {
                            bg_ctx->argv[i] = strdup(cmd->argv[i]);
                            if (!bg_ctx->argv[i]) {
                                ok = false;
                                break;
                            }
                        }
                    }
                    if (!ok) {
                        shellShebangThreadCtxFree(bg_ctx);
                        bg_ctx = NULL;
                    }
                } else {
                    shellShebangThreadCtxFree(bg_ctx);
                    bg_ctx = NULL;
                }
            }
            if (bg_ctx) {
                pthread_t tid;
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_setstacksize(&attr, SHELL_TOOL_THREAD_STACK_SZ);
                if (pthread_create(&tid, &attr, shellShebangBackgroundMain, bg_ctx) == 0) {
                    pthread_detach(tid);
                    pthread_attr_destroy(&attr);
                    fprintf(stderr, "[shebang-bg] detached thread launched\n");
                    shellUpdateStatus(0);
                    gShellRuntime.pipeline = saved_pipeline;
                    shellResetPipeline();
                    return 0;
                }
                pthread_attr_destroy(&attr);
                fprintf(stderr, "[shebang-bg] failed to spawn thread, falling back inline\n");
                shellShebangThreadCtxFree(bg_ctx);
            }
        }
        int shebang_status = shellMaybeExecShebangTool(cmd->argv[0], cmd->argv);
        gShellRuntime.pipeline = saved_pipeline;
        if (shebang_status >= 0) {
            shellUpdateStatus(shebang_status);
            builtin_ran = true;
        } else {
            /* Fall back to smallclue applets executed in-process. */
            const SmallclueApplet *applet = smallclueFindApplet(cmd->argv[0]);
            if (applet) {
                int argc = cmd->argc > 0 ? cmd->argc : 0;
                char **argv_copy = (char **)calloc((size_t)(argc + 1), sizeof(char *));
                if (argv_copy) {
                    bool ok = true;
                    for (int i = 0; i < argc; ++i) {
                        if (cmd->argv[i]) {
                            argv_copy[i] = strdup(cmd->argv[i]);
                            if (!argv_copy[i]) {
                                ok = false;
                                break;
                            }
                        }
                    }
                    if (ok) {
                        int sc_status = smallclueDispatchApplet(applet, argc, argv_copy);
                        shellUpdateStatus(sc_status);
                        builtin_ran = true;
                    }
                    for (int i = 0; i < argc; ++i) {
                        free(argv_copy[i]);
                    }
                    free(argv_copy);
                }
            }
        }
    }
#endif
    if (!builtin_ran) {
        runtimeError(vm, "%s: command unavailable in pipeline", cmd->argv[0]);
        shellUpdateStatus(127);
    }
    shellFlushStandardStreams();
    status = gShellRuntime.last_status;

cleanup:
    if (applied_redirs) {
        shellRestoreExecRedirections(backups, backup_count);
    } else if (backups) {
        shellRestoreExecRedirections(backups, backup_count);
    }
    shellFreeExecRedirBackups(backups, backup_count);
    return status;
}

static bool shellPipelineExecuteStageInProcess(VM *vm,
                                               ShellPipelineContext *ctx,
                                               ShellCommand *cmd,
                                               size_t stage_pos,
                                               bool pipeline_head,
                                               bool pipeline_tail) {
    if (!ctx || !ctx->active || !ctx->pids || stage_pos >= ctx->stage_count) {
        runtimeError(vm, "pipeline: invalid context");
        shellUpdateStatus(1);
        if (ctx) {
            shellAbortPipeline();
        }
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    fprintf(stderr,
            "[pipeline-bg] stage entry head=%d tail=%d ctx_active=%d ctx_bg=%d cmd_bg=%d stages=%zu pos=%zu\n",
            (int)pipeline_head, (int)pipeline_tail,
            (int)ctx->active, (int)ctx->background,
            (int)cmd->background, ctx->stage_count, stage_pos);
    if (pipeline_tail && ctx->background && ctx->stage_count == 1) {
        fprintf(stderr, "[pipeline-bg] attempt tail=%d ctx_bg=%d cmd_bg=%d stages=%zu\n",
                (int)pipeline_tail, (int)ctx->background, (int)cmd->background, ctx->stage_count);
        ShellBackgroundStageCtx *bg_ctx =
            (ShellBackgroundStageCtx *)calloc(1, sizeof(ShellBackgroundStageCtx));
        ShellCommand *cloned = shellCloneCommandForBackground(cmd);
        if (!bg_ctx || !cloned) {
            free(bg_ctx);
            shellFreeCommand(cloned);
            free(cloned);
            runtimeError(vm, "pipeline: unable to allocate background context");
            shellUpdateStatus(1);
            shellResetPipeline();
            shellRuntimeProcessPendingSignals();
            return true;
        }
        bg_ctx->vm = vm;
        bg_ctx->cmd = cloned;
        bg_ctx->synthetic_pid = vprocReservePid();

        if (ctx->pids && stage_pos < ctx->stage_count) {
            ctx->pids[stage_pos] = bg_ctx->synthetic_pid;
            ctx->pgid = bg_ctx->synthetic_pid;
            ctx->launched = stage_pos + 1;
        }

        /* Reset pipeline on the main thread before detaching so prompt can return. */
        ctx->last_status = 0;
        shellUpdateStatus(0);
        shellRuntimeProcessPendingSignals();

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, SHELL_TOOL_THREAD_STACK_SZ);
        if (pthread_create(&tid, &attr, shellInProcessBackgroundMain, bg_ctx) == 0) {
            pthread_detach(tid);
            pthread_attr_destroy(&attr);
            if (bg_ctx->synthetic_pid > 0) {
                ShellJob *job = shellRegisterJob(bg_ctx->synthetic_pid, ctx->pids, ctx->launched, cmd);
                if (job) {
                    job->running = true;
                    job->stopped = false;
                    job->last_status = 0;
                }
            }
            shellResetPipeline();
            return true;
        }
        pthread_attr_destroy(&attr);
        shellFreeCommand(cloned);
        free(cloned);
        free(bg_ctx);
        runtimeError(vm, "pipeline: failed to launch background worker");
        shellUpdateStatus(1);
        shellResetPipeline();
        return true;
    }
#endif
    int stdin_fd = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;
    int input_pipe[2] = { -1, -1 };
    int output_pipe[2] = { -1, -1 };
    bool ok = false;
    pthread_t feed_thread;
    bool feed_thread_started = false;
    ShellPipelineFeed feed = { .data = NULL, .len = 0, .fd = -1, .src_fd = -1, .err = 0 };
    pthread_t capture_thread;
    bool capture_thread_started = false;
    ShellPipelineCapture capture = { .fd = -1, .spool_fd = -1, .size = 0, .capacity = 0, .err = 0, .buffer = NULL };
#if defined(PSCAL_TARGET_IOS)
    bool pushed_virtual_tty = false;
    VProc *vp = NULL;
#endif
    bool need_spool = !pipeline_tail;
    int status = 0;

    if (!pipeline_head) {
        if (ctx->spool_fd < 0) {
            runtimeError(vm, "pipeline: missing intermediate buffer");
            shellUpdateStatus(1);
            return false;
        }
        lseek(ctx->spool_fd, 0, SEEK_SET);
        bool pager_tail = pipeline_tail &&
                          cmd->argc > 0 &&
                          (strcmp(cmd->argv[0], "less") == 0 || strcmp(cmd->argv[0], "more") == 0);
        if (pager_tail) {
            stdin_fd = dup(ctx->spool_fd);
            if (stdin_fd < 0) {
                runtimeError(vm, "pipeline: failed to reopen pager buffer: %s", strerror(errno));
                shellUpdateStatus(errno ? errno : 1);
                return false;
            }
            lseek(stdin_fd, 0, SEEK_SET);
        } else {
            if (pipe(input_pipe) != 0) {
                runtimeError(vm, "pipeline: failed to allocate input buffer: %s", strerror(errno));
                shellUpdateStatus(errno ? errno : 1);
                return false;
            }
            feed.src_fd = dup(ctx->spool_fd);
            if (feed.src_fd < 0) {
                runtimeError(vm, "pipeline: failed to duplicate buffer fd: %s", strerror(errno));
                shellUpdateStatus(errno ? errno : 1);
                return false;
            }
            lseek(feed.src_fd, 0, SEEK_SET);
            feed.fd = input_pipe[1];
            input_pipe[1] = -1;
            if (pthread_create(&feed_thread, NULL, shellPipelineFeedThread, &feed) == 0) {
                feed_thread_started = true;
            } else {
                runtimeError(vm, "pipeline: failed to start feeder");
                shellUpdateStatus(1);
                goto cleanup;
            }
            stdin_fd = input_pipe[0];
            input_pipe[0] = -1;
        }
    }

    if (need_spool) {
        if (pipe(output_pipe) != 0) {
            runtimeError(vm, "pipeline: unable to allocate buffer: %s", strerror(errno));
            shellUpdateStatus(errno ? errno : 1);
            goto cleanup;
        }
        stdout_fd = output_pipe[1];
        capture.spool_fd = shellPipelineCreateSpoolFile();
        if (capture.spool_fd < 0) {
            runtimeError(vm, "pipeline: failed to allocate spool storage: %s", strerror(errno));
            shellUpdateStatus(errno ? errno : 1);
            goto cleanup;
        }
    }

    if (ctx->merge_stderr && stage_pos < ctx->stage_count && ctx->merge_stderr[stage_pos] && stdout_fd >= 0) {
        stderr_fd = stdout_fd;
    }

#if defined(PSCAL_TARGET_IOS)
    if (stdin_fd >= 0 && !pushed_virtual_tty) {
        int dup_fd = dup(STDIN_FILENO);
        if (dup_fd >= 0) {
            shellPagerPushVirtualTTYOverride(dup_fd);
            pushed_virtual_tty = true;
        }
    }
#endif

    bool apply_exec_redirs = !(ctx->stage_count > 1);
    if (need_spool && output_pipe[0] >= 0) {
        capture.fd = output_pipe[0];
        output_pipe[0] = -1;
        if (pthread_create(&capture_thread, NULL, shellPipelineCaptureThread, &capture) == 0) {
            capture_thread_started = true;
        } else {
            runtimeError(vm, "pipeline: failed to start capture thread");
            shellUpdateStatus(1);
            goto cleanup;
        }
    }

#if defined(PSCAL_TARGET_IOS)
    vp = shellCreateVProcForStage(stdin_fd,
                                  stdout_fd,
                                  stderr_fd,
                                  pscalRuntimeDetectWindowCols(),
                                  pscalRuntimeDetectWindowRows(),
                                  -1);
    if (!vp) {
        runtimeError(vm, "pipeline: failed to allocate vproc");
        shellUpdateStatus(1);
        goto cleanup;
    }
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
#endif

    status = shellRunBuiltinInProcess(vm, cmd, stdin_fd, stdout_fd, stderr_fd, apply_exec_redirs);
    ctx->last_status = status;
    bool ctx_valid = ctx->active && ctx->pids && stage_pos < ctx->stage_count;
    if (ctx_valid) {
        ctx->pids[stage_pos] = 0;
#if defined(PSCAL_TARGET_IOS)
        if (vp) {
            ctx->pids[stage_pos] = vprocPid(vp);
        }
        ctx->launched = stage_pos + 1;
#else
        ctx->launched = stage_pos + 1;
#endif
    }
    ok = true;

cleanup:
#if defined(PSCAL_TARGET_IOS)
    vprocDeactivate();
    if (vp) {
        vprocMarkExit(vp, status);
        vprocDestroy(vp);
    }
#endif
    if (stdin_fd >= 0) close(stdin_fd);
    if (input_pipe[0] >= 0) close(input_pipe[0]);
    if (input_pipe[1] >= 0) close(input_pipe[1]);
    if (stdout_fd >= 0) {
        close(stdout_fd);
        if (output_pipe[1] == stdout_fd) {
            output_pipe[1] = -1;
        }
        stdout_fd = -1;
    }
#if defined(PSCAL_TARGET_IOS)
    if (pushed_virtual_tty) {
        shellPagerPopVirtualTTYOverride();
        pushed_virtual_tty = false;
    }
#endif
    if (capture_thread_started) {
        pthread_join(capture_thread, NULL);
        capture_thread_started = false;
        if (capture.err != 0) {
            ok = false;
            runtimeError(vm, "pipeline: failed to capture output: %s", strerror(capture.err));
            shellUpdateStatus(capture.err);
        } else if (ok && need_spool) {
            if (ctx->spool_fd >= 0) {
                close(ctx->spool_fd);
            }
            ctx->spool_fd = capture.spool_fd;
            ctx->spool_size = capture.size;
            if (ctx->spool_fd >= 0) {
                lseek(ctx->spool_fd, 0, SEEK_SET);
            }
            capture.spool_fd = -1;
        }
    }
    if (capture.buffer) {
        free(capture.buffer);
        capture.buffer = NULL;
    }
    if (capture.spool_fd >= 0) {
        close(capture.spool_fd);
        capture.spool_fd = -1;
    }

    if (feed_thread_started) {
        pthread_join(feed_thread, NULL);
        feed_thread_started = false;
        if (feed.err != 0) {
            ok = false;
            runtimeError(vm, "pipeline: failed to feed input: %s", strerror(feed.err));
            shellUpdateStatus(feed.err);
        }
    }

    if (!pipeline_head && ctx->spool_fd >= 0) {
        close(ctx->spool_fd);
        ctx->spool_fd = -1;
        ctx->spool_size = 0;
    }

    if (need_spool && output_pipe[1] >= 0) {
        close(output_pipe[1]);
        output_pipe[1] = -1;
    }

    if (!feed_thread_started && feed.fd >= 0) {
        close(feed.fd);
        feed.fd = -1;
    }

    if (ok && pipeline_tail) {
        ctx->background = cmd->background;
        shellFinishPipeline(cmd);
        shellRuntimeProcessPendingSignals();
    }

    return ok;
}
#endif /* PSCAL_TARGET_IOS */

static bool shellExecuteExecBuiltin(VM *vm, ShellCommand *cmd) {
    if (!shellCommandIsExecBuiltin(cmd)) {
        return false;
    }
    if (cmd->background) {
        runtimeError(vm, "exec: cannot be used in background");
        shellUpdateStatus(1);
        return true;
    }

    if (cmd->argc <= 1) {
        ShellExecRedirBackup *backups = NULL;
        size_t backup_count = 0;
        if (!shellApplyExecRedirections(vm, cmd, &backups, &backup_count)) {
            return true;
        }
        shellFreeExecRedirBackups(backups, backup_count);
        shellUpdateStatus(0);
        return true;
    }

    ShellExecRedirBackup *backups = NULL;
    size_t backup_count = 0;
    if (!shellApplyExecRedirections(vm, cmd, &backups, &backup_count)) {
        return true;
    }

    char **argv = &cmd->argv[1];
    if (!argv || !argv[0] || argv[0][0] == '\0') {
        runtimeError(vm, "exec: expected command");
        shellRestoreExecRedirections(backups, backup_count);
        shellFreeExecRedirBackups(backups, backup_count);
        shellUpdateStatus(1);
        return true;
    }

    execvp(argv[0], argv);
    int err = errno;
    runtimeError(vm, "exec: %s: %s", argv[0], strerror(err));
    shellRestoreExecRedirections(backups, backup_count);
    shellFreeExecRedirBackups(backups, backup_count);
    shellUpdateStatus((err == ENOENT) ? 127 : 126);
    return true;
}

static Value shellExecuteCommand(VM *vm, ShellCommand *cmd) {
    if (!cmd) {
        shellScalarCacheInvalidate();
        return makeVoid();
    }
    shellRuntimeProcessPendingSignals();
    if (shellLoopSkipActive()) {
        shellFreeCommand(cmd);
        shellScalarCacheInvalidate();
        return makeVoid();
    }
    if (!shellExpandAliasesIfNeeded(vm, cmd)) {
        shellUpdateStatus(1);
        shellFreeCommand(cmd);
        shellScalarCacheInvalidate();
        return makeVoid();
    }
    shellPatchCombinedRedirectTokens(cmd);
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
#if defined(PSCAL_TARGET_IOS)
    if (!ctx->active && cmd->background) {
        if (shellEnsurePipeline(1, false)) {
            ctx->active = true;
            ctx->in_process = true;
            ctx->stage_count = 1;
            ctx->launched = 0;
            ctx->background = true;
            cmd->pipeline_index = 0;
            cmd->is_pipeline_head = true;
            cmd->is_pipeline_tail = true;
            fprintf(stderr, "[pipeline-bg] auto in-process pipeline for background command '%s'\n",
                    (cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<null>");
        } else {
            runtimeError(vm, "pipeline: unable to allocate background context");
            shellUpdateStatus(1);
            shellFreeCommand(cmd);
            shellScalarCacheInvalidate();
            return makeVoid();
        }
    }
#endif
    bool pipeline_head = cmd->is_pipeline_head;
    bool pipeline_tail = cmd->is_pipeline_tail;
    size_t stage_pos = 0;
    if (ctx->active) {
        size_t stage_count = ctx->stage_count ? ctx->stage_count : 1;
        if (cmd->pipeline_index >= 0) {
            stage_pos = (size_t)cmd->pipeline_index;
        } else {
            stage_pos = ctx->launched;
        }
        if (stage_pos >= stage_count) {
            stage_pos = stage_count - 1;
        }
        pipeline_head = (stage_pos == 0);
        pipeline_tail = ((stage_pos + 1) >= stage_count);
    }
    ShellAssignmentBackup *assignment_backups = NULL;
    size_t assignment_backup_count = 0;
    bool assignments_applied = false;

    if (cmd->argc == 0) {
        const char *failed_assignment = NULL;
        bool invalid_assignment = false;
        if (cmd->assignment_count > 0) {
            if (!shellApplyAssignmentsPermanently(cmd, &failed_assignment, &invalid_assignment)) {
                if (invalid_assignment) {
                    runtimeError(vm, "shell exec: invalid assignment '%s'",
                                 failed_assignment ? failed_assignment : "<assignment>");
                    shellUpdateStatus(1);
                } else {
                    runtimeError(vm, "shell exec: failed to apply assignment '%s': %s",
                                 failed_assignment ? failed_assignment : "<assignment>",
                                 strerror(errno));
                    shellUpdateStatus(errno ? errno : 1);
                }
            } else {
                shellUpdateStatus(0);
            }
        } else {
            shellUpdateStatus(0);
        }
        if (ctx->active) {
            int status = gShellRuntime.last_status;
            if (ctx->stage_count <= 1 && ctx->negated) {
                status = (status == 0) ? 1 : 0;
                shellUpdateStatus(status);
            }
            ctx->last_status = status;
            if (ctx->stage_count <= 1) {
                shellResetPipeline();
            }
        }
        shellFreeCommand(cmd);
        shellScalarCacheInvalidate();
        return makeVoid();
    }

    const char *failed_assignment = NULL;
    bool invalid_assignment = false;
    if (cmd->assignment_count > 0) {
        if (!shellApplyAssignmentsTemporary(cmd, &assignment_backups, &assignment_backup_count,
                                            &failed_assignment, &invalid_assignment)) {
            if (invalid_assignment) {
                runtimeError(vm, "shell exec: invalid assignment '%s'",
                             failed_assignment ? failed_assignment : "<assignment>");
                shellUpdateStatus(1);
            } else {
                runtimeError(vm, "shell exec: failed to apply assignment '%s': %s",
                             failed_assignment ? failed_assignment : "<assignment>",
                             strerror(errno));
                shellUpdateStatus(errno ? errno : 1);
            }
            if (ctx->active) {
                shellAbortPipeline();
            }
            shellFreeCommand(cmd);
            shellScalarCacheInvalidate();
            return makeVoid();
        }
        assignments_applied = true;
    }

    int stdin_fd = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;
    if (ctx->active) {
        if (ctx->stage_count == 1 && shellCommandIsExecBuiltin(cmd)) {
            shellExecuteExecBuiltin(vm, cmd);
            if (assignments_applied) {
                shellRestoreAssignments(assignment_backups, assignment_backup_count);
                assignments_applied = false;
                assignment_backups = NULL;
                assignment_backup_count = 0;
            }
            int status = gShellRuntime.last_status;
            if (ctx->negated) {
                status = (status == 0) ? 1 : 0;
                shellUpdateStatus(status);
            }
            ctx->last_status = status;
            shellResetPipeline();
            shellFreeCommand(cmd);
            shellScalarCacheInvalidate();
            return makeVoid();
        }
        if (ctx->stage_count == 1 && !ctx->background && !cmd->background &&
            shellInvokeBuiltin(vm, cmd)) {
            if (assignments_applied) {
                shellRestoreAssignments(assignment_backups, assignment_backup_count);
                assignments_applied = false;
                assignment_backups = NULL;
                assignment_backup_count = 0;
            }
            int status = gShellRuntime.last_status;
            if (ctx->negated) {
                status = (status == 0) ? 1 : 0;
                shellUpdateStatus(status);
            }
            ctx->last_status = status;
            shellResetPipeline();
            shellFreeCommand(cmd);
            shellScalarCacheInvalidate();
            return makeVoid();
        }
        size_t idx = stage_pos;
        if (idx >= ctx->stage_count) {
            if (assignments_applied) {
                shellRestoreAssignments(assignment_backups, assignment_backup_count);
            }
            assignment_backups = NULL;
            assignment_backup_count = 0;
            runtimeError(vm, "shell exec: pipeline index out of range");
            shellFreeCommand(cmd);
            shellResetPipeline();
            shellScalarCacheInvalidate();
            return makeVoid();
        }
        bool pipeline_uses_pipes = true;
#if defined(PSCAL_TARGET_IOS)
        pipeline_uses_pipes = !ctx->in_process;
#endif
        if (pipeline_uses_pipes && ctx->stage_count > 1) {
            if (!pipeline_head) {
                stdin_fd = ctx->pipes[idx - 1][0];
            }
            if (!pipeline_tail) {
                stdout_fd = ctx->pipes[idx][1];
            }
        }
        if (pipeline_uses_pipes &&
            ctx->merge_stderr && idx < ctx->stage_count && ctx->merge_stderr[idx]) {
            stderr_fd = stdout_fd;
        }
#if defined(PSCAL_TARGET_IOS)
        if (cmd->background) {
            ctx->background = true;
        }
        if (ctx->in_process) {
            bool stage_ok = shellPipelineExecuteStageInProcess(vm,
                                                               ctx,
                                                               cmd,
                                                               stage_pos,
                                                               pipeline_head,
                                                               pipeline_tail);
            if (assignments_applied) {
                shellRestoreAssignments(assignment_backups, assignment_backup_count);
                assignments_applied = false;
                assignment_backups = NULL;
                assignment_backup_count = 0;
            }
            if (!stage_ok) {
                shellAbortPipeline();
            } else if (pipeline_tail) {
                int status = ctx->last_status;
                if (ctx->negated) {
                    status = (status == 0) ? 1 : 0;
                    shellUpdateStatus(status);
                }
                ctx->last_status = status;
                shellResetPipeline();
            }
            shellFreeCommand(cmd);
            shellScalarCacheInvalidate();
            return makeVoid();
        }
#endif
    } else {
        if (shellCommandIsExecBuiltin(cmd)) {
            shellExecuteExecBuiltin(vm, cmd);
            if (assignments_applied) {
                shellRestoreAssignments(assignment_backups, assignment_backup_count);
                assignments_applied = false;
                assignment_backups = NULL;
                assignment_backup_count = 0;
            }
            shellFreeCommand(cmd);
            shellScalarCacheInvalidate();
            return makeVoid();
        }
        if (shellInvokeBuiltin(vm, cmd)) {
            if (assignments_applied) {
                shellRestoreAssignments(assignment_backups, assignment_backup_count);
                assignments_applied = false;
                assignment_backups = NULL;
                assignment_backup_count = 0;
            }
            shellFreeCommand(cmd);
            shellScalarCacheInvalidate();
            return makeVoid();
        }
    }

    bool background_execution = cmd->background;
    if (ctx->active) {
        if (ctx->background) {
            background_execution = true;
        }
        if (cmd->background) {
            ctx->background = true;
        }
    }

    pid_t child = -1;
    if (cmd->argv && cmd->argv[0] && cmd->argv[0][0] != '\0') {
        shellHashRecordExecution(cmd->argv[0]);
    }
    bool ignore_job_signals = background_execution &&
                              (!gShellRuntime.job_control_enabled
#if defined(PSCAL_TARGET_IOS)
                               || pscalRuntimeVirtualTTYEnabled()
#endif
                               );

    int spawn_err = shellSpawnProcess(vm,
                                      cmd,
                                      stdin_fd,
                                      stdout_fd,
                                      stderr_fd,
                                      &child,
                                      ignore_job_signals);
    if (assignments_applied) {
        shellRestoreAssignments(assignment_backups, assignment_backup_count);
        assignments_applied = false;
        assignment_backups = NULL;
        assignment_backup_count = 0;
    }
    if (spawn_err != 0) {
        runtimeError(vm, "shell exec: failed to spawn '%s': %s", cmd->argv[0], strerror(spawn_err));
        if (ctx->active) {
            shellAbortPipeline();
        }
        shellFreeCommand(cmd);
        shellUpdateStatus(127);
        shellScalarCacheInvalidate();
        return makeVoid();
    }

    if (ctx->active) {
        pid_t target_pgid = (ctx->pgid > 0) ? ctx->pgid : child;
        if (setpgid(child, target_pgid) != 0) {
            if (errno != EACCES && errno != ESRCH) {
                /* best-effort: ignore errors from lack of job control */
            }
        }
        if (ctx->pgid <= 0) {
            ctx->pgid = target_pgid;
        }
    } else {
        if (setpgid(child, child) != 0) {
            if (errno != EACCES && errno != ESRCH) {
                /* ignore */
            }
        }
    }

    if (ctx->active) {
        if (!pipeline_head && stdin_fd >= 0) {
            close(stdin_fd);
            if (cmd->pipeline_index > 0) {
                ctx->pipes[cmd->pipeline_index - 1][0] = -1;
            }
        }
        if (!pipeline_tail && stdout_fd >= 0) {
            close(stdout_fd);
            ctx->pipes[cmd->pipeline_index][1] = -1;
        }
        ctx->pids[ctx->launched++] = child;
        if (pipeline_tail) {
            ctx->background = cmd->background;
            shellFinishPipeline(cmd);
            shellRuntimeProcessPendingSignals();
        }
    } else {
        int status = 0;
        if (!cmd->background) {
            shellEnsureJobControl();
            bool job_control = gShellRuntime.job_control_enabled;
            bool stopped = false;
            if (job_control) {
                shellJobControlSetForeground(child);
            }
            shellWaitPid(child, &status, job_control, &stopped);
            if (job_control) {
                shellJobControlRestoreForeground();
            }
            if (!cmd->background && status >= 128 && status < 128 + NSIG) {
                shellHandlePendingSignal(status - 128);
            }
            shellRuntimeProcessPendingSignals();
            if (stopped && job_control) {
                pid_t job_pids[1];
                job_pids[0] = child;
                ShellJob *job = shellRegisterJob(child, job_pids, 1, cmd);
                if (job) {
                    job->stopped = true;
                    job->running = false;
                    job->last_status = status;
                }
                shellUpdateStatus(status);
                shellFreeCommand(cmd);
                shellScalarCacheInvalidate();
                return makeVoid();
            }
        } else {
            pid_t job_pids[1];
            job_pids[0] = child;
            ShellJob *job = shellRegisterJob(child, job_pids, 1, cmd);
            if (job) {
                job->running = true;
                job->stopped = false;
                job->last_status = 0;
            }
            status = 0;
        }
        shellUpdateStatus(status);
        shellRuntimeProcessPendingSignals();
    }

    shellFreeCommand(cmd);
    shellScalarCacheInvalidate();
    return makeVoid();
}
