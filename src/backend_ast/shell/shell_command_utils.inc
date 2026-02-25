/* Auto-generated include: command assembly helpers and metadata parsing. Included from shell.c. */
#include "shell/runner.h"
#include <stdlib.h>
#include <string.h>
#if defined(PSCAL_TARGET_IOS)
#include "common/path_virtualization.h"
#include "common/path_truncate.h"
#include "ios/vproc.h"
extern void pscalRuntimeDebugLog(const char *message);
#endif

#if defined(PSCAL_TARGET_IOS)
#undef stdin
#undef stdout
#undef stderr
#undef printf
#undef puts
#undef putchar
#undef perror
#define stdin shellVprocStdin()
#define stdout shellVprocStdout()
#define stderr shellVprocStderr()
#define printf(...) fprintf(stdout, __VA_ARGS__)
#define puts(text) shellVprocPuts((text))
#define putchar(ch) fputc((ch), stdout)
#define perror(label) shellVprocPerror((label))
#endif

#if defined(PSCAL_TARGET_IOS)
#define SHELL_GETCWD(buf, len) vprocShellGetcwdShim((buf), (len))
#define SHELL_CHDIR(path) vprocShellChdirShim((path))
#else
#define SHELL_GETCWD(buf, len) getcwd((buf), (len))
#define SHELL_CHDIR(path) chdir((path))
#endif

#if defined(PSCAL_TARGET_IOS)
static const char *shellVisiblePath(const char *path, char *buffer, size_t buffer_len) {
    if (!path) {
        return NULL;
    }
    if (pathTruncateStrip(path, buffer, buffer_len)) {
        return buffer;
    }
    return path;
}
#endif

static const char *shellCommandLookupName(const char *name,
                                          char *buffer,
                                          size_t buffer_size) {
    if (!name || !*name) {
        return name;
    }

    const char *candidate = name;
#if defined(PSCAL_TARGET_IOS)
    if (name[0] == '/' &&
        buffer &&
        buffer_size > 0 &&
        pathTruncateStrip(name, buffer, buffer_size) &&
        buffer[0] == '/') {
        candidate = buffer;
    }
#else
    (void)buffer;
    (void)buffer_size;
#endif

    static const char *prefixes[] = {
        "/usr/bin/",
        "/bin/",
        "/private/usr/bin/",
        "/private/bin/"
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        const char *prefix = prefixes[i];
        size_t prefix_len = strlen(prefix);
        if (strncmp(candidate, prefix, prefix_len) != 0) {
            continue;
        }
        const char *leaf = candidate + prefix_len;
        if (!*leaf || strchr(leaf, '/') != NULL) {
            return name;
        }
        return leaf;
    }
    return name;
}

static bool shellBufferEnsure(char **buffer, size_t *length, size_t *capacity, size_t extra);
static bool shellCommandAppendArgOwned(ShellCommand *cmd, char *value);
static bool shellCommandAppendAssignmentOwned(ShellCommand *cmd, char *value, bool is_array_literal);
static bool shellLooksLikeAssignment(const char *text);
static bool shellParseAssignment(const char *assignment, char **out_name, const char **out_value);
static bool shellApplyAssignmentsPermanently(const ShellCommand *cmd,
                                             const char **out_failed_assignment,
                                             bool *out_invalid_assignment);
static bool shellApplyAssignmentsTemporary(const ShellCommand *cmd,
                                           ShellAssignmentBackup **out_backups,
                                           size_t *out_count,
                                           const char **out_failed_assignment,
                                           bool *out_invalid_assignment);
static bool shellSubscriptIsNumeric(const char *text);
static void shellRestoreAssignments(ShellAssignmentBackup *backups, size_t count);
static int shellSpawnProcess(VM *vm,
                             const ShellCommand *cmd,
                             int stdin_fd,
                             int stdout_fd,
                             int stderr_fd,
                             pid_t *child_pid,
                             bool ignore_job_signals);
static int shellWaitPid(pid_t pid, int *status_out, bool allow_stop, bool *out_stopped);
static void shellFreeCommand(ShellCommand *cmd);
static void shellUpdateStatus(int status);
static bool shellCommandIsExecBuiltin(const ShellCommand *cmd);
static bool shellExecuteExecBuiltin(VM *vm, ShellCommand *cmd);
static bool shellApplyExecRedirections(VM *vm, const ShellCommand *cmd,
                                       ShellExecRedirBackup **out_backups,
                                       size_t *out_count);
static bool shellEnsureExecRedirBackup(int target_fd,
                                       int avoid_fd,
                                       ShellExecRedirBackup **backups,
                                       size_t *count,
                                       size_t *capacity);
static void shellRestoreExecRedirections(ShellExecRedirBackup *backups,
                                         size_t count);
static void shellFreeExecRedirBackups(ShellExecRedirBackup *backups,
                                      size_t count);
static inline ShellScalarCacheEntry *shellScalarCacheFind(const char *command);
static inline ShellScalarCacheEntry *shellScalarCacheStore(const char *command,
                                                          const char *result,
                                                          size_t result_length);
static inline void shellScalarCacheRemoveAt(size_t index);
static inline void shellScalarCacheClear(void);
static inline void shellScalarCacheInvalidate(void);
static inline bool shellCaptureStackPush(ShellScalarCacheEntry *entry,
                                         const char *command,
                                         const char *capture,
                                         size_t capture_length,
                                         bool from_cache);
static inline void shellCaptureStackPop(void);
static inline ShellScalarCaptureFrame *shellCaptureStackTop(void);
static inline void shellCaptureStackClear(void);

static bool shellDecodeWordSpec(const char *encoded, const char **out_text, uint8_t *out_flags,
                                const char **out_meta, size_t *out_meta_len) {
    if (out_text) {
        *out_text = encoded ? encoded : "";
    }
    if (out_flags) {
        *out_flags = 0;
    }
    if (out_meta) {
        *out_meta = NULL;
    }
    if (out_meta_len) {
        *out_meta_len = 0;
    }
    if (!encoded) {
        return false;
    }
    size_t len = strlen(encoded);
    if (len < 8 || encoded[0] != SHELL_WORD_ENCODE_PREFIX) {
        return false;
    }
    if (out_flags) {
        uint8_t stored = (uint8_t)encoded[1];
        *out_flags = stored > 0 ? (stored - 1) : 0;
    }
    char meta_len_buf[7];
    memcpy(meta_len_buf, encoded + 2, 6);
    meta_len_buf[6] = '\0';
    size_t meta_len = strtoul(meta_len_buf, NULL, 16);
    if (8 + meta_len > len) {
        return false;
    }
    if (out_meta) {
        *out_meta = encoded + 8;
    }
    if (out_meta_len) {
        *out_meta_len = meta_len;
    }
    if (out_text) {
        *out_text = encoded + 8 + meta_len;
    }
    return true;
}

static void shellFreeMetaSubstitutions(ShellMetaSubstitution *subs, size_t count) {
    if (!subs) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(subs[i].command);
    }
    free(subs);
}

static bool shellParseCommandMetadata(const char *meta, size_t meta_len,
                                      ShellMetaSubstitution **out_subs, size_t *out_count) {
    if (out_subs) {
        *out_subs = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!meta || meta_len == 0) {
        return true;
    }
    if (meta_len < 4) {
        return false;
    }
    char count_buf[5];
    memcpy(count_buf, meta, 4);
    count_buf[4] = '\0';
    size_t count = strtoul(count_buf, NULL, 16);
    if (count == 0) {
        return true;
    }
    ShellMetaSubstitution *subs = (ShellMetaSubstitution *)calloc(count, sizeof(ShellMetaSubstitution));
    if (!subs) {
        return false;
    }
    size_t offset = 4;
    for (size_t i = 0; i < count; ++i) {
        if (offset + 1 + 6 + 6 > meta_len) {
            shellFreeMetaSubstitutions(subs, count);
            return false;
        }
        char style_char = meta[offset++];
        ShellMetaSubstitutionStyle style = (style_char == 'B') ?
            SHELL_META_SUBSTITUTION_BACKTICK : SHELL_META_SUBSTITUTION_DOLLAR;

        char span_buf[7];
        memcpy(span_buf, meta + offset, 6);
        span_buf[6] = '\0';
        size_t span = strtoul(span_buf, NULL, 16);
        offset += 6;

        char len_buf[7];
        memcpy(len_buf, meta + offset, 6);
        len_buf[6] = '\0';
        size_t cmd_len = strtoul(len_buf, NULL, 16);
        offset += 6;
        if (offset + cmd_len > meta_len) {
            shellFreeMetaSubstitutions(subs, count);
            return false;
        }
        char *command = (char *)malloc(cmd_len + 1);
        if (!command && cmd_len > 0) {
            shellFreeMetaSubstitutions(subs, count);
            return false;
        }
        if (cmd_len > 0) {
            memcpy(command, meta + offset, cmd_len);
        }
        if (command) {
            command[cmd_len] = '\0';
        } else {
            command = strdup("");
            if (!command) {
                shellFreeMetaSubstitutions(subs, count);
                return false;
            }
        }
        offset += cmd_len;
        subs[i].style = style;
        subs[i].span_length = span;
        subs[i].command = command;
    }
    if (out_subs) {
        *out_subs = subs;
    } else {
        shellFreeMetaSubstitutions(subs, count);
    }
    if (out_count) {
        *out_count = count;
    }
    return true;
}

typedef struct {
    int fd;
    char *buffer;
    size_t length;
    size_t capacity;
    int error;
} ShellCommandSubReadContext;

static char **shellCommandSubSnapshotEnv(void) {
    extern char **environ;
    size_t count = 0;
    for (char **cursor = environ; cursor && *cursor; ++cursor) {
        count++;
    }
    char **copy = (char **)calloc(count + 1, sizeof(char *));
    if (!copy) {
        return NULL;
    }
    size_t written = 0;
    for (char **cursor = environ; cursor && *cursor; ++cursor) {
        copy[written] = strdup(*cursor);
        if (!copy[written]) {
            for (size_t i = 0; i < written; ++i) {
                free(copy[i]);
            }
            free(copy);
            return NULL;
        }
        written++;
    }
    copy[written] = NULL;
    return copy;
}

static void shellCommandSubRestoreEnv(char **snapshot) {
    if (!snapshot) {
        return;
    }
    /* Clear current environment; emulate clearenv for platforms that do not expose it. */
    extern char **environ;
    if (environ) {
        for (char **cursor = environ; *cursor; ++cursor) {
            const char *entry = *cursor;
            const char *eq = entry ? strchr(entry, '=') : NULL;
            if (!eq || entry == eq) {
                continue;
            }
            size_t name_len = (size_t)(eq - entry);
            char *name = (char *)malloc(name_len + 1);
            if (!name) {
                continue;
            }
            memcpy(name, entry, name_len);
            name[name_len] = '\0';
            unsetenv(name);
            free(name);
        }
    }
    for (char **cursor = snapshot; *cursor; ++cursor) {
        /* putenv takes ownership of the provided string; duplicate so we can free the snapshot. */
        char *dup = strdup(*cursor);
        if (dup) {
            putenv(dup);
        }
    }
    for (char **cursor = snapshot; *cursor; ++cursor) {
        free(*cursor);
    }
    free(snapshot);
}

static void shellCommandSubstitutionDrain(ShellCommandSubReadContext *ctx) {
    if (!ctx || ctx->fd < 0) {
        return;
    }
    int flags = fcntl(ctx->fd, F_GETFL);
    if (flags >= 0) {
        (void)fcntl(ctx->fd, F_SETFL, flags | O_NONBLOCK);
    }
    char buffer[256];
    int idle_spins = 0;
    bool saw_data = false;
    while (true) {
        ssize_t n = vprocHostRead(ctx->fd, buffer, sizeof(buffer));
        if (n > 0) {
            saw_data = true;
            idle_spins = 0;
            if (!shellBufferEnsure(&ctx->buffer, &ctx->length, &ctx->capacity, (size_t)n)) {
                ctx->error = ENOMEM;
                free(ctx->buffer);
                ctx->buffer = NULL;
                ctx->length = 0;
                ctx->capacity = 0;
                break;
            }
            memcpy(ctx->buffer + ctx->length, buffer, (size_t)n);
            ctx->length += (size_t)n;
            ctx->buffer[ctx->length] = '\0';
        } else if (n == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Command substitution output is complete once execution returns.
             * Avoid blocking forever on leaked write-end descriptors. */
            int max_spins = saw_data ? 4 : 20;
            if (idle_spins < max_spins) {
                ++idle_spins;
                usleep(1000);
                continue;
            }
            break;
        } else {
            ctx->error = errno ? errno : EIO;
            free(ctx->buffer);
            ctx->buffer = NULL;
            ctx->length = 0;
            ctx->capacity = 0;
            break;
        }
    }
    vprocHostClose(ctx->fd);
    ctx->fd = -1;
}

