/* Auto-generated include: command execution and pipeline management. Included from shell.c. */
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
                fflush(NULL);
                _exit(status);
            }

            execvp(cmd->argv[0], cmd->argv);
            int err = errno;
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

static int shellWaitPid(pid_t pid, int *status_out, bool allow_stop, bool *out_stopped) {
    if (out_stopped) {
        *out_stopped = false;
    }
    int status = 0;
    int options = allow_stop ? WUNTRACED : 0;
    pid_t waited;
    bool sent_terminate = false;
    while (true) {
        waited = waitpid(pid, &status, options);
        if (waited >= 0) {
            break;
        }
        if (errno != EINTR) {
            return errno;
        }
        shellRuntimeProcessPendingSignals();
        if (gShellExitRequested || gShellRuntime.break_requested) {
            if (!sent_terminate) {
                kill(pid, SIGTERM);
                sent_terminate = true;
            }
            return EINTR;
        }
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
    free(ctx->merge_stderr);
    ctx->merge_stderr = NULL;
    ctx->active = false;
    ctx->stage_count = 0;
    ctx->launched = 0;
    ctx->background = false;
    ctx->last_status = 0;
    ctx->pgid = -1;
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
    ctx->pgid = -1;
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
    pid_t job_pgid = (ctx->pgid > 0) ? ctx->pgid : ((ctx->launched > 0) ? ctx->pids[0] : -1);

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
                FILE *log = fopen("/tmp/exsh_pipeline.log", "a");
                if (log) {
                    fprintf(log, "pipeline kill pid=%d pgid=%d\n", (int)pid, (int)ctx->pgid);
                    fclose(log);
                }
                if (ctx->pgid > 0) {
                    kill(-ctx->pgid, SIGTERM);
                } else {
                    kill(pid, SIGTERM);
                }
                continue;
            }
            bool stopped = false;
            int status = 0;
            int err = shellWaitPid(pid, &status, job_control, &stopped);
            if (err == EINTR && (gShellExitRequested || gShellRuntime.break_requested)) {
                ctx->pids[i] = -1;
                continue;
            }
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
            ShellJob *job = shellRegisterJob(job_pgid, ctx->pids, ctx->launched, tail_cmd);
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
        ShellJob *job = shellRegisterJob(job_pgid, ctx->pids, ctx->launched, tail_cmd);
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

static void shellRestoreExecRedirections(ShellExecRedirBackup *backups, size_t count) {
    if (!backups) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        ShellExecRedirBackup *backup = &backups[i];
        if (backup->saved_valid && backup->saved_fd >= 0) {
            dup2(backup->saved_fd, backup->target_fd);
        } else if (backup->was_closed) {
            close(backup->target_fd);
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
                int fd = open(redir->path, redir->flags, redir->mode);
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
        return makeVoid();
    }
    shellRuntimeProcessPendingSignals();
    if (gShellExitRequested || gShellRuntime.break_requested) {
        shellFreeCommand(cmd);
        return makeVoid();
    }
    if (shellLoopSkipActive()) {
        shellFreeCommand(cmd);
        return makeVoid();
    }
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    bool pipeline_head = cmd->is_pipeline_head;
    bool pipeline_tail = cmd->is_pipeline_tail;
    if (ctx->active && cmd->pipeline_index >= 0) {
        size_t stage_count = ctx->stage_count;
        int index = cmd->pipeline_index;
        if (index >= 0 && (size_t)index < stage_count) {
            pipeline_head = (index == 0);
            pipeline_tail = ((size_t)index + 1 == stage_count);
        }
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
            return makeVoid();
        }
        if (ctx->stage_count == 1 && shellInvokeBuiltin(vm, cmd)) {
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
            return makeVoid();
        }
        size_t idx = (size_t)cmd->pipeline_index;
        if (idx >= ctx->stage_count) {
            if (assignments_applied) {
                shellRestoreAssignments(assignment_backups, assignment_backup_count);
            }
            assignment_backups = NULL;
            assignment_backup_count = 0;
            runtimeError(vm, "shell exec: pipeline index out of range");
            shellFreeCommand(cmd);
            shellResetPipeline();
            return makeVoid();
        }
        if (ctx->stage_count > 1) {
            if (!pipeline_head) {
                stdin_fd = ctx->pipes[idx - 1][0];
            }
            if (!pipeline_tail) {
                stdout_fd = ctx->pipes[idx][1];
            }
        }
        if (ctx->merge_stderr && idx < ctx->stage_count && ctx->merge_stderr[idx]) {
            stderr_fd = stdout_fd;
        }
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
    int spawn_err = shellSpawnProcess(vm,
                                      cmd,
                                      stdin_fd,
                                      stdout_fd,
                                      stderr_fd,
                                      &child,
                                      background_execution && !gShellRuntime.job_control_enabled);
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
            if (gShellExitRequested || gShellRuntime.break_requested) {
                kill(-child, SIGTERM);
            }
            int wait_err = shellWaitPid(child, &status, job_control, &stopped);
            if (wait_err == EINTR && (gShellExitRequested || gShellRuntime.break_requested)) {
                kill(-child, SIGTERM);
                int cleanup_status = 0;
                while (shellWaitPid(child, &cleanup_status, false, NULL) == EINTR) {
                    shellRuntimeProcessPendingSignals();
                }
                if (job_control) {
                    shellJobControlRestoreForeground();
                }
                int interrupted_status = gShellRuntime.last_status;
                shellRuntimeProcessPendingSignals();
                shellUpdateStatus(interrupted_status);
                shellFreeCommand(cmd);
                return makeVoid();
            }
            if (wait_err != 0) {
                status = 127;
            }
            if (job_control) {
                shellJobControlRestoreForeground();
            }
            if (wait_err == 0 && !cmd->background && status >= 128 && status < 128 + NSIG) {
                shellHandlePendingSignal(status - 128);
            }
            shellRuntimeProcessPendingSignals();
            if (wait_err == 0 && stopped && job_control) {
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
    return makeVoid();
}