static char *shellRunCommandSubstitution(const char *command) {
    const char *source = command ? command : "";
    ShellScalarCacheEntry *cached_entry = NULL;
    ShellScalarCacheEntry *stored_entry = NULL;
    ShellScalarCaptureFrame *capture_frame = NULL;
    bool capture_pushed = shellCaptureStackPush(NULL, source, NULL, 0, false);
    if (capture_pushed) {
        capture_frame = shellCaptureStackTop();
    }

    char *result = NULL;
    size_t result_length = 0;
    bool from_cache = false;

    ShellCommandSubReadContext reader = {
        .fd = -1,
        .buffer = NULL,
        .length = 0,
        .capacity = 0,
        .error = 0,
    };

    int pipes[2] = {-1, -1};
    int saved_stdout = -1;
    int mapped_stdout_fd = -1;
    int saved_mapped_stdout = -1;
    bool stdout_redirected = false;
    bool mapped_stdout_redirected = false;
#if defined(PSCAL_TARGET_IOS)
    VProcSessionStdio *prev_session_stdio = NULL;
    VProcSessionStdio *cmdsub_session_stdio = NULL;
    bool cmdsub_session_activated = false;
#endif

    ShellRunOptions opts;
    ShellSymbolTableScope table_scope;
    bool scope_pushed = false;
    bool exit_requested = false;
    int status = EXIT_FAILURE;
    bool can_run = true;
    bool local_scope_pushed = false;
    bool previous_local_scope_flag = gShellRuntime.local_scope_active;
    bool previous_pipeline_flag = gShellRuntime.pipeline_subshell_active;
    char **env_snapshot = NULL;

    if (*source) {
        cached_entry = shellScalarCacheFind(source);
        if (cached_entry && cached_entry->result) {
            from_cache = true;
            stored_entry = cached_entry;
            gShellRuntime.scalar_cache_hits++;
            cached_entry->hits++;
            result = strdup(cached_entry->result);
            if (!result) {
                result = strdup("");
                result_length = 0;
            } else {
                result_length = cached_entry->result_length;
            }
            goto substitution_finish;
        }
        gShellRuntime.scalar_cache_misses++;
    }

    if (vprocHostPipe(pipes) != 0) {
        result = strdup("");
        result_length = 0;
        goto substitution_finish;
    }

    saved_stdout = vprocHostDup(STDOUT_FILENO);
    if (saved_stdout < 0) {
        vprocHostClose(pipes[0]);
        vprocHostClose(pipes[1]);
        pipes[0] = pipes[1] = -1;
        result = strdup("");
        result_length = 0;
        goto substitution_finish;
    }

#if defined(PSCAL_TARGET_IOS)
    VProc *active_vp = vprocCurrent();
    prev_session_stdio = vprocSessionStdioCurrent();
    if (active_vp) {
        int translated = vprocTranslateFd(active_vp, STDOUT_FILENO);
        if (translated >= 0 && translated != STDOUT_FILENO) {
            mapped_stdout_fd = translated;
            saved_mapped_stdout = vprocHostDup(mapped_stdout_fd);
            if (saved_mapped_stdout < 0) {
                vprocHostClose(pipes[0]);
                vprocHostClose(pipes[1]);
                pipes[0] = pipes[1] = -1;
                vprocHostClose(saved_stdout);
                saved_stdout = -1;
                result = strdup("");
                result_length = 0;
                goto substitution_finish;
            }
        }
    }
#endif

    if (vprocHostDup2(pipes[1], STDOUT_FILENO) < 0) {
        int err = errno;
        vprocHostClose(pipes[0]);
        vprocHostClose(pipes[1]);
        pipes[0] = pipes[1] = -1;
        vprocHostClose(saved_stdout);
        saved_stdout = -1;
        if (saved_mapped_stdout >= 0) {
            vprocHostClose(saved_mapped_stdout);
            saved_mapped_stdout = -1;
        }
        fprintf(stderr, "exsh: command substitution: failed to redirect stdout: %s\n", strerror(err));
        result = strdup("");
        result_length = 0;
        goto substitution_finish;
    }
    stdout_redirected = true;

#if defined(PSCAL_TARGET_IOS)
    if (mapped_stdout_fd >= 0) {
        if (vprocHostDup2(pipes[1], mapped_stdout_fd) < 0) {
            int err = errno;
            vprocHostDup2(saved_stdout, STDOUT_FILENO);
            vprocHostClose(saved_stdout);
            saved_stdout = -1;
            vprocHostClose(pipes[0]);
            vprocHostClose(pipes[1]);
            pipes[0] = pipes[1] = -1;
            if (saved_mapped_stdout >= 0) {
                vprocHostClose(saved_mapped_stdout);
                saved_mapped_stdout = -1;
            }
            fprintf(stderr, "exsh: command substitution: failed to redirect mapped stdout: %s\n", strerror(err));
            result = strdup("");
            result_length = 0;
            goto substitution_finish;
        }
        mapped_stdout_redirected = true;
    }
#endif
    vprocHostClose(pipes[1]);
    pipes[1] = -1;

    reader.fd = pipes[0];

#if defined(PSCAL_TARGET_IOS)
    cmdsub_session_stdio = vprocSessionStdioCreate();
    if (cmdsub_session_stdio) {
        int session_kernel_pid = prev_session_stdio ? prev_session_stdio->kernel_pid : vprocGetKernelPid();
        int session_stdin = (prev_session_stdio && prev_session_stdio->stdin_host_fd >= 0)
            ? prev_session_stdio->stdin_host_fd
            : STDIN_FILENO;
        int session_stderr = (prev_session_stdio && prev_session_stdio->stderr_host_fd >= 0)
            ? prev_session_stdio->stderr_host_fd
            : STDERR_FILENO;
        vprocSessionStdioInitWithFds(cmdsub_session_stdio,
                                     session_stdin,
                                     STDOUT_FILENO,
                                     session_stderr,
                                     session_kernel_pid);
        if (prev_session_stdio) {
            cmdsub_session_stdio->shell_pid = prev_session_stdio->shell_pid;
            cmdsub_session_stdio->session_id = prev_session_stdio->session_id;
        }
        vprocSessionStdioActivate(cmdsub_session_stdio);
        cmdsub_session_activated = true;
    }
#endif

    memset(&opts, 0, sizeof(opts));
    opts.no_cache = 1;
    opts.quiet = true;
    opts.suppress_warnings = true;
    opts.exit_on_signal = shellRuntimeExitOnSignal();
    const char *frontend_path = shellRuntimeGetArg0();
    opts.frontend_path = frontend_path ? frontend_path : "exsh";

    exit_requested = false;
    status = EXIT_FAILURE;

    shellSymbolTableScopeInit(&table_scope);
    scope_pushed = shellSymbolTableScopePush(&table_scope);
    if (!scope_pushed && !shellSymbolTableScopeIsActive()) {
        fprintf(stderr, "shell: failed to allocate symbol tables.\n");
    }

    can_run = scope_pushed || shellSymbolTableScopeIsActive();
    if (can_run) {
        if (!shellLocalScopePush()) {
            fprintf(stderr, "shell: command substitution: failed to allocate local scope.\n");
            can_run = false;
        } else {
            local_scope_pushed = true;
            gShellRuntime.local_scope_active = true;
            gShellRuntime.pipeline_subshell_active = true;
        }
    }

    if (can_run) {
        env_snapshot = shellCommandSubSnapshotEnv();
        status = shellRunSource(source, "<command-substitution>", &opts, &exit_requested);
    }

    if (scope_pushed) {
        shellSymbolTableScopePop(&table_scope);
    }

    if (local_scope_pushed) {
        shellLocalScopePop();
    }
    gShellRuntime.local_scope_active = previous_local_scope_flag;
    gShellRuntime.pipeline_subshell_active = previous_pipeline_flag;

    shellFlushStandardStreams();

    if (mapped_stdout_redirected && saved_mapped_stdout >= 0) {
        vprocHostDup2(saved_mapped_stdout, mapped_stdout_fd);
        mapped_stdout_redirected = false;
    }
    if (saved_mapped_stdout >= 0) {
        vprocHostClose(saved_mapped_stdout);
        saved_mapped_stdout = -1;
    }
    if (stdout_redirected) {
        vprocHostDup2(saved_stdout, STDOUT_FILENO);
        stdout_redirected = false;
    }
    if (saved_stdout >= 0) {
        vprocHostClose(saved_stdout);
        saved_stdout = -1;
    }

    /* Drain synchronously to avoid background lifetime issues. */
    shellCommandSubstitutionDrain(&reader);

    if (reader.fd >= 0) {
        vprocHostClose(reader.fd);
        reader.fd = -1;
    }
    if (pipes[0] >= 0) {
        vprocHostClose(pipes[0]);
        pipes[0] = -1;
    }

    int final_status = shellRuntimeLastStatus();
    if (!exit_requested && status != EXIT_SUCCESS) {
        final_status = status;
    }
    shellUpdateStatus(final_status);

    if (!reader.buffer) {
        result = strdup("");
        result_length = 0;
        goto substitution_finish;
    }

    char *output = reader.buffer;
    size_t length = reader.length;
    while (length > 0 && (output[length - 1] == '\n' || output[length - 1] == '\r')) {
        output[--length] = '\0';
    }
    result = output;
    result_length = length;

    if (*source) {
        stored_entry = shellScalarCacheStore(source, result, result_length);
        if (stored_entry) {
            gShellRuntime.scalar_cache_stores++;
        }
    }

substitution_finish:
#if defined(PSCAL_TARGET_IOS)
    if (cmdsub_session_activated) {
        vprocSessionStdioActivate(prev_session_stdio);
        cmdsub_session_activated = false;
    }
    if (cmdsub_session_stdio) {
        vprocSessionStdioDestroy(cmdsub_session_stdio);
        cmdsub_session_stdio = NULL;
    }
#endif
    if (reader.error && !exit_requested) {
        shellUpdateStatus(reader.error);
    }
    if (capture_frame) {
        capture_frame->entry = stored_entry;
        capture_frame->from_cache = from_cache;
        if (capture_frame->capture) {
            free(capture_frame->capture);
            capture_frame->capture = NULL;
        }
        if (result) {
            capture_frame->capture = strdup(result);
            if (capture_frame->capture) {
                capture_frame->capture_length = result_length;
            } else {
                capture_frame->capture_length = 0;
            }
        } else {
            capture_frame->capture_length = 0;
        }
    }

    if (capture_pushed) {
        shellCaptureStackPop();
    }

    if (env_snapshot) {
        shellCommandSubRestoreEnv(env_snapshot);
        env_snapshot = NULL;
    }

    if (!result) {
        result = strdup("");
    }
    return result;
}

static bool shellBufferEnsure(char **buffer, size_t *length, size_t *capacity, size_t extra) {
    if (!buffer || !length || !capacity) {
        return false;
    }
    size_t needed = *length + extra + 1; // +1 for null terminator
    if (*capacity == 0 && *buffer != NULL) {
        /* If the caller passed a non-owned or stale pointer with no capacity
         * metadata, reset to NULL so we allocate fresh storage rather than
         * reallocating an invalid pointer. */
        *buffer = NULL;
    }
    if (needed <= *capacity) {
        return true;
    }
    size_t new_capacity = *capacity ? *capacity : 32;
    while (needed > new_capacity) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    char *new_buffer = (char *)realloc(*buffer, new_capacity);
    if (!new_buffer) {
        return false;
    }
    *buffer = new_buffer;
    *capacity = new_capacity;
    return true;
}

static void shellDisposeCompiledFunction(ShellCompiledFunction *fn) {
    if (!fn) {
        return;
    }
    freeBytecodeChunk(&fn->chunk);
    free(fn);
}

static pthread_mutex_t gShellFunctionTableMutex = PTHREAD_MUTEX_INITIALIZER;

static ShellFunctionEntry *shellFindFunctionEntryUnlocked(const char *name) {
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < gShellFunctionCount; ++i) {
        ShellFunctionEntry *entry = &gShellFunctions[i];
        if (entry->name && strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

static bool shellFunctionIsDefined(const char *name) {
    bool defined = false;
    pthread_mutex_lock(&gShellFunctionTableMutex);
    ShellFunctionEntry *entry = shellFindFunctionEntryUnlocked(name);
    defined = (entry && entry->compiled);
    pthread_mutex_unlock(&gShellFunctionTableMutex);
    return defined;
}

static ShellCompiledFunction *shellFindCompiledFunction(const char *name) {
    ShellCompiledFunction *compiled = NULL;
    pthread_mutex_lock(&gShellFunctionTableMutex);
    ShellFunctionEntry *entry = shellFindFunctionEntryUnlocked(name);
    if (entry) {
        compiled = entry->compiled;
    }
    pthread_mutex_unlock(&gShellFunctionTableMutex);
    return compiled;
}

static bool shellStoreFunction(const char *name, const char *param_meta, ShellCompiledFunction *compiled) {
    if (!name || !compiled) {
        return false;
    }
    char *name_copy = strdup(name);
    char *meta_copy = NULL;
    if (!name_copy) {
        return false;
    }
    if (param_meta && *param_meta) {
        meta_copy = strdup(param_meta);
        if (!meta_copy) {
            free(name_copy);
            return false;
        }
    }
    pthread_mutex_lock(&gShellFunctionTableMutex);
    ShellFunctionEntry *existing = shellFindFunctionEntryUnlocked(name);
    if (existing) {
        /* Another shell session may still be executing the old body. */
        free(existing->name);
        free(existing->parameter_metadata);
        existing->name = name_copy;
        existing->parameter_metadata = meta_copy;
        existing->compiled = compiled;
        pthread_mutex_unlock(&gShellFunctionTableMutex);
        return true;
    }
    ShellFunctionEntry *entries = realloc(gShellFunctions, sizeof(ShellFunctionEntry) * (gShellFunctionCount + 1));
    if (!entries) {
        pthread_mutex_unlock(&gShellFunctionTableMutex);
        free(name_copy);
        free(meta_copy);
        return false;
    }
    gShellFunctions = entries;
    ShellFunctionEntry *entry = &gShellFunctions[gShellFunctionCount++];
    entry->name = name_copy;
    entry->parameter_metadata = meta_copy;
    entry->compiled = compiled;
    pthread_mutex_unlock(&gShellFunctionTableMutex);
    return true;
}

static bool shellCommandAppendArgOwned(ShellCommand *cmd, char *value) {
    if (!cmd || !value) {
        free(value);
        return false;
    }
    char **new_argv = realloc(cmd->argv, sizeof(char*) * (cmd->argc + 2));
    if (!new_argv) {
        free(value);
        return false;
    }
    cmd->argv = new_argv;
    cmd->argv[cmd->argc] = value;
    cmd->argc++;
    cmd->argv[cmd->argc] = NULL;
    return true;
}

static void shellRewriteDoubleBracketTest(ShellCommand *cmd) {
    if (!cmd || cmd->argc < 2 || !cmd->argv) {
        return;
    }
    char *first = cmd->argv[0];
    if (!first || strcmp(first, "[[") != 0) {
        return;
    }
    size_t last_index = cmd->argc - 1;
    char *last = cmd->argv[last_index];
    if (!last || strcmp(last, "]]") != 0) {
        return;
    }

    free(last);
    cmd->argv[last_index] = NULL;
    cmd->argc = last_index;

    if (cmd->argv) {
        cmd->argv[cmd->argc] = NULL;
    }

    if (cmd->redirs && cmd->redir_count > 0) {
        for (size_t i = 0; i < cmd->redir_count; ++i) {
            ShellRedirection *redir = &cmd->redirs[i];
            if (!redir) {
                continue;
            }

            const char *op = NULL;
            if (redir->kind == SHELL_RUNTIME_REDIR_OPEN) {
                int mode = redir->flags & O_ACCMODE;
                if (mode == O_RDONLY) {
                    op = "<";
                } else if (mode == O_WRONLY || mode == O_RDWR) {
                    op = (redir->flags & O_APPEND) ? ">>" : ">";
                }
            }

            if (!op) {
                continue;
            }

            char *op_copy = strdup(op);
            if (op_copy) {
                shellCommandAppendArgOwned(cmd, op_copy);
            }

            char *target = NULL;
            if (redir->path) {
                target = redir->path;
                redir->path = NULL;
            }
            if (!target) {
                target = strdup("");
            }
            if (target) {
                shellCommandAppendArgOwned(cmd, target);
            }

            free(redir->here_doc);
            redir->here_doc = NULL;
            free(redir->here_string_literal);
            redir->here_string_literal = NULL;
            free(redir->here_string);
            redir->here_string = NULL;
        }

        free(cmd->redirs);
        cmd->redirs = NULL;
        cmd->redir_count = 0;
    }

    char *replacement = strdup("__shell_double_bracket");
    if (replacement) {
        free(first);
        cmd->argv[0] = replacement;
    }

}

static bool shellCommandAppendAssignmentOwned(ShellCommand *cmd, char *value, bool is_array_literal) {
    if (!cmd || !value) {
        free(value);
        return false;
    }
    ShellAssignmentEntry *entries =
        (ShellAssignmentEntry *)realloc(cmd->assignments,
                                        sizeof(ShellAssignmentEntry) * (cmd->assignment_count + 1));
    if (!entries) {
        free(value);
        return false;
    }
    cmd->assignments = entries;
    cmd->assignments[cmd->assignment_count].text = value;
    cmd->assignments[cmd->assignment_count].is_array_literal = is_array_literal;
    cmd->assignment_count++;
    return true;
}

static bool shellLooksLikeAssignment(const char *text) {
    if (!text) {
        return false;
    }
    char *name = NULL;
    bool ok = shellParseAssignment(text, &name, NULL);
    free(name);
    return ok;
}

static bool shellParseAssignment(const char *assignment, char **out_name, const char **out_value) {
    if (out_name) {
        *out_name = NULL;
    }
    if (out_value) {
        *out_value = NULL;
    }
    if (!assignment) {
        return false;
    }
    const char *eq = strchr(assignment, '=');
    if (!eq || eq == assignment) {
        return false;
    }
    size_t name_len = (size_t)(eq - assignment);
    bool in_brackets = false;
    for (size_t i = 0; i < name_len; ++i) {
        unsigned char ch = (unsigned char)assignment[i];
        if (i == 0) {
            if (!isalpha(ch) && ch != '_') {
                return false;
            }
            continue;
        }
        if (in_brackets) {
            if (ch == ']') {
                in_brackets = false;
            }
            continue;
        }
        if (ch == '[') {
            in_brackets = true;
            continue;
        }
        if (!isalnum(ch) && ch != '_') {
            return false;
        }
    }
    if (in_brackets) {
        return false;
    }
    char *name = (char *)malloc(name_len + 1);
    if (!name) {
        return false;
    }
    memcpy(name, assignment, name_len);
    name[name_len] = '\0';
    if (out_name) {
        *out_name = name;
    } else {
        free(name);
    }
    if (out_value) {
        *out_value = eq + 1;
    }
    return true;
}

static bool shellExtractArrayNameAndSubscript(const char *text,
                                             char **out_name,
                                             char **out_subscript) {
    if (out_name) {
        *out_name = NULL;
    }
    if (out_subscript) {
        *out_subscript = NULL;
    }
    if (!text) {
        return false;
    }
    const char *open = strchr(text, '[');
    if (!open) {
        return false;
    }
    const char *close = strrchr(text, ']');
    if (!close || close < open || close[1] != '\0') {
        return false;
    }
    size_t name_len = (size_t)(open - text);
    if (name_len == 0) {
        return false;
    }
    char *name_copy = (char *)malloc(name_len + 1);
    if (!name_copy) {
        return false;
    }
    memcpy(name_copy, text, name_len);
    name_copy[name_len] = '\0';

    size_t sub_len = (size_t)(close - (open + 1));
    char *sub_copy = (char *)malloc(sub_len + 1);
    if (!sub_copy) {
        free(name_copy);
        return false;
    }
    if (sub_len > 0) {
        memcpy(sub_copy, open + 1, sub_len);
    }
    sub_copy[sub_len] = '\0';

    if (out_name) {
        *out_name = name_copy;
    } else {
        free(name_copy);
    }
    if (out_subscript) {
        *out_subscript = sub_copy;
    } else {
        free(sub_copy);
    }
    return true;
}

static bool shellApplyAssignmentsPermanently(const ShellCommand *cmd,
                                             const char **out_failed_assignment,
                                             bool *out_invalid_assignment) {
    if (out_failed_assignment) {
        *out_failed_assignment = NULL;
    }
    if (out_invalid_assignment) {
        *out_invalid_assignment = false;
    }
    if (!cmd) {
        return true;
    }
    for (size_t i = 0; i < cmd->assignment_count; ++i) {
        const ShellAssignmentEntry *entry = &cmd->assignments[i];
        const char *assignment = entry->text;
        char *name = NULL;
        const char *value = NULL;
        if (!shellParseAssignment(assignment, &name, &value)) {
            if (out_failed_assignment) {
                *out_failed_assignment = assignment;
            }
            if (out_invalid_assignment) {
                *out_invalid_assignment = true;
            }
            free(name);
            return false;
        }
        char *base_name = NULL;
        char *subscript_text = NULL;
        bool is_element = shellExtractArrayNameAndSubscript(name, &base_name, &subscript_text);
        if (is_element && (!subscript_text || !shellSubscriptIsNumeric(subscript_text)) &&
            !shellAssociativeArraysSupported()) {
            free(base_name);
            base_name = NULL;
            free(subscript_text);
            subscript_text = NULL;
            is_element = false;
        }
        const char *effective_name = is_element ? base_name : name;
        if (shellHandleSpecialAssignment(effective_name, value)) {
            free(name);
            free(base_name);
            free(subscript_text);
            continue;
        }
        bool set_ok;
        if (is_element) {
            set_ok = shellArrayRegistrySetElement(effective_name, subscript_text, value);
        } else {
            set_ok = shellSetTrackedVariable(effective_name, value, entry->is_array_literal);
        }
        free(base_name);
        free(subscript_text);
        if (!set_ok) {
            if (out_failed_assignment) {
                *out_failed_assignment = assignment;
            }
            free(name);
            return false;
        }
        free(name);
    }
    return true;
}

static void shellRestoreAssignments(ShellAssignmentBackup *backups, size_t count) {
    if (!backups) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        ShellAssignmentBackup *backup = &backups[i];
        if (!backup->name) {
            continue;
        }
        if (backup->had_previous) {
            shellSetTrackedVariable(backup->name, backup->previous_value, backup->previous_was_array);
        } else {
            shellUnsetTrackedVariable(backup->name);
        }
        free(backup->name);
        free(backup->previous_value);
    }
    free(backups);
}

static bool shellApplyAssignmentsTemporary(const ShellCommand *cmd,
                                           ShellAssignmentBackup **out_backups,
                                           size_t *out_count,
                                           const char **out_failed_assignment,
                                           bool *out_invalid_assignment) {
    if (out_backups) {
        *out_backups = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (out_failed_assignment) {
        *out_failed_assignment = NULL;
    }
    if (out_invalid_assignment) {
        *out_invalid_assignment = false;
    }
    if (!cmd || cmd->assignment_count == 0) {
        return true;
    }
    ShellAssignmentBackup *backups = calloc(cmd->assignment_count, sizeof(ShellAssignmentBackup));
    if (!backups) {
        return false;
    }
    for (size_t i = 0; i < cmd->assignment_count; ++i) {
        const ShellAssignmentEntry *entry = &cmd->assignments[i];
        const char *assignment = entry->text;
        char *name = NULL;
        const char *value = NULL;
        if (!shellParseAssignment(assignment, &name, &value)) {
            if (out_failed_assignment) {
                *out_failed_assignment = assignment;
            }
            if (out_invalid_assignment) {
                *out_invalid_assignment = true;
            }
            shellRestoreAssignments(backups, i);
            return false;
        }
        char *base_name = NULL;
        char *subscript_text = NULL;
        bool is_element = shellExtractArrayNameAndSubscript(name, &base_name, &subscript_text);
        if (is_element && (!subscript_text || !shellSubscriptIsNumeric(subscript_text)) &&
            !shellAssociativeArraysSupported()) {
            free(base_name);
            base_name = NULL;
            free(subscript_text);
            subscript_text = NULL;
            is_element = false;
        }
        char *effective_name_owned = NULL;
        if (is_element) {
            effective_name_owned = base_name;
            base_name = NULL;
        } else {
            effective_name_owned = name;
            name = NULL;
        }
        const char *effective_name = effective_name_owned;

        if (shellHandleSpecialAssignment(effective_name, value)) {
            free(name);
            free(base_name);
            free(subscript_text);
            free(effective_name_owned);
            backups[i].name = NULL;
            backups[i].previous_value = NULL;
            backups[i].had_previous = false;
            backups[i].previous_was_array = false;
            continue;
        }

        backups[i].name = effective_name_owned;
        effective_name_owned = NULL;
        free(name);
        free(base_name);
        ShellLocalVariable *local_entry = shellLocalScopeFind(backups[i].name);
        if (local_entry) {
            if (!shellLocalScopeGetCopy(backups[i].name, &backups[i].previous_value,
                                        &backups[i].previous_was_array)) {
                shellRestoreAssignments(backups, i + 1);
                return false;
            }
            backups[i].had_previous = true;
        } else {
            const char *previous = shellSafeGetenv(backups[i].name);
            if (previous) {
                backups[i].previous_value = strdup(previous);
                if (!backups[i].previous_value) {
                    shellRestoreAssignments(backups, i + 1);
                    return false;
                }
                backups[i].had_previous = true;
                backups[i].previous_was_array = (shellArrayRegistryFindConst(backups[i].name) != NULL);
            } else {
                backups[i].previous_value = NULL;
                backups[i].had_previous = false;
                backups[i].previous_was_array = false;
            }
        }
        bool set_ok;
        if (is_element) {
            set_ok = shellArrayRegistrySetElement(backups[i].name, subscript_text, value);
        } else {
            set_ok = shellSetTrackedVariable(backups[i].name, value, entry->is_array_literal);
        }
        free(subscript_text);
        if (!set_ok) {
            if (out_failed_assignment) {
                *out_failed_assignment = assignment;
            }
            shellRestoreAssignments(backups, i + 1);
            return false;
        }
    }
    if (out_backups) {
        *out_backups = backups;
    } else {
        shellRestoreAssignments(backups, cmd->assignment_count);
    }
    if (out_count) {
        *out_count = cmd->assignment_count;
    }
    return true;
}

static bool shellWordShouldGlob(uint8_t flags, const char *text) {
    if (!text || !*text) {
        return false;
    }
    if (flags & (SHELL_WORD_FLAG_SINGLE_QUOTED | SHELL_WORD_FLAG_DOUBLE_QUOTED)) {
        return false;
    }
    for (const char *cursor = text; *cursor; ++cursor) {
        if (*cursor == '*' || *cursor == '?' || *cursor == '[') {
            return true;
        }
    }
    return false;
}

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} ShellStringArray;

static void shellStringArrayFree(ShellStringArray *array) {
    if (!array) {
        return;
    }
    if (array->items) {
        for (size_t i = 0; i < array->count; ++i) {
            free(array->items[i]);
        }
        free(array->items);
    }
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static bool shellStringArrayAppend(ShellStringArray *array, char *value) {
    if (!array || !value) {
        return false;
    }
    if (array->count + 1 > array->capacity) {
        size_t new_capacity = array->capacity ? array->capacity * 2 : 4;
        char **new_items = (char **)realloc(array->items, new_capacity * sizeof(char *));
        if (!new_items) {
            return false;
        }
        array->items = new_items;
        array->capacity = new_capacity;
    }
    array->items[array->count++] = value;
    return true;
}

static void shellFreeStringArray(char **items, size_t count) {
    if (!items) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(items[i]);
    }
    free(items);
}

static bool shellQuotedMapEnsureCapacity(bool track, bool **map, size_t *length, size_t *capacity, size_t extra) {
    if (!track) {
        return true;
    }
    size_t needed = *length + extra;
    if (needed <= *capacity) {
        return true;
    }
    size_t new_capacity = (*capacity == 0) ? 32 : *capacity;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }
    bool *resized = (bool *)realloc(*map, new_capacity * sizeof(bool));
    if (!resized) {
        return false;
    }
    *map = resized;
    *capacity = new_capacity;
    return true;
}

static bool shellQuotedMapAppendRepeated(bool track,
                                         bool **map,
                                         size_t *length,
                                         size_t *capacity,
                                         bool flag,
                                         size_t count) {
    if (!shellQuotedMapEnsureCapacity(track, map, length, capacity, count)) {
        return false;
    }
    if (!track) {
        return true;
    }
    for (size_t i = 0; i < count; ++i) {
        (*map)[(*length)++] = flag;
    }
    return true;
}

static bool shellSplitExpandedWord(const char *expanded, uint8_t word_flags,
                                   const bool *quoted_map, size_t quoted_len,
                                   bool array_zero,
                                   char ***out_fields, size_t *out_field_count) {
    if (out_fields) {
        *out_fields = NULL;
    }
    if (out_field_count) {
        *out_field_count = 0;
    }
    if (!out_fields || !out_field_count) {
        return false;
    }
    if (!expanded) {
        return true;
    }

    size_t length = strlen(expanded);
    if (array_zero && length == 0) {
        return true;
    }
    if (strchr(expanded, SHELL_ARRAY_ELEMENT_SEP)) {
        ShellStringArray fields = (ShellStringArray){0};
        const char *segment = expanded;
        bool base_quoted = (word_flags & (SHELL_WORD_FLAG_SINGLE_QUOTED | SHELL_WORD_FLAG_DOUBLE_QUOTED)) != 0;
        while (1) {
            const char *next = strchr(segment, SHELL_ARRAY_ELEMENT_SEP);
            size_t seg_len = next ? (size_t)(next - segment) : strlen(segment);
            char *copy = (char *)malloc(seg_len + 1);
            if (!copy) {
                shellStringArrayFree(&fields);
                return false;
            }
            if (seg_len > 0) {
                memcpy(copy, segment, seg_len);
            }
            copy[seg_len] = '\0';
            if (base_quoted) {
                if (!shellStringArrayAppend(&fields, copy)) {
                    free(copy);
                    shellStringArrayFree(&fields);
                    return false;
                }
            } else {
                char **sub_fields = NULL;
                size_t sub_count = 0;
                if (!shellSplitExpandedWord(copy, 0, NULL, 0, false, &sub_fields, &sub_count)) {
                    free(copy);
                    shellStringArrayFree(&fields);
                    return false;
                }
                free(copy);
                for (size_t i = 0; i < sub_count; ++i) {
                    if (!shellStringArrayAppend(&fields, sub_fields[i])) {
                        for (size_t j = i; j < sub_count; ++j) {
                            free(sub_fields[j]);
                        }
                        free(sub_fields);
                        shellStringArrayFree(&fields);
                        return false;
                    }
                }
                free(sub_fields);
            }
            if (!next) {
                break;
            }
            segment = next + 1;
        }
        *out_fields = fields.items;
        *out_field_count = fields.count;
        return true;
    }
    bool use_map = quoted_map && quoted_len == length;
    const char *ifs = shellSafeGetenv("IFS");
    if (!ifs) {
        ifs = " \t\n";
    }
    bool quoted = (word_flags & (SHELL_WORD_FLAG_SINGLE_QUOTED | SHELL_WORD_FLAG_DOUBLE_QUOTED)) != 0;
    if (!quoted && use_map) {
        quoted = true;
        for (size_t i = 0; i < length; ++i) {
            if (!quoted_map[i]) {
                quoted = false;
                break;
            }
        }
    }
    if (quoted || *ifs == '\0') {
        char *dup = strdup(expanded);
        if (!dup) {
            return false;
        }
        char **items = (char **)malloc(sizeof(char *));
        if (!items) {
            free(dup);
            return false;
        }
        items[0] = dup;
        *out_fields = items;
        *out_field_count = 1;
        return true;
    }
    if (*expanded == '\0') {
        return true;
    }

    bool delim_map[256] = {false};
    bool whitespace_map[256] = {false};
    for (const char *cursor = ifs; *cursor; ++cursor) {
        unsigned char ch = (unsigned char)*cursor;
        delim_map[ch] = true;
        if (isspace((unsigned char)*cursor)) {
            whitespace_map[ch] = true;
        }
    }

    ShellStringArray fields = (ShellStringArray){0};
    const char *cursor = expanded;
    while (*cursor) {
        size_t index = (size_t)(cursor - expanded);
        if (use_map && quoted_map[index]) {
            break;
        }
        if (!whitespace_map[(unsigned char)*cursor]) {
            break;
        }
        cursor++;
    }

    bool last_non_wh_delim = false;
    while (*cursor) {
        unsigned char ch = (unsigned char)*cursor;
        size_t index = (size_t)(cursor - expanded);
        bool char_quoted = use_map && quoted_map[index];
        if (!char_quoted && delim_map[ch] && !whitespace_map[ch]) {
            char *empty = strdup("");
            if (!empty || !shellStringArrayAppend(&fields, empty)) {
                free(empty);
                shellStringArrayFree(&fields);
                return false;
            }
            cursor++;
            while (*cursor) {
                size_t ws_index = (size_t)(cursor - expanded);
                if (use_map && quoted_map[ws_index]) {
                    break;
                }
                if (!whitespace_map[(unsigned char)*cursor]) {
                    break;
                }
                cursor++;
            }
            last_non_wh_delim = true;
            continue;
        }

        const char *start = cursor;
        while (*cursor) {
            unsigned char inner = (unsigned char)*cursor;
            size_t inner_index = (size_t)(cursor - expanded);
            bool inner_quoted = use_map && quoted_map[inner_index];
            if (!inner_quoted && delim_map[inner]) {
                break;
            }
            cursor++;
        }
        size_t span = (size_t)(cursor - start);
        if (span > 0) {
            char *segment = (char *)malloc(span + 1);
            if (!segment) {
                shellStringArrayFree(&fields);
                return false;
            }
            memcpy(segment, start, span);
            segment[span] = '\0';
            if (!shellStringArrayAppend(&fields, segment)) {
                free(segment);
                shellStringArrayFree(&fields);
                return false;
            }
        }

        if (!*cursor) {
            last_non_wh_delim = false;
            break;
        }

        if (!char_quoted && delim_map[(unsigned char)*cursor] && !whitespace_map[(unsigned char)*cursor]) {
            cursor++;
            last_non_wh_delim = true;
        } else {
            while (*cursor) {
                size_t ws_index = (size_t)(cursor - expanded);
                if (use_map && quoted_map[ws_index]) {
                    break;
                }
                if (!whitespace_map[(unsigned char)*cursor]) {
                    break;
                }
                cursor++;
            }
            last_non_wh_delim = false;
        }

        while (*cursor) {
            size_t ws_index = (size_t)(cursor - expanded);
            if (use_map && quoted_map[ws_index]) {
                break;
            }
            if (!whitespace_map[(unsigned char)*cursor]) {
                break;
            }
            cursor++;
        }
    }

    if (last_non_wh_delim) {
        char *empty = strdup("");
        if (!empty || !shellStringArrayAppend(&fields, empty)) {
            free(empty);
            shellStringArrayFree(&fields);
            return false;
        }
    }

    if (fields.count == 0) {
        free(fields.items);
        return true;
    }

    *out_fields = fields.items;
    *out_field_count = fields.count;
    return true;
}

static bool shellLoopFrameEnsureValueCapacity(ShellLoopFrame *frame, size_t *capacity, size_t needed) {
    if (!frame || !capacity) {
        return false;
    }
    if (*capacity >= needed) {
        return true;
    }
    size_t new_capacity = (*capacity == 0) ? 4 : *capacity;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }
    char **resized = (char **)realloc(frame->for_values, new_capacity * sizeof(char *));
    if (!resized) {
        return false;
    }
    frame->for_values = resized;
    *capacity = new_capacity;
    return true;
}

static bool shellLoopFrameAppendValue(ShellLoopFrame *frame, size_t *capacity, char *value) {
    if (!frame || !capacity || !value) {
        free(value);
        return false;
    }
    if (!shellLoopFrameEnsureValueCapacity(frame, capacity, frame->for_count + 1)) {
        free(value);
        return false;
    }
    frame->for_values[frame->for_count++] = value;
    return true;
}

static void shellBufferAppendChar(char **buffer, size_t *length, size_t *capacity, char c) {
    if (!shellBufferEnsure(buffer, length, capacity, 1)) {
        return;
    }
    (*buffer)[(*length)++] = c;
    (*buffer)[*length] = '\0';
}

static void shellBufferAppendString(char **buffer, size_t *length, size_t *capacity, const char *str) {
    if (!str) {
        return;
    }
    size_t add = strlen(str);
    if (add == 0) {
        return;
    }
    if (!shellBufferEnsure(buffer, length, capacity, add)) {
        return;
    }
    memcpy(*buffer + *length, str, add);
    *length += add;
    (*buffer)[*length] = '\0';
}

static void shellBufferAppendSlice(char **buffer,
                                   size_t *length,
                                   size_t *capacity,
                                   const char *data,
                                   size_t slice_len) {
    if (!data || slice_len == 0) {
        return;
    }
    if (!shellBufferEnsure(buffer, length, capacity, slice_len)) {
        return;
    }
    memcpy(*buffer + *length, data, slice_len);
    *length += slice_len;
    (*buffer)[*length] = '\0';
}

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} ShellHistoryWordArray;

static void shellHistoryWordArrayFree(ShellHistoryWordArray *array) {
    if (!array || !array->items) {
        return;
    }
    for (size_t i = 0; i < array->count; ++i) {
        free(array->items[i]);
    }
    free(array->items);
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static bool shellHistoryWordArrayAppend(ShellHistoryWordArray *array, char *word) {
    if (!array || !word) {
        return false;
    }
    if (array->count == array->capacity) {
        size_t new_capacity = array->capacity ? array->capacity * 2 : 8;
        char **items = realloc(array->items, new_capacity * sizeof(char *));
        if (!items) {
            return false;
        }
        array->items = items;
        array->capacity = new_capacity;
    }
    array->items[array->count++] = word;
    return true;
}

static bool shellTokenizeHistoryEntry(const char *entry, ShellHistoryWordArray *out_words) {
    if (!entry || !out_words) {
        return false;
    }
    out_words->items = NULL;
    out_words->count = 0;
    out_words->capacity = 0;

    char *current = NULL;
    size_t current_len = 0;
    size_t current_cap = 0;
    bool in_single = false;
    bool in_double = false;
    bool escape = false;
    bool word_active = false;

    for (size_t i = 0;; ++i) {
        char c = entry[i];
        bool at_end = (c == '\0');

        if (!at_end && escape) {
            shellBufferAppendChar(&current, &current_len, &current_cap, c);
            escape = false;
            word_active = true;
            continue;
        }

        if (!at_end && c == '\\' && !escape) {
            escape = true;
            word_active = true;
            continue;
        }

        if (!at_end && c == '\'' && !in_double) {
            in_single = !in_single;
            word_active = true;
            continue;
        }

        if (!at_end && c == '"' && !in_single) {
            in_double = !in_double;
            word_active = true;
            continue;
        }

        if (at_end && escape) {
            shellBufferAppendChar(&current, &current_len, &current_cap, '\\');
            escape = false;
        }

        bool is_space = (!in_single && !in_double && (c == ' ' || c == '\t'));
        if (at_end || is_space) {
            if (word_active) {
                const char *src = current ? current : "";
                char *word = strdup(src);
                if (!word) {
                    free(current);
                    shellHistoryWordArrayFree(out_words);
                    return false;
                }
                if (!shellHistoryWordArrayAppend(out_words, word)) {
                    free(word);
                    free(current);
                    shellHistoryWordArrayFree(out_words);
                    return false;
                }
            }
            free(current);
            current = NULL;
            current_len = 0;
            current_cap = 0;
            word_active = false;
            if (at_end) {
                break;
            }
            continue;
        }

        shellBufferAppendChar(&current, &current_len, &current_cap, c);
        word_active = true;
    }

    return true;
}

static char *shellJoinHistoryWords(char **items, size_t start, size_t end) {
    if (!items || start >= end) {
        return strdup("");
    }
    size_t total = 0;
    for (size_t i = start; i < end; ++i) {
        total += strlen(items[i]);
        if (i + 1 < end) {
            total += 1;
        }
    }
    char *result = (char *)malloc(total + 1);
    if (!result) {
        return NULL;
    }
    size_t pos = 0;
    for (size_t i = start; i < end; ++i) {
        size_t len = strlen(items[i]);
        memcpy(result + pos, items[i], len);
        pos += len;
        if (i + 1 < end) {
            result[pos++] = ' ';
        }
    }
    result[pos] = '\0';
    return result;
}

static bool shellHistoryCollectUntil(const char **cursor, char delim, char **out_value) {
    if (!cursor || !*cursor || !out_value) {
        return false;
    }
    const char *p = *cursor;
    char *value = NULL;
    size_t len = 0;
    size_t cap = 0;
    bool escape = false;
    while (*p) {
        char c = *p;
        if (!escape && c == '\\') {
            escape = true;
            p++;
            continue;
        }
        if (!escape && c == delim) {
            *cursor = p + 1;
            if (!value) {
                value = strdup("");
            }
            if (!value) {
                return false;
            }
            *out_value = value;
            return true;
        }
        if (escape) {
            if (c != delim && c != '\\') {
                shellBufferAppendChar(&value, &len, &cap, '\\');
            }
            shellBufferAppendChar(&value, &len, &cap, c);
            escape = false;
        } else {
            shellBufferAppendChar(&value, &len, &cap, c);
        }
        p++;
    }
    free(value);
    return false;
}

static bool shellHistoryParseSubstitutionSpec(const char *spec,
                                              bool *out_is_substitution,
                                              bool *out_global,
                                              char **out_pattern,
                                              char **out_replacement) {
    if (out_is_substitution) {
        *out_is_substitution = false;
    }
    if (out_global) {
        *out_global = false;
    }
    if (out_pattern) {
        *out_pattern = NULL;
    }
    if (out_replacement) {
        *out_replacement = NULL;
    }
    if (!spec) {
        return true;
    }
    const char *cursor = spec;
    bool prefix_global = false;
    if (cursor[0] == 'g' && cursor[1] == 's') {
        prefix_global = true;
        cursor++;
    }
    if (*cursor != 's') {
        return true;
    }
    cursor++;
    if (*cursor == '\0') {
        if (out_is_substitution) {
            *out_is_substitution = true;
        }
        return false;
    }
    char delim = *cursor++;
    char *pattern = NULL;
    if (!shellHistoryCollectUntil(&cursor, delim, &pattern)) {
        if (out_is_substitution) {
            *out_is_substitution = true;
        }
        free(pattern);
        return false;
    }
    char *replacement = NULL;
    if (!shellHistoryCollectUntil(&cursor, delim, &replacement)) {
        if (out_is_substitution) {
            *out_is_substitution = true;
        }
        free(pattern);
        free(replacement);
        return false;
    }
    bool trailing_global = false;
    if (*cursor == 'g') {
        trailing_global = true;
        cursor++;
    }
    if (*cursor != '\0') {
        if (out_is_substitution) {
            *out_is_substitution = true;
        }
        free(pattern);
        free(replacement);
        return false;
    }
    if (out_is_substitution) {
        *out_is_substitution = true;
    }
    if (out_global) {
        *out_global = prefix_global || trailing_global;
    }
    if (out_pattern) {
        *out_pattern = pattern;
    } else {
        free(pattern);
    }
    if (out_replacement) {
        *out_replacement = replacement;
    } else {
        free(replacement);
    }
    return true;
}

static void shellHistoryAppendReplacement(char **buffer,
                                          size_t *length,
                                          size_t *capacity,
                                          const char *replacement,
                                          const char *match_start,
                                          size_t match_len) {
    if (!replacement) {
        return;
    }
    for (size_t i = 0; replacement[i]; ++i) {
        char c = replacement[i];
        if (c == '&') {
            if (match_start && match_len > 0) {
                shellBufferAppendSlice(buffer, length, capacity, match_start, match_len);
            }
            continue;
        }
        if (c == '\\') {
            char next = replacement[i + 1];
            if (next == '\0') {
                shellBufferAppendChar(buffer, length, capacity, '\\');
                continue;
            }
            i++;
            switch (next) {
                case 't':
                    shellBufferAppendChar(buffer, length, capacity, '\t');
                    break;
                case 'n':
                    shellBufferAppendChar(buffer, length, capacity, '\n');
                    break;
                case '\\':
                    shellBufferAppendChar(buffer, length, capacity, '\\');
                    break;
                case '&':
                    shellBufferAppendChar(buffer, length, capacity, '&');
                    break;
                default:
                    shellBufferAppendChar(buffer, length, capacity, next);
                    break;
            }
            continue;
        }
        shellBufferAppendChar(buffer, length, capacity, c);
    }
}

static char *shellHistoryApplyRegexSubstitution(const char *entry,
                                                const char *pattern,
                                                const char *replacement,
                                                bool global) {
    if (!entry || !pattern || !replacement) {
        return NULL;
    }
    regex_t regex;
    int rc = regcomp(&regex, pattern, REG_EXTENDED);
    if (rc != 0) {
        return NULL;
    }

    char *result = NULL;
    size_t length = 0;
    size_t capacity = 0;
    const char *cursor = entry;
    bool replaced = false;

    while (cursor && *cursor) {
        regmatch_t match;
        int flags = (cursor != entry) ? REG_NOTBOL : 0;
        rc = regexec(&regex, cursor, 1, &match, flags);
        if (rc != 0) {
            shellBufferAppendString(&result, &length, &capacity, cursor);
            break;
        }
        replaced = true;
        size_t match_len = (size_t)(match.rm_eo - match.rm_so);
        shellBufferAppendSlice(&result, &length, &capacity, cursor, (size_t)match.rm_so);
        const char *match_start = cursor + match.rm_so;
        shellHistoryAppendReplacement(&result, &length, &capacity, replacement, match_start, match_len);
        cursor += match.rm_eo;
        if (!global) {
            shellBufferAppendString(&result, &length, &capacity, cursor);
            break;
        }
        if (match_len == 0) {
            if (*cursor == '\0') {
                break;
            }
            shellBufferAppendChar(&result, &length, &capacity, *cursor);
            cursor++;
        }
    }

    if (!replaced) {
        if (result) {
            free(result);
        }
        result = strdup(entry);
    }

    regfree(&regex);
    return result;
}

static bool shellApplyHistoryDesignator(const char *entry, const char *designator, size_t len, char **out_line) {
    if (out_line) {
        *out_line = NULL;
    }
    if (!entry || !out_line) {
        return false;
    }
    if (!designator || len == 0) {
        *out_line = strdup(entry);
        return *out_line != NULL;
    }

    char *spec = strndup(designator, len);
    if (!spec) {
        return false;
    }

    ShellHistoryWordArray words = {0};
    if (!shellTokenizeHistoryEntry(entry, &words)) {
        free(spec);
        return false;
    }

    bool success = true;
    char *result = NULL;

    bool is_substitution = false;
    bool substitution_global = false;
    char *sub_pattern = NULL;
    char *sub_replacement = NULL;
    if (!shellHistoryParseSubstitutionSpec(spec, &is_substitution, &substitution_global, &sub_pattern, &sub_replacement)) {
        success = false;
    } else if (is_substitution) {
        result = shellHistoryApplyRegexSubstitution(entry,
                                                    sub_pattern ? sub_pattern : "",
                                                    sub_replacement ? sub_replacement : "",
                                                    substitution_global);
        if (!result) {
            success = false;
        }
    } else if (strcmp(spec, "*") == 0) {
        if (words.count <= 1) {
            result = strdup("");
        } else {
            result = shellJoinHistoryWords(words.items, 1, words.count);
        }
    } else if (strcmp(spec, "^") == 0) {
        if (words.count <= 1) {
            success = false;
        } else {
            result = strdup(words.items[1]);
        }
    } else if (strcmp(spec, "$") == 0) {
        if (words.count == 0) {
            success = false;
        } else {
            result = strdup(words.items[words.count - 1]);
        }
    } else {
        char *endptr = NULL;
        long index = strtol(spec, &endptr, 10);
        if (endptr && *endptr == '\0') {
            if (index < 0 || (size_t)index >= words.count) {
                success = false;
            } else {
                result = strdup(words.items[index]);
            }
        } else {
            success = false;
        }
    }

    free(spec);
    free(sub_pattern);
    free(sub_replacement);

    if (!success || !result) {
        free(result);
        shellHistoryWordArrayFree(&words);
        return false;
    }

    *out_line = result;
    shellHistoryWordArrayFree(&words);
    return true;
}

static const char *shellHistoryEntryByIndex(long index) {
    if (gShellHistory.count == 0 || index == 0) {
        return NULL;
    }
    if (index > 0) {
        if ((size_t)index > gShellHistory.count) {
            return NULL;
        }
        return gShellHistory.entries[index - 1];
    }
    size_t offset = (size_t)(-index);
    if (offset == 0 || offset > gShellHistory.count) {
        return NULL;
    }
    return gShellHistory.entries[gShellHistory.count - offset];
}

static const char *shellHistoryFindByPrefix(const char *prefix, size_t len) {
    if (!prefix || len == 0) {
        return NULL;
    }
    for (size_t i = gShellHistory.count; i > 0; --i) {
        const char *entry = gShellHistory.entries[i - 1];
        if (!entry) {
            continue;
        }
        const char *trimmed = entry;
        while (*trimmed == ' ' || *trimmed == '\t') {
            trimmed++;
        }
        if (strncmp(trimmed, prefix, len) == 0) {
            char next = trimmed[len];
            if (next == '\0' || next == ' ' || next == '\t') {
                return entry;
            }
        }
    }
    return NULL;
}

static const char *shellHistoryFindBySubstring(const char *needle, size_t len) {
    if (!needle || len == 0) {
        return NULL;
    }
    char *pattern = strndup(needle, len);
    if (!pattern) {
        return NULL;
    }
    for (size_t i = gShellHistory.count; i > 0; --i) {
        const char *entry = gShellHistory.entries[i - 1];
        if (entry && strstr(entry, pattern)) {
            free(pattern);
            return entry;
        }
    }
    free(pattern);
    return NULL;
}

static const char *shellHistoryFindByRegex(const char *pattern, size_t len, bool *out_invalid) {
    if (out_invalid) {
        *out_invalid = false;
    }
    if (!pattern || len == 0) {
        return NULL;
    }
    char *expr = strndup(pattern, len);
    if (!expr) {
        if (out_invalid) {
            *out_invalid = true;
        }
        return NULL;
    }
    regex_t regex;
    int rc = regcomp(&regex, expr, REG_EXTENDED | REG_NOSUB);
    free(expr);
    if (rc != 0) {
        if (out_invalid) {
            *out_invalid = true;
        }
        return NULL;
    }
    const char *result = NULL;
    for (size_t i = gShellHistory.count; i > 0; --i) {
        const char *entry = gShellHistory.entries[i - 1];
        if (entry && regexec(&regex, entry, 0, NULL, 0) == 0) {
            result = entry;
            break;
        }
    }
    regfree(&regex);
    return result;
}

static char *shellJoinArrayValuesWithSeparator(char **items, size_t count, char separator);

static char *shellJoinPositionalParameters(bool array_style, size_t *out_count) {
    if (out_count) {
        *out_count = (gParamCount > 0) ? (size_t)gParamCount : 0;
    }
    if (gParamCount <= 0 || !gParamValues) {
        return strdup("");
    }
    char separator = array_style ? SHELL_ARRAY_ELEMENT_SEP : ' ';
    return shellJoinArrayValuesWithSeparator(gParamValues,
                                            (size_t)gParamCount,
                                            separator);
}

static char *shellLookupParameterValueInternal(const char *name,
                                               size_t len,
                                               bool *out_is_set) {
    if (out_is_set) {
        *out_is_set = false;
    }
    if (!name || len == 0) {
        if (out_is_set) {
            *out_is_set = true;
        }
        return strdup("");
    }
    if (len == 1) {
        switch (name[0]) {
            case '?': {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d", gShellRuntime.last_status);
                if (out_is_set) {
                    *out_is_set = true;
                }
                return strdup(buffer);
            }
            case '$': {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d", (int)getpid());
                if (out_is_set) {
                    *out_is_set = true;
                }
                return strdup(buffer);
            }
            case '#': {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d", gParamCount);
                if (out_is_set) {
                    *out_is_set = true;
                }
                return strdup(buffer);
            }
            case '*':
            case '@': {
                if (out_is_set) {
                    *out_is_set = gParamCount > 0;
                }
                size_t ignored = 0;
                return shellJoinPositionalParameters(name[0] == '@', &ignored);
            }
            case '0': {
                if (out_is_set) {
                    *out_is_set = true;
                }
                if (gShellArg0) {
                    return strdup(gShellArg0);
                }
                return strdup("exsh");
            }
            default:
                break;
        }
    }

    if (len == 6 && strncmp(name, "RANDOM", 6) == 0) {
        unsigned int value = shellRandomNextValue();
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "%u", value);
        if (out_is_set) {
            *out_is_set = true;
        }
        return strdup(buffer);
    }

    bool numeric = true;
    for (size_t i = 0; i < len; ++i) {
        if (!isdigit((unsigned char)name[i])) {
            numeric = false;
            break;
        }
    }
    if (numeric) {
        long index = strtol(name, NULL, 10);
        bool have_value = (index >= 1 && index <= gParamCount && gParamValues);
        const char *value = "";
        if (have_value) {
            value = gParamValues[index - 1] ? gParamValues[index - 1] : "";
        }
        if (out_is_set) {
            *out_is_set = have_value;
        }
        return strdup(value);
    }

    char *key = (char *)malloc(len + 1);
    if (!key) {
        return NULL;
    }
    memcpy(key, name, len);
    key[len] = '\0';
    const ShellArrayVariable *array_var = shellArrayRegistryFindConst(key);
    if (array_var) {
        if (out_is_set) {
            *out_is_set = true;
        }
        const char *first = (array_var->count > 0 && array_var->values)
                                ? (array_var->values[0] ? array_var->values[0] : "")
                                : "";
        char *result = strdup(first ? first : "");
        free(key);
        return result;
    }
    char *local_value = NULL;
    bool local_is_array = false;
    if (shellLocalScopeGetCopy(key, &local_value, &local_is_array)) {
        if (out_is_set) {
            *out_is_set = true;
        }
        free(key);
        return local_value ? local_value : strdup("");
    }
    const char *env = shellSafeGetenv(key);
    if (out_is_set) {
        *out_is_set = (env != NULL);
    }
    free(key);
    if (!env) {
        return strdup("");
    }
    return strdup(env);
}

static char *shellLookupParameterValue(const char *name, size_t len) {
    return shellLookupParameterValueInternal(name, len, NULL);
}

static void shellFreeArrayValues(char **items, size_t count) {
    if (!items) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(items[i]);
    }
    free(items);
}

static bool shellAppendArrayValue(char ***items, size_t *count, size_t *capacity, char *value) {
    if (!items || !count || !capacity || !value) {
        free(value);
        return false;
    }
    if (*count == *capacity) {
        size_t new_capacity = *capacity ? (*capacity * 2) : 4;
        char **expanded = realloc(*items, new_capacity * sizeof(char *));
        if (!expanded) {
            free(value);
            return false;
        }
        *items = expanded;
        *capacity = new_capacity;
    }
    (*items)[(*count)++] = value;
    return true;
}

static char *shellParseNextArrayToken(char **cursor_ptr) {
    if (!cursor_ptr || !*cursor_ptr) {
        return NULL;
    }
    char *cursor = *cursor_ptr;
    char *token = NULL;
    size_t length = 0;
    size_t capacity = 0;
    while (*cursor) {
        unsigned char ch = (unsigned char)*cursor;
        if (isspace(ch)) {
            break;
        }
        if (ch == '\\') {
            if (cursor[1]) {
                shellBufferAppendChar(&token, &length, &capacity, cursor[1]);
                cursor += 2;
            } else {
                cursor++;
            }
            continue;
        }
        if (ch == '\'' || ch == '"') {
            char quote = (char)ch;
            cursor++;
            while (*cursor && *cursor != quote) {
                if (quote == '"' && *cursor == '\\' && cursor[1]) {
                    shellBufferAppendChar(&token, &length, &capacity, cursor[1]);
                    cursor += 2;
                } else {
                    shellBufferAppendChar(&token, &length, &capacity, *cursor);
                    cursor++;
                }
            }
            if (*cursor == quote) {
                cursor++;
            }
            continue;
        }
        shellBufferAppendChar(&token, &length, &capacity, (char)ch);
        cursor++;
    }
    if (!token) {
        token = strdup("");
        if (!token) {
            return NULL;
        }
    }
    *cursor_ptr = cursor;
    return token;
}

static bool shellParseArrayValues(const char *value, char ***out_items, size_t *out_count) {
    if (out_items) {
        *out_items = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!value) {
        return true;
    }
    const char *start = value;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    const char *end = value + strlen(value);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    if (end > start && *start == '(' && end[-1] == ')') {
        start++;
        end--;
        while (start < end && isspace((unsigned char)*start)) {
            start++;
        }
        while (end > start && isspace((unsigned char)end[-1])) {
            end--;
        }
    }
    size_t span = (size_t)(end - start);
    if (span == 0) {
        return true;
    }
    char *copy = (char *)malloc(span + 1);
    if (!copy) {
        return false;
    }
    memcpy(copy, start, span);
    copy[span] = '\0';

    char *cursor = copy;
    char **items = NULL;
    size_t count = 0;
    size_t capacity = 0;
    while (*cursor) {
        while (*cursor && isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        char *token = shellParseNextArrayToken(&cursor);
        if (!token) {
            free(copy);
            shellFreeArrayValues(items, count);
            return false;
        }
        if (!shellAppendArrayValue(&items, &count, &capacity, token)) {
            free(copy);
            shellFreeArrayValues(items, count);
            return false;
        }
        while (*cursor && isspace((unsigned char)*cursor)) {
            cursor++;
        }
    }
    free(copy);
    if (out_items) {
        *out_items = items;
    } else {
        shellFreeArrayValues(items, count);
    }
    if (out_count) {
        *out_count = count;
    }
    return true;
}

static char *shellDecodeAssociativeKey(const char *text, size_t len) {
    if (!text) {
        return strdup("");
    }
    if (len == (size_t)-1) {
        len = strlen(text);
    }
    if (len >= 2 && text[0] == '"' && text[len - 1] == '"') {
        size_t inner_len = len - 2;
        char *decoded = (char *)malloc(inner_len + 1);
        if (!decoded) {
            return NULL;
        }
        size_t out_index = 0;
        for (size_t i = 1; i + 1 < len; ++i) {
            char ch = text[i];
            if (ch == '\\' && i + 1 < len - 1) {
                decoded[out_index++] = text[++i];
            } else {
                decoded[out_index++] = ch;
            }
        }
        decoded[out_index] = '\0';
        return decoded;
    }
    if (len >= 2 && text[0] == '\'' && text[len - 1] == '\'') {
        size_t inner_len = len - 2;
        char *decoded = (char *)malloc(inner_len + 1);
        if (!decoded) {
            return NULL;
        }
        if (inner_len > 0) {
            memcpy(decoded, text + 1, inner_len);
        }
        decoded[inner_len] = '\0';
        return decoded;
    }
    char *decoded = (char *)malloc(len + 1);
    if (!decoded) {
        return NULL;
    }
    size_t out_index = 0;
    for (size_t i = 0; i < len; ++i) {
        char ch = text[i];
        if (ch == '\\' && i + 1 < len) {
            decoded[out_index++] = text[++i];
        } else {
            decoded[out_index++] = ch;
        }
    }
    decoded[out_index] = '\0';
    return decoded;
}

static bool shellParseAssociativeArrayLiteral(const char *value,
                                              char ***out_keys,
                                              char ***out_values,
                                              size_t *out_count) {
    if (out_keys) {
        *out_keys = NULL;
    }
    if (out_values) {
        *out_values = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!value) {
        return true;
    }
    const char *start = value;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    const char *end = value + strlen(value);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    if (end > start && *start == '(' && end[-1] == ')') {
        start++;
        end--;
        while (start < end && isspace((unsigned char)*start)) {
            start++;
        }
        while (end > start && isspace((unsigned char)end[-1])) {
            end--;
        }
    }
    size_t span = (size_t)(end - start);
    if (span == 0) {
        return true;
    }
    char *copy = (char *)malloc(span + 1);
    if (!copy) {
        return false;
    }
    memcpy(copy, start, span);
    copy[span] = '\0';

    char *cursor = copy;
    char **keys = NULL;
    char **values = NULL;
    size_t key_count = 0;
    size_t value_count = 0;
    size_t key_capacity = 0;
    size_t value_capacity = 0;

    while (*cursor) {
        while (*cursor && isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        if (*cursor != '[') {
            shellFreeArrayValues(keys, key_count);
            shellFreeArrayValues(values, value_count);
            free(copy);
            return false;
        }
        cursor++;
        char *key_start = cursor;
        bool in_single = false;
        bool in_double = false;
        while (*cursor) {
            char ch = *cursor;
            if (ch == '\\' && !in_single && cursor[1]) {
                cursor += 2;
                continue;
            }
            if (ch == '\'' && !in_double) {
                in_single = !in_single;
                cursor++;
                continue;
            }
            if (ch == '"' && !in_single) {
                in_double = !in_double;
                cursor++;
                continue;
            }
            if (!in_single && !in_double && ch == ']') {
                break;
            }
            cursor++;
        }
        if (*cursor != ']') {
            shellFreeArrayValues(keys, key_count);
            shellFreeArrayValues(values, value_count);
            free(copy);
            return false;
        }
        char *key_end = cursor;
        size_t raw_key_len = (size_t)(key_end - key_start);
        char *decoded_key = shellDecodeAssociativeKey(key_start, raw_key_len);
        if (!decoded_key) {
            shellFreeArrayValues(keys, key_count);
            shellFreeArrayValues(values, value_count);
            free(copy);
            return false;
        }
        cursor++;
        while (*cursor && isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor != '=') {
            free(decoded_key);
            shellFreeArrayValues(keys, key_count);
            shellFreeArrayValues(values, value_count);
            free(copy);
            return false;
        }
        cursor++;
        while (*cursor && isspace((unsigned char)*cursor)) {
            cursor++;
        }
        char *value_cursor = cursor;
        char *token = shellParseNextArrayToken(&value_cursor);
        if (!token) {
            free(decoded_key);
            shellFreeArrayValues(keys, key_count);
            shellFreeArrayValues(values, value_count);
            free(copy);
            return false;
        }
        cursor = value_cursor;
        if (!shellAppendArrayValue(&keys, &key_count, &key_capacity, decoded_key)) {
            free(token);
            shellFreeArrayValues(keys, key_count);
            shellFreeArrayValues(values, value_count);
            free(copy);
            return false;
        }
        if (!shellAppendArrayValue(&values, &value_count, &value_capacity, token)) {
            shellFreeArrayValues(keys, key_count);
            shellFreeArrayValues(values, value_count);
            free(copy);
            return false;
        }
    }

    while (*cursor && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != '\0') {
        shellFreeArrayValues(keys, key_count);
        shellFreeArrayValues(values, value_count);
        free(copy);
        return false;
    }

    free(copy);
    if (out_keys) {
        *out_keys = keys;
    } else {
        shellFreeArrayValues(keys, key_count);
    }
    if (out_values) {
        *out_values = values;
    } else {
        shellFreeArrayValues(values, value_count);
    }
    if (out_count) {
        *out_count = value_count;
    }
    return true;
}

static bool shellParseArrayLiteral(const char *value,
                                   char ***out_items,
                                   char ***out_keys,
                                   size_t *out_count,
                                   ShellArrayKind *out_kind) {
    if (out_items) {
        *out_items = NULL;
    }
    if (out_keys) {
        *out_keys = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (out_kind) {
        *out_kind = SHELL_ARRAY_KIND_INDEXED;
    }
    if (!value) {
        return true;
    }
    bool looks_associative = false;
    for (const char *cursor = value; *cursor; ++cursor) {
        if (*cursor == '[') {
            const char *closing = strchr(cursor + 1, ']');
            if (!closing) {
                break;
            }
            const char *after = closing + 1;
            while (*after && isspace((unsigned char)*after)) {
                after++;
            }
            if (*after == '=') {
                looks_associative = true;
                break;
            }
            cursor = closing;
        }
    }
    if (looks_associative) {
        char **keys = NULL;
        char **items = NULL;
        size_t count = 0;
        if (!shellParseAssociativeArrayLiteral(value, &keys, &items, &count)) {
            return false;
        }
        if (out_items) {
            *out_items = items;
        } else {
            shellFreeArrayValues(items, count);
        }
        if (out_keys) {
            *out_keys = keys;
        } else {
            shellFreeArrayValues(keys, count);
        }
        if (out_count) {
            *out_count = count;
        }
        if (out_kind) {
            *out_kind = SHELL_ARRAY_KIND_ASSOCIATIVE;
        }
        return true;
    }

    char **items = NULL;
    size_t count = 0;
    if (!shellParseArrayValues(value, &items, &count)) {
        return false;
    }
    if (out_items) {
        *out_items = items;
    } else {
        shellFreeArrayValues(items, count);
    }
    if (out_count) {
        *out_count = count;
    }
    if (out_kind) {
        *out_kind = SHELL_ARRAY_KIND_INDEXED;
    }
    if (out_keys) {
        *out_keys = NULL;
    }
    return true;
}

static bool shellSubscriptIsNumeric(const char *text) {
    if (!text) {
        return false;
    }
    const char *cursor = text;
    while (*cursor && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor == '\0') {
        return false;
    }
    while (*cursor) {
        if (isspace((unsigned char)*cursor)) {
            while (*cursor && isspace((unsigned char)*cursor)) {
                cursor++;
            }
            if (*cursor == '\0') {
                return true;
            }
            return false;
        }
        if (!isdigit((unsigned char)*cursor)) {
            return false;
        }
        cursor++;
    }
    return true;
}

static void shellBufferAppendQuoted(char **buffer,
                                    size_t *length,
                                    size_t *capacity,
                                    const char *text) {
    shellBufferAppendChar(buffer, length, capacity, '"');
    if (text) {
        for (const char *cursor = text; *cursor; ++cursor) {
            unsigned char ch = (unsigned char)*cursor;
            if (ch == '"' || ch == '\\') {
                shellBufferAppendChar(buffer, length, capacity, '\\');
            }
            shellBufferAppendChar(buffer, length, capacity, (char)ch);
        }
    }
    shellBufferAppendChar(buffer, length, capacity, '"');
}

static char *shellBuildArrayLiteral(const ShellArrayVariable *var) {
    char *buffer = NULL;
    size_t length = 0;
    size_t capacity = 0;
    shellBufferAppendChar(&buffer, &length, &capacity, '(');
    if (var) {
        for (size_t i = 0; i < var->count; ++i) {
            if (i > 0) {
                shellBufferAppendChar(&buffer, &length, &capacity, ' ');
            }
            const char *value = (var->values && var->values[i]) ? var->values[i] : "";
            if (var->kind == SHELL_ARRAY_KIND_ASSOCIATIVE) {
                const char *key = (var->keys && var->keys[i]) ? var->keys[i] : "";
                shellBufferAppendChar(&buffer, &length, &capacity, '[');
                shellBufferAppendQuoted(&buffer, &length, &capacity, key);
                shellBufferAppendChar(&buffer, &length, &capacity, ']');
                shellBufferAppendChar(&buffer, &length, &capacity, '=');
                shellBufferAppendQuoted(&buffer, &length, &capacity, value);
            } else {
                shellBufferAppendQuoted(&buffer, &length, &capacity, value);
            }
        }
    }
    shellBufferAppendChar(&buffer, &length, &capacity, ')');
    if (!buffer) {
        return strdup("()");
    }
    return buffer;
}

static void shellArrayRegistryAssignFromText(const char *name, const char *value) {
    if (!name) {
        return;
    }
    if (!value) {
        shellArrayRegistryRemove(name);
        return;
    }
    char **items = NULL;
    char **keys = NULL;
    size_t count = 0;
    ShellArrayKind kind = SHELL_ARRAY_KIND_INDEXED;
    if (!shellParseArrayLiteral(value, &items, &keys, &count, &kind)) {
        shellArrayRegistryRemove(name);
        return;
    }
    if (!shellArrayRegistryStore(name, items, keys, count, kind)) {
        shellArrayRegistryRemove(name);
    }
    shellFreeArrayValues(items, count);
    if (keys) {
        shellFreeArrayValues(keys, count);
    }
}

static bool shellArrayRegistryInitializeAssociative(const char *name) {
    if (!name) {
        return false;
    }
    if (!shellAssociativeArraysSupported()) {
        errno = EINVAL;
        return false;
    }
    ShellArrayVariable *var = shellArrayRegistryFindMutable(name);
    if (!var) {
        if (!shellArrayRegistryEnsureCapacity(gShellArrayVarCount + 1)) {
            return false;
        }
        var = &gShellArrayVars[gShellArrayVarCount++];
        var->name = strdup(name);
        if (!var->name) {
            gShellArrayVarCount--;
            return false;
        }
        var->values = NULL;
        var->keys = NULL;
        var->count = 0;
    } else {
        shellArrayVariableClear(var);
    }
    var->kind = SHELL_ARRAY_KIND_ASSOCIATIVE;
    return true;
}

static bool shellReadonlyEnsureCapacity(size_t needed) {
    if (gShellReadonlyVarCapacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellReadonlyVarCapacity ? (gShellReadonlyVarCapacity * 2) : 8;
    if (new_capacity < needed) {
        new_capacity = needed;
    }
    if (new_capacity > SIZE_MAX / sizeof(ShellReadonlyEntry)) {
        return false;
    }
    ShellReadonlyEntry *resized =
        (ShellReadonlyEntry *)realloc(gShellReadonlyVars, new_capacity * sizeof(ShellReadonlyEntry));
    if (!resized) {
        return false;
    }
    for (size_t i = gShellReadonlyVarCapacity; i < new_capacity; ++i) {
        resized[i].name = NULL;
    }
    gShellReadonlyVars = resized;
    gShellReadonlyVarCapacity = new_capacity;
    return true;
}

static ShellReadonlyEntry *shellReadonlyFindMutable(const char *name) {
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < gShellReadonlyVarCount; ++i) {
        ShellReadonlyEntry *entry = &gShellReadonlyVars[i];
        if (entry->name && strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

static bool shellReadonlyContains(const char *name) {
    return shellReadonlyFindMutable(name) != NULL;
}

static bool shellReadonlyAdd(const char *name) {
    if (!name || *name == '\0') {
        return false;
    }
    if (shellReadonlyContains(name)) {
        return true;
    }
    if (!shellReadonlyEnsureCapacity(gShellReadonlyVarCount + 1)) {
        return false;
    }
    ShellReadonlyEntry *entry = &gShellReadonlyVars[gShellReadonlyVarCount++];
    entry->name = strdup(name);
    if (!entry->name) {
        gShellReadonlyVarCount--;
        return false;
    }
    return true;
}

static void shellReadonlySetErrorName(const char *name) {
    if (gShellReadonlyErrorName) {
        free(gShellReadonlyErrorName);
        gShellReadonlyErrorName = NULL;
    }
    if (name && *name) {
        gShellReadonlyErrorName = strdup(name);
    }
}

static const char *shellReadonlyGetErrorName(void) {
    return gShellReadonlyErrorName ? gShellReadonlyErrorName : "";
}

static int shellCompareStringPointers(const void *lhs, const void *rhs) {
    const char *const *left = (const char *const *)lhs;
    const char *const *right = (const char *const *)rhs;
    const char *l = (left && *left) ? *left : "";
    const char *r = (right && *right) ? *right : "";
    return strcmp(l, r);
}

static void shellReadonlyPrintVariables(void) {
    if (gShellReadonlyVarCount == 0) {
        return;
    }
    char **names = (char **)malloc(gShellReadonlyVarCount * sizeof(char *));
    if (names) {
        for (size_t i = 0; i < gShellReadonlyVarCount; ++i) {
            names[i] = gShellReadonlyVars[i].name;
        }
        qsort(names, gShellReadonlyVarCount, sizeof(char *), shellCompareStringPointers);
    }

    for (size_t i = 0; i < gShellReadonlyVarCount; ++i) {
        const char *name = names ? names[i] : gShellReadonlyVars[i].name;
        if (!name) {
            continue;
        }
        const char *value = shellSafeGetenv(name);
        if (!value) {
            printf("readonly %s\n", name);
            continue;
        }
        printf("readonly %s=\"", name);
        for (const char *cursor = value; *cursor; ++cursor) {
            unsigned char ch = (unsigned char)*cursor;
            if (ch == '"' || ch == '\\') {
                putchar('\\');
            }
            putchar(ch);
        }
        printf("\"\n");
    }

    if (names) {
        free(names);
    }
}

static bool shellArrayRegistrySetElement(const char *name,
                                         const char *subscript,
                                         const char *value) {
    if (!name || !subscript) {
        return false;
    }
    shellReadonlySetErrorName(NULL);
    if (shellReadonlyContains(name)) {
        errno = EPERM;
        shellReadonlySetErrorName(name);
        return false;
    }
    const char *text_value = value ? value : "";
    const char *trim_start = subscript;
    while (*trim_start && isspace((unsigned char)*trim_start)) {
        trim_start++;
    }
    size_t sub_len = strlen(trim_start);
    while (sub_len > 0 && isspace((unsigned char)trim_start[sub_len - 1])) {
        sub_len--;
    }
    char *sub_copy = (char *)malloc(sub_len + 1);
    if (!sub_copy) {
        return false;
    }
    memcpy(sub_copy, trim_start, sub_len);
    sub_copy[sub_len] = '\0';

    ShellArrayVariable *var = shellArrayRegistryFindMutable(name);
    ShellArrayKind target_kind;
    if (var) {
        target_kind = var->kind;
    } else {
        target_kind = shellSubscriptIsNumeric(sub_copy) ? SHELL_ARRAY_KIND_INDEXED
                                                        : SHELL_ARRAY_KIND_ASSOCIATIVE;
        if (target_kind == SHELL_ARRAY_KIND_ASSOCIATIVE &&
            !shellAssociativeArraysSupported()) {
            free(sub_copy);
            errno = EINVAL;
            return false;
        }
        if (!shellArrayRegistryEnsureCapacity(gShellArrayVarCount + 1)) {
            free(sub_copy);
            return false;
        }
        var = &gShellArrayVars[gShellArrayVarCount++];
        var->name = strdup(name);
        if (!var->name) {
            gShellArrayVarCount--;
            free(sub_copy);
            return false;
        }
        var->values = NULL;
        var->keys = NULL;
        var->count = 0;
        var->kind = target_kind;
    }

    if (var->kind == SHELL_ARRAY_KIND_ASSOCIATIVE && target_kind != SHELL_ARRAY_KIND_ASSOCIATIVE) {
        free(sub_copy);
        return false;
    }
    if (var->kind == SHELL_ARRAY_KIND_INDEXED && target_kind != SHELL_ARRAY_KIND_INDEXED) {
        free(sub_copy);
        return false;
    }

    bool ok = true;
    if (var->kind == SHELL_ARRAY_KIND_ASSOCIATIVE) {
        if (!shellAssociativeArraysSupported()) {
            free(sub_copy);
            errno = EINVAL;
            return false;
        }
        char *decoded_key = shellDecodeAssociativeKey(sub_copy, strlen(sub_copy));
        free(sub_copy);
        if (!decoded_key) {
            return false;
        }
        size_t index = SIZE_MAX;
        for (size_t i = 0; i < var->count; ++i) {
            const char *existing = (var->keys && var->keys[i]) ? var->keys[i] : "";
            if (strcmp(existing, decoded_key) == 0) {
                index = i;
                break;
            }
        }
        char *dup_value = strdup(text_value);
        if (!dup_value) {
            free(decoded_key);
            return false;
        }
        if (index == SIZE_MAX) {
            char **new_values = (char **)realloc(var->values, (var->count + 1) * sizeof(char *));
            if (!new_values) {
                free(decoded_key);
                free(dup_value);
                return false;
            }
            var->values = new_values;
            char **new_keys = (char **)realloc(var->keys, (var->count + 1) * sizeof(char *));
            if (!new_keys) {
                free(decoded_key);
                free(dup_value);
                return false;
            }
            var->keys = new_keys;
            var->values[var->count] = dup_value;
            var->keys[var->count] = decoded_key;
            var->count++;
        } else {
            free(var->values[index]);
            var->values[index] = dup_value;
            free(decoded_key);
        }
    } else {
        char *endptr = NULL;
        long parsed = strtol(sub_copy, &endptr, 10);
        free(sub_copy);
        if (!endptr || *endptr != '\0' || parsed < 0) {
            return false;
        }
        size_t index = (size_t)parsed;
        if (index >= var->count) {
            size_t old_count = var->count;
            char **resized = (char **)realloc(var->values, (index + 1) * sizeof(char *));
            if (!resized) {
                return false;
            }
            var->values = resized;
            for (size_t i = old_count; i <= index; ++i) {
                var->values[i] = NULL;
            }
            for (size_t i = old_count; i <= index; ++i) {
                var->values[i] = strdup("");
                if (!var->values[i]) {
                    for (size_t j = old_count; j < i; ++j) {
                        free(var->values[j]);
                        var->values[j] = NULL;
                    }
                    var->values = (char **)realloc(var->values, old_count * sizeof(char *));
                    var->count = old_count;
                    return false;
                }
            }
            var->count = index + 1;
        }
        char *dup_value = strdup(text_value);
        if (!dup_value) {
            return false;
        }
        free(var->values[index]);
        var->values[index] = dup_value;
    }

    char *literal = shellBuildArrayLiteral(var);
    if (literal) {
        setenv(name, literal, 1);
        free(literal);
    } else {
        setenv(name, "", 1);
    }
    return ok;
}

static bool shellSetTrackedVariable(const char *name, const char *value, bool is_array_literal) {
    if (!name) {
        return false;
    }
    shellReadonlySetErrorName(NULL);
    if (shellReadonlyContains(name)) {
        errno = EPERM;
        shellReadonlySetErrorName(name);
        return false;
    }
    ShellLocalVariable *local_entry = shellLocalScopeFind(name);
    if (!local_entry &&
        (gShellRuntime.pipeline_subshell_active || gShellRuntime.local_scope_active) &&
        shellLocalScopeDepth() > 0) {
        if (!shellLocalScopeSetOnCurrentFrame(name, value ? value : "", is_array_literal)) {
            errno = ENOMEM;
            return false;
        }
        local_entry = shellLocalScopeFind(name);
    }
    if (local_entry) {
        if (shellLocalScopeAssignExisting(name, value ? value : "", is_array_literal)) {
            if (is_array_literal) {
                const char *local_text = local_entry->value ? local_entry->value : "";
                shellArrayRegistryAssignFromText(name, local_text);
            } else {
                shellArrayRegistryRemove(name);
            }
            if (strcmp(name, "EXSH_LAST_STATUS") == 0) {
                const char *text = value ? value : "";
                if (setenv(name, text, 1) != 0) {
                    return false;
                }
            }
            return true;
        }
        errno = ENOMEM;
        return false;
    }
    const char *text = value ? value : "";
    if (setenv(name, text, 1) != 0) {
        return false;
    }
    if (is_array_literal) {
        const char *current = shellSafeGetenv(name);
        shellArrayRegistryAssignFromText(name, current);
    } else {
        shellArrayRegistryRemove(name);
    }
    return true;
}

static void shellUnsetTrackedVariable(const char *name) {
    if (!name) {
        return;
    }
    if (shellLocalScopeUnset(name)) {
        return;
    }
    unsetenv(name);
    shellArrayRegistryRemove(name);
}

static void shellDirectoryStackReportError(VM *vm, const char *builtin, const char *message) {
    const char *label = (builtin && *builtin) ? builtin : "shell";
    const char *detail = (message && *message) ? message : "unknown error";
    if (vm) {
        runtimeError(vm, "%s: %s", label, detail);
    } else {
        fprintf(stderr, "%s: %s\n", label, detail);
    }
}

static void shellDirectoryStackReportErrno(VM *vm, const char *builtin) {
    shellDirectoryStackReportError(vm, builtin, strerror(errno));
}

static bool shellDirectoryStackUpdateEnvironment(const char *old_cwd, const char *new_cwd);

static bool shellDirectoryStackEnsureCapacity(size_t needed) {
    if (gShellRuntime.dir_stack_capacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellRuntime.dir_stack_capacity ? (gShellRuntime.dir_stack_capacity * 2) : 4;
    if (new_capacity < needed) {
        new_capacity = needed;
    }
    if (new_capacity > SIZE_MAX / sizeof(char *)) {
        return false;
    }
    char **resized = (char **)realloc(gShellRuntime.dir_stack, new_capacity * sizeof(char *));
    if (!resized) {
        return false;
    }
    for (size_t i = gShellRuntime.dir_stack_capacity; i < new_capacity; ++i) {
        resized[i] = NULL;
    }
    gShellRuntime.dir_stack = resized;
    gShellRuntime.dir_stack_capacity = new_capacity;
    return true;
}

static bool shellDirectoryStackEnsureInitialised(VM *vm, const char *builtin) {
    if (gShellRuntime.dir_stack_initialised && gShellRuntime.dir_stack_count > 0) {
        return true;
    }
    if (gShellRuntime.dir_stack_count > 0) {
        gShellRuntime.dir_stack_initialised = true;
        return true;
    }
    char cwd[PATH_MAX];
    const char *initial_path = NULL;
    if (SHELL_GETCWD(cwd, sizeof(cwd))) {
        initial_path = cwd;
    }
    if (!initial_path || *initial_path == '\0') {
        const char *home = shellSafeGetenv("PSCALI_REAL_HOME");
        if (!home || !*home) {
            home = shellSafeGetenv("HOME");
        }
        initial_path = (home && *home) ? home : ".";
    }
    char resolved[PATH_MAX];
    const char *expanded_path = initial_path;
#if defined(PSCAL_TARGET_IOS)
    char virtual_expanded[PATH_MAX];
    if (pathTruncateExpand(initial_path, virtual_expanded, sizeof(virtual_expanded))) {
        expanded_path = virtual_expanded;
    }
#endif
    const char *source_path = expanded_path;
    if (realpath(expanded_path, resolved)) {
        source_path = resolved;
    }
#if defined(PSCAL_TARGET_IOS)
    {
        char logBuf[PATH_MAX * 2];
        snprintf(logBuf, sizeof(logBuf),
                 "[exsh] init dir stack: PSCALI_REAL_HOME=%s HOME=%s expanded=%s resolved=%s",
                 shellSafeGetenv("PSCALI_REAL_HOME") ? shellSafeGetenv("PSCALI_REAL_HOME") : "(null)",
                 shellSafeGetenv("HOME") ? shellSafeGetenv("HOME") : "(null)",
                 expanded_path,
                 source_path);
        pscalRuntimeDebugLog(logBuf);
    }
#endif
    char *copy = strdup(source_path);
    if (!copy) {
        shellDirectoryStackReportError(vm, builtin, "out of memory");
        shellUpdateStatus(1);
        return false;
    }
    if (!shellDirectoryStackEnsureCapacity(1)) {
        free(copy);
        shellDirectoryStackReportError(vm, builtin, "out of memory");
        shellUpdateStatus(1);
        return false;
    }
    gShellRuntime.dir_stack[0] = copy;
    gShellRuntime.dir_stack_count = 1;
    gShellRuntime.dir_stack_initialised = true;
    if (!shellDirectoryStackUpdateEnvironment(NULL, gShellRuntime.dir_stack[0])) {
        const char *readonly = shellReadonlyGetErrorName();
        if (errno == EPERM && readonly && *readonly) {
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "%s: readonly variable", readonly);
            shellDirectoryStackReportError(vm, builtin, buffer);
        } else {
            shellDirectoryStackReportErrno(vm, builtin);
        }
        free(gShellRuntime.dir_stack[0]);
        gShellRuntime.dir_stack[0] = NULL;
        gShellRuntime.dir_stack_count = 0;
        gShellRuntime.dir_stack_initialised = false;
        shellUpdateStatus(errno ? errno : 1);
        return false;
    }
    return true;
}

static bool shellDirectoryStackAssignTopOwned(char *path) {
    if (!path) {
        path = strdup("");
        if (!path) {
            return false;
        }
    }
    size_t needed = (gShellRuntime.dir_stack_count > 0) ? gShellRuntime.dir_stack_count : 1;
    if (!shellDirectoryStackEnsureCapacity(needed)) {
        free(path);
        return false;
    }
    if (gShellRuntime.dir_stack_count == 0) {
        gShellRuntime.dir_stack[0] = path;
        gShellRuntime.dir_stack_count = 1;
    } else {
        char *old = gShellRuntime.dir_stack[0];
        gShellRuntime.dir_stack[0] = path;
        free(old);
    }
    gShellRuntime.dir_stack_initialised = true;
    return true;
}

static bool shellDirectoryStackUpdateEnvironment(const char *old_cwd, const char *new_cwd) {
    const char *new_value = new_cwd;
    const char *old_value = old_cwd;
#if defined(PSCAL_TARGET_IOS)
    char new_visible[PATH_MAX];
    char old_visible[PATH_MAX];
    if (new_cwd) {
        new_value = shellVisiblePath(new_cwd, new_visible, sizeof(new_visible));
    }
    if (old_cwd) {
        old_value = shellVisiblePath(old_cwd, old_visible, sizeof(old_visible));
    }
#endif
    if (new_value) {
        if (!shellSetTrackedVariable("PWD", new_value, false)) {
            return false;
        }
    }
    if (old_value) {
        if (!shellSetTrackedVariable("OLDPWD", old_value, false)) {
            if (new_value) {
                const char *current = shellSafeGetenv("PWD");
                if (!current || strcmp(current, old_value) != 0) {
                    (void)shellSetTrackedVariable("PWD", old_value, false);
                }
            }
            return false;
        }
    }
    return true;
}

static bool shellDirectoryStackChdir(VM *vm,
                                     const char *builtin,
                                     const char *target,
                                     char **out_new_cwd,
                                     char **out_old_cwd) {
    if (!target) {
        shellDirectoryStackReportError(vm, builtin, "expected directory path");
        shellUpdateStatus(1);
        return false;
    }
    char old_cwd[PATH_MAX];
    if (!SHELL_GETCWD(old_cwd, sizeof(old_cwd))) {
        shellDirectoryStackReportErrno(vm, builtin);
        shellUpdateStatus(errno ? errno : 1);
        return false;
    }
    if (SHELL_CHDIR(target) != 0) {
#if defined(PSCAL_TARGET_IOS)
        char logBuf[PATH_MAX + 64];
        snprintf(logBuf, sizeof(logBuf), "[exsh] chdir failed target=%s errno=%d", target ? target : "(null)", errno);
        pscalRuntimeDebugLog(logBuf);
#endif
        shellDirectoryStackReportErrno(vm, builtin);
        shellUpdateStatus(errno ? errno : 1);
        return false;
    }
    char new_cwd[PATH_MAX];
    if (!SHELL_GETCWD(new_cwd, sizeof(new_cwd))) {
        shellDirectoryStackReportErrno(vm, builtin);
        shellUpdateStatus(errno ? errno : 1);
        (void)SHELL_CHDIR(old_cwd);
        return false;
    }
    char *new_copy = strdup(new_cwd);
    if (!new_copy) {
        shellDirectoryStackReportError(vm, builtin, "out of memory");
        shellUpdateStatus(1);
        (void)SHELL_CHDIR(old_cwd);
        return false;
    }
    char *old_copy = NULL;
    if (out_old_cwd) {
        old_copy = strdup(old_cwd);
        if (!old_copy) {
            shellDirectoryStackReportError(vm, builtin, "out of memory");
            shellUpdateStatus(1);
            free(new_copy);
            (void)SHELL_CHDIR(old_cwd);
            return false;
        }
    }
    if (out_new_cwd) {
        *out_new_cwd = new_copy;
    } else {
        free(new_copy);
    }
    if (out_old_cwd) {
        *out_old_cwd = old_copy;
    }
    return true;
}

static void shellDirectoryStackWriteAll(int fd, const char *data, size_t len) {
    while (len > 0) {
        ssize_t wrote = write(fd, data, len);
        if (wrote < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (wrote == 0) {
            break;
        }
        data += (size_t)wrote;
        len -= (size_t)wrote;
    }
}

static void shellDirectoryStackWriteEntry(int fd, const char *entry) {
    if (fd < 0) {
        return;
    }
    if (!entry) {
        return;
    }
    const char *home = shellSafeGetenv("HOME");
    size_t home_len = home ? strlen(home) : 0;
    if (home_len > 0 && strncmp(entry, home, home_len) == 0 &&
        (entry[home_len] == '\0' || entry[home_len] == '/')) {
        shellDirectoryStackWriteAll(fd, "~", 1);
        const char *suffix = entry + home_len;
        if (*suffix == '/') {
            suffix++;
            if (*suffix) {
                shellDirectoryStackWriteAll(fd, "/", 1);
                shellDirectoryStackWriteAll(fd, suffix, strlen(suffix));
            }
        }
        return;
    }
    shellDirectoryStackWriteAll(fd, entry, strlen(entry));
}

static void shellDirectoryStackPrint(int fd) {
    if (fd < 0) {
        return;
    }
    if (!gShellRuntime.dir_stack || gShellRuntime.dir_stack_count == 0) {
        shellDirectoryStackWriteAll(fd, "\n", 1);
        return;
    }
    for (size_t i = 0; i < gShellRuntime.dir_stack_count; ++i) {
        const char *entry = gShellRuntime.dir_stack[i] ? gShellRuntime.dir_stack[i] : "";
        shellDirectoryStackWriteEntry(fd, entry);
        if (i + 1 < gShellRuntime.dir_stack_count) {
            shellDirectoryStackWriteAll(fd, " ", 1);
        }
    }
    shellDirectoryStackWriteAll(fd, "\n", 1);
}

static char *shellLookupRawEnvironmentValue(const char *name, size_t len) {
    if (!name) {
        return strdup("");
    }
    char *key = (char *)malloc(len + 1);
    if (!key) {
        return NULL;
    }
    memcpy(key, name, len);
    key[len] = '\0';
    char *local_value = NULL;
    if (shellLocalScopeGetCopy(key, &local_value, NULL)) {
        free(key);
        if (!local_value) {
            return strdup("");
        }
        return local_value;
    }
    const char *env = shellSafeGetenv(key);
    free(key);
    if (!env) {
        return strdup("");
    }
    return strdup(env);
}

static char *shellJoinArrayValuesWithSeparator(char **items, size_t count, char separator) {
    char *joined = NULL;
    size_t length = 0;
    size_t capacity = 0;
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) {
            shellBufferAppendChar(&joined, &length, &capacity, separator);
        }
        shellBufferAppendString(&joined, &length, &capacity, items && items[i] ? items[i] : "");
    }
    if (!joined) {
        joined = strdup("");
    }
    return joined;
}

static char *shellJoinArrayValues(char **items, size_t count) {
    if (!items || count == 0) {
        return strdup("");
    }
    return shellJoinArrayValuesWithSeparator(items, count, ' ');
}

static char *shellJoinNumericIndices(size_t count, char separator) {
    if (count == 0) {
        return strdup("");
    }
    char *joined = NULL;
    size_t length = 0;
    size_t capacity = 0;
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) {
            shellBufferAppendChar(&joined, &length, &capacity, separator);
        }
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%zu", i);
        shellBufferAppendString(&joined, &length, &capacity, buffer);
    }
    if (!joined) {
        joined = strdup("");
    }
    return joined;
}

static char *shellExpandArraySubscriptValue(const char *name,
                                            size_t name_len,
                                            const char *subscript,
                                            size_t subscript_len,
                                            size_t *out_count,
                                            bool *out_is_full_expansion) {
    if (out_count) {
        *out_count = 0;
    }
    if (out_is_full_expansion) {
        *out_is_full_expansion = false;
    }
    if (!name || name_len == 0 || !subscript) {
        return strdup("");
    }
    while (subscript_len > 0 && isspace((unsigned char)subscript[0])) {
        subscript++;
        subscript_len--;
    }
    while (subscript_len > 0 && isspace((unsigned char)subscript[subscript_len - 1])) {
        subscript_len--;
    }
    const ShellArrayVariable *array_var = shellArrayRegistryLookup(name, name_len);
    char **items = NULL;
    char **keys = NULL;
    size_t count = 0;
    bool using_registry = false;
    ShellArrayKind kind = SHELL_ARRAY_KIND_INDEXED;
    if (array_var) {
        items = array_var->values;
        count = array_var->count;
        keys = array_var->keys;
        kind = array_var->kind;
        using_registry = true;
    } else {
        char *raw = shellLookupRawEnvironmentValue(name, name_len);
        if (!raw) {
            return NULL;
        }
        if (!shellParseArrayLiteral(raw, &items, &keys, &count, &kind)) {
            free(raw);
            return NULL;
        }
        free(raw);
    }

    char *result = NULL;
    if (subscript_len == 0) {
        result = strdup("");
    } else if (subscript_len == 1 && (subscript[0] == '*' || subscript[0] == '@')) {
        if (out_count) {
            *out_count = count;
        }
        if (subscript[0] == '@') {
            if (out_is_full_expansion) {
                *out_is_full_expansion = true;
            }
            result = shellJoinArrayValuesWithSeparator(items, count, SHELL_ARRAY_ELEMENT_SEP);
        } else {
            result = shellJoinArrayValues(items, count);
        }
    } else if (kind == SHELL_ARRAY_KIND_ASSOCIATIVE) {
        char *key_text = (char *)malloc(subscript_len + 1);
        if (key_text) {
            memcpy(key_text, subscript, subscript_len);
            key_text[subscript_len] = '\0';
            char *decoded_key = shellDecodeAssociativeKey(key_text, subscript_len);
            free(key_text);
            if (decoded_key) {
                for (size_t i = 0; i < count; ++i) {
                    const char *stored_key = (keys && keys[i]) ? keys[i] : "";
                    if (strcmp(stored_key, decoded_key) == 0) {
                        result = strdup(items[i] ? items[i] : "");
                        break;
                    }
                }
                if (!result) {
                    result = strdup("");
                }
                free(decoded_key);
            }
        }
    } else {
        char *index_text = (char *)malloc(subscript_len + 1);
        if (index_text) {
            memcpy(index_text, subscript, subscript_len);
            index_text[subscript_len] = '\0';
            char *endptr = NULL;
            long index = strtol(index_text, &endptr, 10);
            if (endptr && *endptr == '\0' && index >= 0 && (size_t)index < count) {
                result = strdup(items[index] ? items[index] : "");
            } else {
                result = strdup("");
            }
            free(index_text);
        }
    }

    if (!using_registry) {
        shellFreeArrayValues(items, count);
        if (keys) {
            shellFreeArrayValues(keys, count);
        }
    }
    if (!result) {
        result = strdup("");
    }
    return result;
}
