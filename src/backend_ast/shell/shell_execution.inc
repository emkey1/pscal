/* Auto-generated include: command execution and pipeline management. Included from shell.c. */

#include "ios/vproc.h"
#include "backend_ast/builtin.h"
#include "smallclue/smallclue.h"
#if defined(PSCAL_TARGET_IOS)
#include "common/path_virtualization.h"
#include "ios/tty/pscal_fd.h"
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#define PIPE_SESSION(ctx) (&(ctx)->session)
#define PIPE_SPOOL_FD(ctx) ((ctx)->session.spool_fd)
#define PIPE_SPOOL_SIZE(ctx) ((ctx)->session.spool_size)
#define PIPE_SPOOL_OWNED(ctx) ((ctx)->session.spool_owned)
#define PIPE_STDOUT_FD(ctx) ((ctx)->session.stdout_fd)
#define PIPE_STDERR_FD(ctx) ((ctx)->session.stderr_fd)
#define PIPE_STDIN_FD(ctx) ((ctx)->session.stdin_fd)
#define PIPE_STDOUT_OWNED(ctx) ((ctx)->session.stdout_owned)
#define PIPE_STDERR_OWNED(ctx) ((ctx)->session.stderr_owned)
#define PIPE_STDIN_OWNED(ctx) ((ctx)->session.stdin_owned)

static int gShellPagerVirtualTTYFd = -1;

static bool shellPathHasShebang(const char *path) {
    if (!path || !*path) {
        return false;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    char header[2];
    ssize_t n = read(fd, header, sizeof(header));
    close(fd);
    return n == 2 && header[0] == '#' && header[1] == '!';
}

static bool shellPathProbeExecutable(const char *path, char *display, size_t display_size) {
    if (!path || !*path || !display || display_size == 0) {
        return false;
    }

    const char *probes[4];
    size_t probe_count = 0;
    probes[probe_count++] = path;

#if defined(PSCAL_TARGET_IOS)
    char expanded[PATH_MAX];
    char stripped[PATH_MAX];
    if (path[0] == '/' && pathTruncateExpand(path, expanded, sizeof(expanded)) &&
        strcmp(expanded, path) != 0) {
        probes[probe_count++] = expanded;
    }
    if (path[0] == '/' && pathTruncateStrip(path, stripped, sizeof(stripped)) &&
        strcmp(stripped, path) != 0) {
        probes[probe_count++] = stripped;
        char stripped_expanded[PATH_MAX];
        if (pathTruncateExpand(stripped, stripped_expanded, sizeof(stripped_expanded)) &&
            strcmp(stripped_expanded, stripped) != 0) {
            probes[probe_count++] = stripped_expanded;
        }
    }
#endif

    for (size_t i = 0; i < probe_count; ++i) {
        const char *candidate = probes[i];
        if (!candidate || !*candidate) {
            continue;
        }
        struct stat st;
        if (stat(candidate, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        bool runnable = (access(candidate, X_OK) == 0);
#if defined(PSCAL_TARGET_IOS)
        if (!runnable && (st.st_mode & 0111) != 0) {
            runnable = true;
        }
        if (!runnable && access(candidate, R_OK) == 0 && shellPathHasShebang(candidate)) {
            runnable = true;
        }
#endif
        if (!runnable) {
            continue;
        }
#if defined(PSCAL_TARGET_IOS)
        if (candidate[0] == '/' && pathTruncateStrip(candidate, display, display_size)) {
            return true;
        }
#endif
        int copied = snprintf(display, display_size, "%s", candidate);
        return copied >= 0 && (size_t)copied < display_size;
    }
    return false;
}

static bool shellResolveExecutableCandidate(const char *candidate, char *resolved, size_t resolved_size) {
    if (!candidate || !resolved || resolved_size == 0) {
        return false;
    }
    return shellPathProbeExecutable(candidate, resolved, resolved_size);
}

/* Resolve bare command names against PATH for in-process shebang execution. */
static char *shellFindExecutableOnPath(const char *name) {
    if (!name || strchr(name, '/')) {
        return NULL;
    }
    char *path_value = shellLookupParameterValue("PATH", strlen("PATH"));
    const char *path = (path_value && *path_value) ? path_value : getenv("PATH");
    char candidate[PATH_MAX];
    if (path) {
        const char *cursor = path;
        while (*cursor) {
            const char *next = strchr(cursor, ':');
            size_t dir_len = next ? (size_t)(next - cursor) : strlen(cursor);
            const char *dir = cursor;
            char dir_buf[PATH_MAX];
            if (dir_len == 0) {
                dir = ".";
                dir_len = 1;
            } else {
                if (dir_len >= sizeof(dir_buf)) {
                    if (!next) {
                        break;
                    }
                    cursor = next + 1;
                    continue;
                }
                memcpy(dir_buf, cursor, dir_len);
                dir_buf[dir_len] = '\0';
                dir = dir_buf;
            }
            int len = snprintf(candidate, sizeof(candidate), "%s/%s", dir, name);
            char resolved[PATH_MAX];
            if (len > 0 && (size_t)len < sizeof(candidate) &&
                shellResolveExecutableCandidate(candidate, resolved, sizeof(resolved))) {
                char *out = strdup(resolved);
                free(path_value);
                return out;
            }
            if (!next) {
                break;
            }
            cursor = next + 1;
        }
    }
#if defined(PSCAL_TARGET_IOS)
    /* Keep fallback probing in app-root namespace instead of device-root paths. */
    const char *fallback_dirs[] = { "/bin", "/Documents/bin" };
    for (size_t i = 0; i < sizeof(fallback_dirs) / sizeof(fallback_dirs[0]); ++i) {
        int len = snprintf(candidate, sizeof(candidate), "%s/%s", fallback_dirs[i], name);
        char resolved[PATH_MAX];
        if (len > 0 && (size_t)len < sizeof(candidate) &&
            shellResolveExecutableCandidate(candidate, resolved, sizeof(resolved))) {
            free(path_value);
            return strdup(resolved);
        }
    }
#endif
    free(path_value);
    return NULL;
}

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

static void shellVprocFormatLabel(const ShellCommand *cmd, char *out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return;
    }
    out[0] = '\0';
    if (!cmd || cmd->argc == 0 || !cmd->argv) {
        return;
    }
    size_t used = 0;
    for (size_t i = 0; i < cmd->argc && used + 1 < out_sz; ++i) {
        const char *part = cmd->argv[i] ? cmd->argv[i] : "";
        size_t len = strlen(part);
        if (used + len + 1 >= out_sz) {
            len = out_sz - used - 1;
        }
        memcpy(out + used, part, len);
        used += len;
        if (used + 1 < out_sz && i + 1 < cmd->argc) {
            out[used++] = ' ';
        }
    }
    out[used] = '\0';
}

typedef struct {
    int pid;
    bool have_prev;
    char prev_label[64];
} ShellVprocLabelScope;

static void shellVprocLabelScopeBegin(ShellVprocLabelScope *scope, const ShellCommand *cmd) {
    if (!scope) {
        return;
    }
    scope->pid = -1;
    scope->have_prev = false;
    scope->prev_label[0] = '\0';

    VProc *current_vp = vprocCurrent();
    if (!current_vp) {
        return;
    }
    int label_pid = vprocPid(current_vp);
    if (label_pid <= 0) {
        return;
    }
    if (label_pid == vprocGetShellSelfPid()) {
        return;
    }

    if (!vprocGetCommandLabel(label_pid, scope->prev_label, sizeof(scope->prev_label))) {
        return;
    }

    char label[96];
    shellVprocFormatLabel(cmd, label, sizeof(label));
    if (!label[0]) {
        return;
    }

    scope->pid = label_pid;
    scope->have_prev = true;
    vprocSetCommandLabel(label_pid, label);
}

static void shellVprocLabelScopeEnd(ShellVprocLabelScope *scope) {
    if (!scope || scope->pid <= 0) {
        return;
    }
    if (scope->have_prev) {
        vprocSetCommandLabel(scope->pid, scope->prev_label);
    }
    scope->pid = -1;
    scope->have_prev = false;
    scope->prev_label[0] = '\0';
}

static VProc *shellCreateVProcForStage(int stdin_host,
                                       int stdout_host,
                                       int stderr_host,
                                       int cols,
                                       int rows,
                                       int pid_hint,
                                       bool inherit_pscal_stdio) {
    (void)vprocEnsureKernelPid();
    /* Optionally inherit stdio from the active vproc; never fall back to the
     * interactive session if the caller disables inheritance. Background workers
     * must not mutate the interactive session's fds. */
    bool stdin_force_null = (stdin_host == -2);
    bool stdin_use_pscal = false;
    bool stdout_use_pscal = false;
    bool stderr_use_pscal = false;
    struct pscal_fd *pscal_stdin = NULL;
    struct pscal_fd *pscal_stdout = NULL;
    struct pscal_fd *pscal_stderr = NULL;
    VProc *active = inherit_pscal_stdio ? vprocCurrent() : NULL;
    VProcSessionStdio *session_stdio = inherit_pscal_stdio ? vprocSessionStdioCurrent() : NULL;
    if (active) {
        if (!stdin_force_null && stdin_host < 0) {
            int translated = vprocTranslateFd(active, STDIN_FILENO);
            if (translated >= 0) {
                stdin_host = translated;
            } else {
                pscal_stdin = vprocGetPscalFd(active, STDIN_FILENO);
                stdin_use_pscal = (pscal_stdin != NULL);
            }
        }
        if (stdout_host < 0) {
            int translated = vprocTranslateFd(active, STDOUT_FILENO);
            if (translated >= 0) {
                stdout_host = translated;
            } else {
                pscal_stdout = vprocGetPscalFd(active, STDOUT_FILENO);
                stdout_use_pscal = (pscal_stdout != NULL);
            }
        }
        if (stderr_host < 0) {
            int translated = vprocTranslateFd(active, STDERR_FILENO);
            if (translated >= 0) {
                stderr_host = translated;
            } else {
                pscal_stderr = vprocGetPscalFd(active, STDERR_FILENO);
                stderr_use_pscal = (pscal_stderr != NULL);
            }
        }
    } else if (session_stdio) {
        if (!stdin_force_null && stdin_host < 0 && session_stdio->stdin_pscal_fd) {
            pscal_stdin = pscal_fd_retain(session_stdio->stdin_pscal_fd);
            stdin_use_pscal = (pscal_stdin != NULL);
        }
        if (stdout_host < 0 && session_stdio->stdout_pscal_fd) {
            pscal_stdout = pscal_fd_retain(session_stdio->stdout_pscal_fd);
            stdout_use_pscal = (pscal_stdout != NULL);
        }
        if (stderr_host < 0 && session_stdio->stderr_pscal_fd) {
            pscal_stderr = pscal_fd_retain(session_stdio->stderr_pscal_fd);
            stderr_use_pscal = (pscal_stderr != NULL);
        }
    }
    if (!stdin_force_null && stdin_host < 0 && !stdin_use_pscal) {
        stdin_host = STDIN_FILENO;
    }
    if (stdin_use_pscal && stdin_host < 0) {
        stdin_host = -2;
    }
    if (stdout_use_pscal && stdout_host < 0) {
        stdout_host = -2;
    }
    if (stderr_use_pscal && stderr_host < 0) {
        stderr_host = -2;
    }
    bool tool_dbg = getenv("PSCALI_TOOL_DEBUG") != NULL;
    if (tool_dbg) {
        fprintf(stderr,
                "[vproc-stage] stdin_host=%d stdout_host=%d stderr_host=%d force_null=%d pscal=%d/%d/%d\n",
                stdin_host, stdout_host, stderr_host, (int)stdin_force_null,
                (int)stdin_use_pscal, (int)stdout_use_pscal, (int)stderr_use_pscal);
    }
    VProcOptions opts = vprocDefaultOptions();
    opts.stdin_fd = (stdin_host >= 0) ? stdin_host : -2;   /* default to /dev/null */
    opts.stdout_fd = (stdout_host >= 0) ? stdout_host : stdout_host;
    opts.stderr_fd = (stderr_host >= 0) ? stderr_host : stderr_host;
    opts.winsize_cols = cols;
    opts.winsize_rows = rows;
    opts.pid_hint = pid_hint;
    if (pid_hint > 0) {
        int existing_job_id = vprocGetJobId(pid_hint);
        if (existing_job_id > 0) {
            opts.job_id = existing_job_id;
        }
    }
    VProc *vp = vprocCreate(&opts);
    if (!vp) {
        if (pscal_stdin) pscal_fd_close(pscal_stdin);
        if (pscal_stdout) pscal_fd_close(pscal_stdout);
        if (pscal_stderr) pscal_fd_close(pscal_stderr);
        return NULL;
    }
    if (stdin_use_pscal && pscal_stdin) {
        if (vprocAdoptPscalFd(vp, STDIN_FILENO, pscal_stdin) != 0 && tool_dbg) {
            fprintf(stderr, "[vproc-stage] adopt pscal stdin failed: %s\n", strerror(errno));
        }
    }
    if (stdout_use_pscal && pscal_stdout) {
        if (vprocAdoptPscalFd(vp, STDOUT_FILENO, pscal_stdout) != 0 && tool_dbg) {
            fprintf(stderr, "[vproc-stage] adopt pscal stdout failed: %s\n", strerror(errno));
        }
    }
    if (stderr_use_pscal && pscal_stderr) {
        if (vprocAdoptPscalFd(vp, STDERR_FILENO, pscal_stderr) != 0 && tool_dbg) {
            fprintf(stderr, "[vproc-stage] adopt pscal stderr failed: %s\n", strerror(errno));
        }
    }
    if (pscal_stdin) pscal_fd_close(pscal_stdin);
    if (pscal_stdout) pscal_fd_close(pscal_stdout);
    if (pscal_stderr) pscal_fd_close(pscal_stderr);
    return vp;
}
#endif

static int shellTryRunScriptFallback(VM *vm, const ShellCommand *cmd) {
    if (!cmd || cmd->argc <= 0 || !cmd->argv || !cmd->argv[0]) {
        return -1;
    }
    const char *script_path = cmd->argv[0];
#if defined(PSCAL_TARGET_IOS)
    const char *probes[4];
    size_t probe_count = 0;
    probes[probe_count++] = cmd->argv[0];

    char expanded[PATH_MAX];
    char stripped[PATH_MAX];
    char stripped_expanded[PATH_MAX];

    if (cmd->argv[0][0] == '/' &&
        pathTruncateExpand(cmd->argv[0], expanded, sizeof(expanded)) &&
        strcmp(expanded, cmd->argv[0]) != 0) {
        probes[probe_count++] = expanded;
    }
    if (cmd->argv[0][0] == '/' &&
        pathTruncateStrip(cmd->argv[0], stripped, sizeof(stripped)) &&
        strcmp(stripped, cmd->argv[0]) != 0) {
        probes[probe_count++] = stripped;
        if (pathTruncateExpand(stripped, stripped_expanded, sizeof(stripped_expanded)) &&
            strcmp(stripped_expanded, stripped) != 0) {
            probes[probe_count++] = stripped_expanded;
        }
    }
#endif

    bool readable = false;
#if defined(PSCAL_TARGET_IOS)
    for (size_t i = 0; i < probe_count; ++i) {
        struct stat st;
        if (stat(probes[i], &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (access(probes[i], R_OK) != 0) {
            continue;
        }
        script_path = probes[i];
        readable = true;
        break;
    }
#else
    struct stat st;
    if (stat(script_path, &st) == 0 && S_ISREG(st.st_mode) && access(script_path, R_OK) == 0) {
        readable = true;
    }
#endif
    if (!readable) {
        return -1;
    }

    char *source = shellLoadFile(script_path);
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

    shellRuntimeSetArg0(cmd->argv[0]);
    bool exit_requested = false;
    int status = shellRunSource(source, cmd->argv[0], &opts, &exit_requested);
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

    /* External-style script execution should not request shell exit when the
     * script itself calls `exit`. Treat it as command termination only. */
    (void)exit_requested;

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
    bool redir_dbg = getenv("PSCALI_REDIR_DEBUG") != NULL;
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
    if (redir_dbg) {
        fprintf(stderr, "[redir-build] arg_count=%d meta=%s\n", arg_count, meta.s_val);
    }
    for (int i = 1; i < arg_count; ++i) {
        Value v = args[i];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "shell exec: arguments must be strings");
            shellFreeCommand(out_cmd);
            return false;
        }
        if (redir_dbg) {
            fprintf(stderr, "[redir-build]   arg[%d]=%s\n", i, v.s_val);
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
            const char *shebang_path = cmd->argv[0];
            char *resolved = shellFindExecutableOnPath(cmd->argv[0]);
            if (resolved) {
                shebang_path = resolved;
            }
            int shebang_status = shellMaybeExecShebangTool(shebang_path, cmd->argv);
            if (resolved) {
                free(resolved);
            }
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
    pid_t pid;
    int status_raw;
    int status_mapped;
    bool stopped;
    bool continued;
} ShellWaitResult;

static int shellWaitPid(pid_t pid, int *status_out, bool allow_stop, bool *out_stopped) {
    ShellWaitResult res = { .pid = pid, .status_raw = 0, .status_mapped = 0, .stopped = false, .continued = false };
    int err = 0;
    int status = 0;
    int options = allow_stop ? WUNTRACED : 0;
    pid_t waited;
    do {
        waited = vprocWaitPidShim(pid, &status, options);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        err = errno;
    }
    res.status_raw = status;
    if (WIFSTOPPED(status)) {
        res.stopped = true;
        res.status_mapped = 128 + WSTOPSIG(status);
    } else if (WIFEXITED(status)) {
        res.status_mapped = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        res.status_mapped = 128 + WTERMSIG(status);
    } else {
        res.status_mapped = status;
    }
    if (status_out) {
        *status_out = res.status_mapped;
    }
    if (out_stopped) {
        *out_stopped = res.stopped;
    }
    return err;
}

static int shellWaitPidDetailed(pid_t pid, bool allow_stop, ShellWaitResult *out) {
    ShellWaitResult tmp;
    ShellWaitResult *res = out ? out : &tmp;
    memset(res, 0, sizeof(*res));
    res->pid = pid;

    int options = allow_stop ? WUNTRACED : 0;
    int status = 0;
    pid_t waited;
    do {
        waited = vprocWaitPidShim(pid, &status, options);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        return errno;
    }

    res->status_raw = status;
    if (WIFSTOPPED(status)) {
        res->stopped = true;
        res->status_mapped = 128 + WSTOPSIG(status);
    } else if (WIFEXITED(status)) {
        res->status_mapped = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        res->status_mapped = 128 + WTERMSIG(status);
    } else {
        res->status_mapped = status;
    }
    return 0;
}

#if defined(PSCAL_TARGET_IOS)
static void shellFdPrintf(int fd, const char *fmt, ...);
#endif

#if defined(PSCAL_TARGET_IOS)
static const char *shellFormatCommandSummary(const ShellCommand *cmd, char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) {
        return NULL;
    }
    buf[0] = '\0';
    if (!cmd || cmd->argc <= 0 || !cmd->argv) {
        return NULL;
    }
    size_t used = 0;
    for (size_t i = 0; i < cmd->argc && used + 1 < buf_len; ++i) {
        const char *part = cmd->argv[i] ? cmd->argv[i] : "";
        size_t len = strlen(part);
        if (used + len + 1 >= buf_len) {
            len = buf_len - used - 1;
        }
        memcpy(buf + used, part, len);
        used += len;
        if (used + 1 < buf_len && i + 1 < cmd->argc) {
            buf[used++] = ' ';
        }
    }
    buf[used] = '\0';
    return (buf[0] != '\0') ? buf : NULL;
}

static void shellPrintJobSuspended(const ShellJob *job, const ShellCommand *cmd) {
    const char *command = (job && job->command) ? job->command : NULL;
    char summary[128];
    if (!command) {
        command = shellFormatCommandSummary(cmd, summary, sizeof(summary));
    }
    if (!command) {
        command = "unknown";
    }
#if defined(PSCAL_TARGET_IOS)
    if (shellRuntimeIsInteractive()) {
        shellFdPrintf(STDERR_FILENO, "^Z\n");
    }
#endif
    shellFdPrintf(STDERR_FILENO, "exsh: suspended  %s\n", command);
}

static void shellPrintJobStarted(const ShellJob *job, pid_t pid) {
    if (pid <= 0) {
        return;
    }
    if (!shellRuntimeIsInteractive()) {
        return;
    }
    size_t job_id = job ? job->id : 0;
    if (job_id == 0) {
        job_id = (size_t)vprocGetJobId((int)pid);
    }
    if (job_id == 0) {
        return;
    }
    shellFdPrintf(STDOUT_FILENO, "[%zu] %d\n", job_id, (int)pid);
}
#endif

#if defined(PSCAL_TARGET_IOS)
static bool shellShouldRunSmallclueInWorker(const ShellCommand *cmd) {
    if (!cmd || !cmd->argv || !cmd->argv[0]) {
        return false;
    }
    if (cmd->background) {
        return false;
    }
    shellEnsureJobControl();
    if (!gShellRuntime.job_control_enabled) {
        return false;
    }
    return smallclueFindApplet(cmd->argv[0]) != NULL;
}
#endif

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
    int job_id;
#if defined(PSCAL_TARGET_IOS)
    VProc *vp;
    ShellRuntimeState *bg_shell_ctx;
    VM *bg_vm;
    char *stdout_path;
    char *stderr_path;
    int shell_pid;
    int kernel_pid;
#endif
} ShellBackgroundStageCtx;

#if defined(PSCAL_TARGET_IOS)
typedef struct PSCALRuntimeContext PSCALRuntimeContext;
extern PSCALRuntimeContext *PSCALRuntimeGetCurrentRuntimeContext(void) __attribute__((weak));
extern void PSCALRuntimeSetCurrentRuntimeContext(PSCALRuntimeContext *ctx) __attribute__((weak));

typedef struct {
    VM *vm;
    ShellCommand *cmd;
    VProc *vp;
    VProcSessionStdio *session_stdio;
    PSCALRuntimeContext *runtime_ctx;
    PSCALRuntimeContext *prev_runtime_ctx;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    bool stdin_is_vproc;
    bool stdout_is_vproc;
    bool stderr_is_vproc;
    bool apply_exec_redirs;
    int shell_pid;
    int kernel_pid;
    int status;
    bool prev_bg_flag;
    bool prev_worker_flag;
    bool stopped;
    pthread_mutex_t stop_mu;
    pthread_cond_t stop_cv;
} ShellInProcessWorkerCtx;

typedef struct {
    VM *vm;
    ShellPipelineContext *ctx;
    ShellCommand *cmd;
    size_t stage_pos;
    bool pipeline_head;
    bool pipeline_tail;
    bool owns_ctx_snapshot;
    VProcSessionStdio *session_stdio;
#if defined(PSCAL_TARGET_IOS)
    PSCALRuntimeContext *runtime_ctx;
#endif
} ShellPipelineStageThreadCtx;
#endif

#if defined(PSCAL_TARGET_IOS)
static void shellVprocSigintHandler(int signo) {
    (void)signo;
    pscalRuntimeRequestSigint();
}

static void shellInProcessWorkerCleanup(void *arg) {
    ShellInProcessWorkerCtx *ctx = (ShellInProcessWorkerCtx *)arg;
    if (!ctx) {
        return;
    }
    pthread_mutex_destroy(&ctx->stop_mu);
    pthread_cond_destroy(&ctx->stop_cv);

    if (ctx->stdin_fd >= 0) {
        if (ctx->stdin_is_vproc) {
            vprocCloseShim(ctx->stdin_fd);
        } else {
            vprocHostClose(ctx->stdin_fd);
        }
        ctx->stdin_fd = -1;
    }
    if (ctx->stdout_fd >= 0) {
        if (ctx->stdout_is_vproc) {
            vprocCloseShim(ctx->stdout_fd);
        } else {
            vprocHostClose(ctx->stdout_fd);
        }
        ctx->stdout_fd = -1;
    }
    if (ctx->stderr_fd >= 0 && ctx->stderr_fd != ctx->stdout_fd) {
        if (ctx->stderr_is_vproc) {
            vprocCloseShim(ctx->stderr_fd);
        } else {
            vprocHostClose(ctx->stderr_fd);
        }
        ctx->stderr_fd = -1;
    }

    vprocDeactivate();
    vprocMarkExit(ctx->vp, ctx->status);
    vprocDiscard(vprocPid(ctx->vp));
    vprocDestroy(ctx->vp);

    gShellBackgroundWorkerActive = ctx->prev_worker_flag;
    gShellCurrentBuiltinBackground = ctx->prev_bg_flag;

    if (ctx->session_stdio) {
        vprocSessionStdioActivate(NULL);
    }
    if (PSCALRuntimeSetCurrentRuntimeContext && ctx->prev_runtime_ctx) {
        PSCALRuntimeSetCurrentRuntimeContext(ctx->prev_runtime_ctx);
    }
    shellFreeCommand(ctx->cmd);
    free(ctx->cmd);
    free(ctx);
}

static void shellInstallVprocSigintHandler(VProc *vp) {
    if (!vp) {
        return;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = shellVprocSigintHandler;
    (void)vprocSigaction(vprocPid(vp), SIGINT, &sa, NULL);
}

/* Open a background redirection target, falling back to the raw path when
 * pathTruncateExpand produces a non-existent path (common under host stubs). */
static int shellBgOpenPath(const char *raw_path,
                           bool append,
                           char *out_final,
                           size_t out_final_sz);
#endif

#if defined(PSCAL_TARGET_IOS)
static void shellInProcessBackgroundCancelCleanup(void *arg) {
    ShellBackgroundStageCtx *ctx = (ShellBackgroundStageCtx *)arg;
    if (!ctx) {
        return;
    }

    /* Undo global environment toggles that would otherwise leak into the
     * interactive shell when a background task is cancelled via vprocKillShim. */
    unsetenv("PSCALI_BG_STDOUT");
    unsetenv("PSCALI_BG_STDOUT_APPEND");
    unsetenv("PSCALI_BG_STDERR");
    unsetenv("PSCALI_BG_STDERR_APPEND");

    if (ctx->vp) {
        vprocDeactivate();
        vprocDestroy(ctx->vp);
        ctx->vp = NULL;
    }
    if (ctx->bg_vm) {
        freeVM(ctx->bg_vm);
        free(ctx->bg_vm);
        ctx->bg_vm = NULL;
    }
    if (ctx->bg_shell_ctx) {
        free(ctx->bg_shell_ctx);
        ctx->bg_shell_ctx = NULL;
    }
    free(ctx->stdout_path);
    ctx->stdout_path = NULL;
    free(ctx->stderr_path);
    ctx->stderr_path = NULL;

    if (ctx->cmd) {
        shellFreeCommand(ctx->cmd);
        free(ctx->cmd);
        ctx->cmd = NULL;
    }
    free(ctx);
}
#endif

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

static void *shellInProcessBackgroundMain(void *arg) {
    ShellBackgroundStageCtx *ctx = (ShellBackgroundStageCtx *)arg;
    if (!ctx) {
        return NULL;
    }
#if defined(PSCAL_TARGET_IOS)
    if (ctx->shell_pid > 0) {
        vprocSetShellSelfPid(ctx->shell_pid);
    }
    if (ctx->kernel_pid > 0) {
        vprocSetKernelPid(ctx->kernel_pid);
    }
#endif
    bool prev_bg_flag = gShellCurrentBuiltinBackground;
    gShellCurrentBuiltinBackground = true;
    /* Allow pthread_cancel to interrupt sleeps/IO. */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
#if defined(PSCAL_TARGET_IOS)
    pthread_cleanup_push(shellInProcessBackgroundCancelCleanup, ctx);
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
        ctx->bg_shell_ctx = bg_ctx;
        prev_ctx = shellRuntimeActivateContext(bg_ctx);
        bg_vm = (VM *)calloc(1, sizeof(VM));
        if (bg_vm) {
            initVM(bg_vm);
            bg_vm->frontendContext = bg_ctx;
            prev_vm = shellSwapCurrentVm(bg_vm);
            vm = bg_vm;
            ctx->bg_vm = bg_vm;
        } else {
            shellRuntimeActivateContext(prev_ctx);
            prev_ctx = NULL;
            free(bg_ctx);
            ctx->bg_shell_ctx = NULL;
            bg_ctx = NULL;
        }
    }
#else
    VM *vm = ctx->vm ? ctx->vm : gShellCurrentVm;
#endif
    ShellCommand *cmd = ctx->cmd;
    bool tool_dbg = getenv("PSCALI_TOOL_DEBUG") != NULL;
    bool pipe_dbg = getenv("PSCALI_PIPE_DEBUG") != NULL;
#if defined(PSCAL_TARGET_IOS)
    if (tool_dbg) {
        fprintf(stderr, "[bg-stage] start cmd=%s bg=%d stage_count=%zu ctx=%p\n",
                (cmd && cmd->argv && cmd->argc > 0) ? cmd->argv[0] : "<nil>",
                (int)(cmd ? cmd->background : 0),
                (size_t)(cmd ? cmd->argc : 0),
                (void *)ctx);
    }
#endif
    size_t redir_count = cmd ? cmd->redir_count : 0;
    bool redir_dbg = getenv("PSCALI_REDIR_DEBUG") != NULL;
    if (tool_dbg || redir_dbg || pipe_dbg) {
        fprintf(stderr, "[bg-stage] redir_count=%zu\n", redir_count);
        if (cmd && cmd->redirs && (redir_dbg || tool_dbg)) {
            for (size_t ri = 0; ri < redir_count; ++ri) {
                ShellRedirection *r = &cmd->redirs[ri];
                fprintf(stderr, "[bg-stage]   redir[%zu] fd=%d kind=%d path=%s dup=%d flags=0x%x\n",
                        ri, r->fd, (int)r->kind, r->path ? r->path : "<none>", r->dup_target_fd, r->flags);
            }
        }
    }
    const char *stdout_raw = NULL;
    const char *stderr_raw = NULL;
    bool stdout_append = false;
    bool stderr_append = false;
    int stdin_fd = -1;
    bool stdin_redirected = false;
    /* Background defaults: stdin -> /dev/null unless explicitly redirected. */
    if (ctx->cmd && ctx->cmd->background) {
        stdin_fd = -2;
    }

    if (cmd && redir_count > 0 && cmd->redirs) {
        for (size_t i = 0; i < redir_count; ++i) {
            ShellRedirection *r = &cmd->redirs[i];
            if (r->kind == SHELL_RUNTIME_REDIR_OPEN) {
                if (r->fd == STDIN_FILENO && r->path) {
                    int fd = open(r->path, r->flags, r->mode);
                    if (fd >= 0) {
                        stdin_fd = fd;
                        stdin_redirected = true;
                    }
                } else if (r->fd == STDOUT_FILENO && r->path) {
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

    if (!stdin_redirected && stdin_fd < 0) {
        stdin_fd = open("/dev/null", O_RDONLY);
    }

    const char *stdout_env = stdout_raw;
    const char *stderr_env = stderr_raw;
    if (tool_dbg || redir_dbg || pipe_dbg) {
        fprintf(stderr, "[bg-stage] stdout_raw=%s stderr_raw=%s\n",
                stdout_raw ? stdout_raw : "<null>",
                stderr_raw ? stderr_raw : "<null>");
    }
#if defined(PSCAL_TARGET_IOS)
    if (tool_dbg) {
        fprintf(stderr, "[bg-stage] cmd=%s stdout_raw=%s stderr_raw=%s\n",
                cmd && cmd->argc > 0 ? cmd->argv[0] : "<null>",
                stdout_env ? stdout_env : "<none>",
                stderr_env ? stderr_env : "<none>");
    }
#endif
    char *stdout_path = stdout_env ? strdup(stdout_env) : NULL;
    char *stderr_path = stderr_env ? strdup(stderr_env) : NULL;
    ctx->stdout_path = stdout_path;
    ctx->stderr_path = stderr_path;
    int vm_stdout = -1;
    int vm_stderr = -1;
    /* If stderr was requested to follow stdout, reuse the stdout handle (after open). */
    int bg_status = 0;
#if defined(PSCAL_TARGET_IOS)
    VProc *vp = NULL;
#endif

    if (stdout_path && *stdout_path) {
        char resolved_out[PATH_MAX];
        vm_stdout = shellBgOpenPath(stdout_path, stdout_append, resolved_out, sizeof(resolved_out));
        if (vm_stdout >= 0 && resolved_out[0]) {
            free(stdout_path);
            stdout_path = strdup(resolved_out);
        }
        if (tool_dbg || redir_dbg) {
            fprintf(stderr, "[bg-stage] open stdout '%s' fd=%d err=%s\n",
                    stdout_path ? stdout_path : "<null>",
                    vm_stdout, (vm_stdout < 0) ? strerror(errno) : "ok");
        }
    }
    if (stderr_path && *stderr_path) {
        char resolved_err[PATH_MAX];
        vm_stderr = shellBgOpenPath(stderr_path, stderr_append, resolved_err, sizeof(resolved_err));
        if (vm_stderr >= 0 && resolved_err[0]) {
            free(stderr_path);
            stderr_path = strdup(resolved_err);
        }
        if (tool_dbg || redir_dbg) {
            fprintf(stderr, "[bg-stage] open stderr '%s' fd=%d err=%s\n",
                    stderr_path ? stderr_path : "<null>",
                    vm_stderr, (vm_stderr < 0) ? strerror(errno) : "ok");
        }
    }
    if (stdout_path && stderr_path && strcmp(stdout_path, stderr_path) == 0 && vm_stdout >= 0) {
        vm_stderr = vm_stdout;
    }
    /* Now that paths may have been rewritten, export the final paths for tools. */
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
#if defined(PSCAL_TARGET_IOS)
    vp = shellCreateVProcForStage(stdin_fd,
                                  vm_stdout,
                                  vm_stderr,
                                  pscalRuntimeDetectWindowCols(),
                                  pscalRuntimeDetectWindowRows(),
                                  ctx->synthetic_pid > 0 ? ctx->synthetic_pid : -1,
                                  true);
    if (!vp) {
        runtimeError(vm, "background: failed to allocate vproc");
        goto cleanup;
    }
    ctx->vp = vp;
    vprocRegisterThread(vp, pthread_self());
    int shell_pid = vprocGetShellSelfPid();
    if (shell_pid > 0) {
        vprocSetParent(vprocPid(vp), shell_pid);
    }
    int shell_sid = (shell_pid > 0) ? vprocGetSid(shell_pid) : -1;
    pid_t group = (ctx->synthetic_pid > 0) ? ctx->synthetic_pid : vprocPid(vp);
    if (shell_sid > 0) {
        vprocSetSid(vprocPid(vp), shell_sid);
    }
    vprocSetPgid(vprocPid(vp), group);
    ctx->synthetic_pid = group;
    if (ctx->job_id > 0) {
        vprocSetJobId(vprocPid(vp), ctx->job_id);
        if (group > 0 && group != vprocPid(vp)) {
            vprocSetJobId(group, ctx->job_id);
        }
    }
    /* Label the vproc for lps/ps visibility. */
        if (cmd && cmd->argc > 0 && cmd->argv) {
            char label[96] = {0};
            shellVprocFormatLabel(cmd, label, sizeof(label));
            if (label[0]) {
                vprocSetCommandLabel(vprocPid(vp), label);
            }
        }
    /* Hand off host fds to the synthetic vproc so dup2 shims work on vproc-local
     * descriptors during builtin execution. */
    /* Adopt host fds into the child vproc so shimmed I/O stays per-process.
     * If adoption fails, keep using the host fd directly (don’t close or rewrite
     * the parent’s descriptors). */
    if (stdin_fd >= 0) {
        int host_fd = stdin_fd;
        int vfd = vprocAdoptHostFd(vp, host_fd);
        if (vfd >= 0) {
            stdin_fd = vfd;
            if (tool_dbg && pipe_dbg) fprintf(stderr, "[bg-stage] adopted stdin host=%d vfd=%d\n", host_fd, vfd);
        } else if (tool_dbg && pipe_dbg) {
            fprintf(stderr, "[bg-stage] adopt stdin host=%d failed: %s\n", host_fd, strerror(errno));
        }
    }
    if (vm_stdout >= 0) {
        int host_fd = vm_stdout;
        int vfd = vprocAdoptHostFd(vp, host_fd);
        if (vfd >= 0) {
            vm_stdout = vfd;
            if (tool_dbg && pipe_dbg) fprintf(stderr, "[bg-stage] adopted stdout host=%d vfd=%d\n", host_fd, vfd);
        } else if (tool_dbg && pipe_dbg) {
            fprintf(stderr, "[bg-stage] adopt stdout host=%d failed: %s\n", host_fd, strerror(errno));
        }
    }
    if (vm_stderr >= 0) {
        int host_fd = vm_stderr;
        int vfd = vprocAdoptHostFd(vp, host_fd);
        if (vfd >= 0) {
            vm_stderr = vfd;
            if (tool_dbg && pipe_dbg) fprintf(stderr, "[bg-stage] adopted stderr host=%d vfd=%d\n", host_fd, vfd);
        } else if (tool_dbg && pipe_dbg) {
            fprintf(stderr, "[bg-stage] adopt stderr host=%d failed: %s\n", host_fd, strerror(errno));
        }
    }
    vprocActivate(vp);
#endif
    gShellBackgroundWorkerActive = true;
    /* We already materialised stdout/stderr redirs above; skip apply_exec_redirs here.
     * Clear cmd redirs so downstream helpers do not reopen/dup back to the session. */
    cmd->redir_count = 0;
    if (tool_dbg && pipe_dbg) {
        VProcSessionStdio *session_stdio = vprocSessionStdioCurrent();
        fprintf(stderr, "[bg-stage] about to run builtin stdin_fd=%d stdout_fd=%d stderr_fd=%d session_out=%d session_err=%d\n",
                stdin_fd, vm_stdout, vm_stderr,
                session_stdio ? session_stdio->stdout_host_fd : -1,
                session_stdio ? session_stdio->stderr_host_fd : -1);
    }
    if (tool_dbg && pipe_dbg) {
        int host_out = vprocTranslateFd(vp, STDOUT_FILENO);
        int host_err = vprocTranslateFd(vp, STDERR_FILENO);
        int host_in = vprocTranslateFd(vp, STDIN_FILENO);
        fprintf(stderr, "[bg-stage] pre-run fdmap stdin host=%d stdout host=%d stderr host=%d bg_flag=%d\n",
                host_in, host_out, host_err, (int)gShellCurrentBuiltinBackground);
    }
    bg_status = shellRunBuiltinInProcess(vm, cmd, stdin_fd, vm_stdout, vm_stderr, false);
    if (tool_dbg && pipe_dbg) {
        fprintf(stderr, "[bg-stage] finished run status=%d\n", bg_status);
    }
    if (tool_dbg && pipe_dbg) {
        int host_out = vprocTranslateFd(vp, STDOUT_FILENO);
        int host_err = vprocTranslateFd(vp, STDERR_FILENO);
        int host_in = vprocTranslateFd(vp, STDIN_FILENO);
        fprintf(stderr, "[bg-stage] post-run fdmap stdin host=%d stdout host=%d stderr host=%d status=%d\n",
                host_in, host_out, host_err, bg_status);
    }
    gShellBackgroundWorkerActive = false;

cleanup:
#if defined(PSCAL_TARGET_IOS)
    if (stdin_fd >= 0) vprocCloseShim(stdin_fd);
    if (vm_stdout >= 0) vprocCloseShim(vm_stdout);
    if (vm_stderr >= 0) vprocCloseShim(vm_stderr);
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
    if (getenv("PSCALI_PIPE_DEBUG")) {
        fprintf(stderr, "[pipeline-bg] command '%s' status=%d\n",
                (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>", bg_status);
    }
    vprocDeactivate();
    if (vp) {
        vprocMarkExit(vp, bg_status);
        vprocDiscard(vprocPid(vp));
        vprocDestroy(vp);
        ctx->vp = NULL;
    }
#else
    if (stdin_fd >= 0) close(stdin_fd);
    if (vm_stdout >= 0) close(vm_stdout);
    if (vm_stderr >= 0) close(vm_stderr);
#endif
    unsetenv("PSCALI_BG_STDOUT");
    unsetenv("PSCALI_BG_STDOUT_APPEND");
    unsetenv("PSCALI_BG_STDERR");
    unsetenv("PSCALI_BG_STDERR_APPEND");
    shellFreeCommand(cmd);
    free(stdout_path);
    ctx->stdout_path = NULL;
    free(stderr_path);
    ctx->stderr_path = NULL;
    free(cmd);
#if defined(PSCAL_TARGET_IOS)
    /* Restore VM/runtime context now that the background task is done. */
    if (bg_vm || prev_vm) {
        shellRestoreCurrentVm(prev_vm);
    }
    if (prev_ctx) {
        shellRuntimeActivateContext(prev_ctx);
    }
    if (bg_vm) {
        freeVM(bg_vm);
        free(bg_vm);
        ctx->bg_vm = NULL;
    }
    if (bg_ctx) {
        free(bg_ctx);
        ctx->bg_shell_ctx = NULL;
    }
#endif
#if defined(PSCAL_TARGET_IOS)
    pthread_cleanup_pop(0);
#endif
    gShellCurrentBuiltinBackground = prev_bg_flag;
    free(ctx);
    return NULL;
}

static void *shellInProcessWorkerMain(void *arg) {
    ShellInProcessWorkerCtx *ctx = (ShellInProcessWorkerCtx *)arg;
    if (!ctx || !ctx->vp || !ctx->cmd) {
        free(ctx);
        return NULL;
    }
    pthread_mutex_init(&ctx->stop_mu, NULL);
    pthread_cond_init(&ctx->stop_cv, NULL);
    ctx->status = W_EXITCODE(128 + SIGINT, 0);
    ctx->prev_runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext
        ? PSCALRuntimeGetCurrentRuntimeContext()
        : NULL;
    if (PSCALRuntimeSetCurrentRuntimeContext && ctx->runtime_ctx) {
        PSCALRuntimeSetCurrentRuntimeContext(ctx->runtime_ctx);
    }
    if (ctx->session_stdio) {
        vprocSessionStdioActivate(ctx->session_stdio);
    }
    if (ctx->shell_pid > 0) {
        vprocSetShellSelfPid(ctx->shell_pid);
    }
    if (ctx->kernel_pid > 0) {
        vprocSetKernelPid(ctx->kernel_pid);
    }

    ctx->prev_bg_flag = gShellCurrentBuiltinBackground;
    ctx->prev_worker_flag = gShellBackgroundWorkerActive;
    gShellCurrentBuiltinBackground = false;
    gShellBackgroundWorkerActive = true;

    pthread_cleanup_push(shellInProcessWorkerCleanup, ctx);

    vprocRegisterThread(ctx->vp, pthread_self());
    vprocActivate(ctx->vp);

    ctx->status = shellRunBuiltinInProcess(ctx->vm,
                                           ctx->cmd,
                                           ctx->stdin_fd,
                                           ctx->stdout_fd,
                                           ctx->stderr_fd,
                                           ctx->apply_exec_redirs);

    pthread_cleanup_pop(1);
    return NULL;
}
#endif

static int shellFinishPipeline(const ShellCommand *tail_cmd);

#if defined(PSCAL_TARGET_IOS)
static bool shellDupOntoTarget(VProc *vp, int src_fd, int target_fd, bool dbg, const char *label);
static void shellBgEnsureParentDir(const char *path) {
    if (!path || !*path) {
        return;
    }
    const char *last_slash = strrchr(path, '/');
    if (!last_slash || last_slash == path) {
        /* No parent component or root only. */
        return;
    }
    char buf[PATH_MAX];
    size_t len = (size_t)(last_slash - path);
    if (len >= sizeof(buf)) {
        return;
    }
    memcpy(buf, path, len);
    buf[len] = '\0';
    /* Create each segment of the parent path. */
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0777);
            *p = '/';
        }
    }
    mkdir(buf, 0777);
}
static int shellBgOpenPath(const char *raw_path,
                           bool append,
                           char *out_final,
                           size_t out_final_sz) {
    if (out_final && out_final_sz > 0) {
        out_final[0] = '\0';
    }
    if (!raw_path || !*raw_path) {
        return -1;
    }
    const char *open_path = raw_path;
    char expanded[PATH_MAX];
    bool expanded_ok = pathTruncateExpand(raw_path, expanded, sizeof(expanded));
    if (expanded_ok) {
        open_path = expanded;
    }
    shellBgEnsureParentDir(open_path);
    int flags = O_CREAT | O_WRONLY | (append ? O_APPEND : O_TRUNC);
    int fd = open(open_path, flags, 0666);
    if (fd < 0 && expanded_ok) {
        /* Fallback to raw path if the expansion produced an inaccessible path
         * (common under host test stubs). */
        shellBgEnsureParentDir(raw_path);
        fd = open(raw_path, flags, 0666);
        if (fd >= 0) {
            open_path = raw_path;
        }
    }
    if (fd >= 0 && out_final && out_final_sz > 0) {
        snprintf(out_final, out_final_sz, "%s", open_path);
    }
    return fd;
}
static void shellPipelineSessionCloseSpool(ShellPipelineContext *ctx) {
    if (!ctx) {
        return;
    }
    if (PIPE_SPOOL_FD(ctx) >= 0 && PIPE_SPOOL_OWNED(ctx)) {
        vprocHostClose(PIPE_SPOOL_FD(ctx));
    }
    PIPE_SPOOL_FD(ctx) = -1;
    PIPE_SPOOL_SIZE(ctx) = 0;
    PIPE_SPOOL_OWNED(ctx) = false;
}

static void shellPipelineSessionCloseStdStreams(ShellPipelineContext *ctx) {
    if (!ctx) {
        return;
    }
    if (PIPE_STDIN_FD(ctx) >= 0 && PIPE_STDIN_OWNED(ctx)) {
        vprocHostClose(PIPE_STDIN_FD(ctx));
    }
    if (PIPE_STDOUT_FD(ctx) >= 0 && PIPE_STDOUT_OWNED(ctx)) {
        vprocHostClose(PIPE_STDOUT_FD(ctx));
    }
    if (PIPE_STDERR_FD(ctx) >= 0 && PIPE_STDERR_OWNED(ctx)) {
        vprocHostClose(PIPE_STDERR_FD(ctx));
    }
    PIPE_STDIN_FD(ctx) = -1;
    PIPE_STDOUT_FD(ctx) = -1;
    PIPE_STDERR_FD(ctx) = -1;
    PIPE_STDIN_OWNED(ctx) = false;
    PIPE_STDOUT_OWNED(ctx) = false;
    PIPE_STDERR_OWNED(ctx) = false;
}

static void shellPipelineSessionRestoreStdStreams(ShellPipelineContext *ctx) {
    if (!ctx) {
        return;
    }
    if (PIPE_STDIN_FD(ctx) >= 0) {
        vprocHostDup2(PIPE_STDIN_FD(ctx), STDIN_FILENO);
        shellResetStandardStream(STDIN_FILENO);
    }
    if (PIPE_STDOUT_FD(ctx) >= 0) {
        vprocHostDup2(PIPE_STDOUT_FD(ctx), STDOUT_FILENO);
        shellResetStandardStream(STDOUT_FILENO);
    }
    if (PIPE_STDERR_FD(ctx) >= 0) {
        vprocHostDup2(PIPE_STDERR_FD(ctx), STDERR_FILENO);
        shellResetStandardStream(STDERR_FILENO);
    }
    shellPipelineSessionCloseStdStreams(ctx);
}

static void shellPipelineSessionSnapshotStdStreams(ShellPipelineContext *ctx) {
    if (!ctx) {
        return;
    }
    shellPipelineSessionCloseStdStreams(ctx);
    PIPE_STDIN_FD(ctx) = vprocHostDup(STDIN_FILENO);
    PIPE_STDOUT_FD(ctx) = vprocHostDup(STDOUT_FILENO);
    PIPE_STDERR_FD(ctx) = vprocHostDup(STDERR_FILENO);
    PIPE_STDIN_OWNED(ctx) = PIPE_STDIN_FD(ctx) >= 0;
    PIPE_STDOUT_OWNED(ctx) = PIPE_STDOUT_FD(ctx) >= 0;
    PIPE_STDERR_OWNED(ctx) = PIPE_STDERR_FD(ctx) >= 0;
}
#endif

static void shellResetPipeline(void) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (!ctx->active) {
        return;
    }
#if defined(PSCAL_TARGET_IOS)
    shellPipelineSessionRestoreStdStreams(ctx);
    shellPipelineSessionCloseSpool(ctx);
    ctx->in_process = false;
    ctx->in_process_worker = false;
    ctx->session_stdio = NULL;
    if (ctx->pscal_pipe_read || ctx->pscal_pipe_write || ctx->pscal_pipe_write_hold) {
        size_t pipe_count = (ctx->stage_count > 0) ? (ctx->stage_count - 1) : 0;
        for (size_t i = 0; i < pipe_count; ++i) {
            if (ctx->pscal_pipe_read && ctx->pscal_pipe_read[i]) {
                pscal_fd_close(ctx->pscal_pipe_read[i]);
                ctx->pscal_pipe_read[i] = NULL;
            }
            if (ctx->pscal_pipe_write && ctx->pscal_pipe_write[i]) {
                pscal_fd_close(ctx->pscal_pipe_write[i]);
                ctx->pscal_pipe_write[i] = NULL;
            }
            if (ctx->pscal_pipe_write_hold && ctx->pscal_pipe_write_hold[i]) {
                pscal_fd_close(ctx->pscal_pipe_write_hold[i]);
                ctx->pscal_pipe_write_hold[i] = NULL;
            }
        }
        free(ctx->pscal_pipe_read);
        free(ctx->pscal_pipe_write);
        free(ctx->pscal_pipe_write_hold);
        ctx->pscal_pipe_read = NULL;
        ctx->pscal_pipe_write = NULL;
        ctx->pscal_pipe_write_hold = NULL;
    }
    if (ctx->threads) {
        free(ctx->threads);
        ctx->threads = NULL;
    }
    if (ctx->thread_started) {
        free(ctx->thread_started);
        ctx->thread_started = NULL;
    }
    if (ctx->stage_status) {
        free(ctx->stage_status);
        ctx->stage_status = NULL;
    }
#endif
    if (ctx->pipes) {
        size_t pipe_count = (ctx->stage_count > 0) ? (ctx->stage_count - 1) : 0;
        for (size_t i = 0; i < pipe_count; ++i) {
            if (ctx->pipes[i][0] >= 0) {
                vprocHostClose(ctx->pipes[i][0]);
                ctx->pipes[i][0] = -1;
            }
            if (ctx->pipes[i][1] >= 0) {
                vprocHostClose(ctx->pipes[i][1]);
                ctx->pipes[i][1] = -1;
            }
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
    shellPipelineSessionRestoreStdStreams(ctx);
    shellPipelineSessionCloseSpool(ctx);
#endif
#if defined(PSCAL_TARGET_IOS)
    if (ctx->pscal_pipe_read || ctx->pscal_pipe_write || ctx->pscal_pipe_write_hold) {
        size_t pipe_count = (ctx->stage_count > 0) ? (ctx->stage_count - 1) : 0;
        for (size_t i = 0; i < pipe_count; ++i) {
            if (ctx->pscal_pipe_read && ctx->pscal_pipe_read[i]) {
                pscal_fd_close(ctx->pscal_pipe_read[i]);
                ctx->pscal_pipe_read[i] = NULL;
            }
            if (ctx->pscal_pipe_write && ctx->pscal_pipe_write[i]) {
                pscal_fd_close(ctx->pscal_pipe_write[i]);
                ctx->pscal_pipe_write[i] = NULL;
            }
            if (ctx->pscal_pipe_write_hold && ctx->pscal_pipe_write_hold[i]) {
                pscal_fd_close(ctx->pscal_pipe_write_hold[i]);
                ctx->pscal_pipe_write_hold[i] = NULL;
            }
        }
        free(ctx->pscal_pipe_read);
        free(ctx->pscal_pipe_write);
        free(ctx->pscal_pipe_write_hold);
        ctx->pscal_pipe_read = NULL;
        ctx->pscal_pipe_write = NULL;
        ctx->pscal_pipe_write_hold = NULL;
    }
    if (ctx->thread_started && ctx->threads) {
        for (size_t i = 0; i < ctx->stage_count; ++i) {
            if (ctx->thread_started[i]) {
                pthread_join(ctx->threads[i], NULL);
                ctx->thread_started[i] = false;
            }
        }
    }
#endif
    if (ctx->pipes) {
        size_t pipe_count = (ctx->stage_count > 0) ? (ctx->stage_count - 1) : 0;
        for (size_t i = 0; i < pipe_count; ++i) {
            if (ctx->pipes[i][0] >= 0) {
                vprocHostClose(ctx->pipes[i][0]);
                ctx->pipes[i][0] = -1;
            }
            if (ctx->pipes[i][1] >= 0) {
                vprocHostClose(ctx->pipes[i][1]);
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
        ctx->in_process_worker = false;
        ctx->session_stdio = vprocSessionStdioCurrent();
        shellPipelineSessionCloseSpool(ctx);
        shellPipelineSessionSnapshotStdStreams(ctx);
//    fprintf(stderr, "[pipe] ensure stages=%zu in_process=1\n", stages);
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
    if (stages > 1) {
#if defined(PSCAL_TARGET_IOS)
        /* iOS default: use in-proc PSCAL pipes. Host pipes can be forced with
         * `PSCALI_PIPE_PSCAL=0` for diagnostics. */
        bool use_pscal_pipes = true;
        const char *pipe_mode = getenv("PSCALI_PIPE_PSCAL");
        if (pipe_mode && *pipe_mode) {
            char ch = pipe_mode[0];
            if (ch == '0' || ch == 'n' || ch == 'N' || ch == 'f' || ch == 'F') {
                use_pscal_pipes = false;
            }
        }
        if (getenv("PSCALI_PIPE_DEBUG")) {
            fprintf(stderr, "[pipe] iOS mode=%s stages=%zu\n",
                    use_pscal_pipes ? "pscal" : "host",
                    stages);
        }
        ctx->pscal_pipe_read = NULL;
        ctx->pscal_pipe_write = NULL;
        ctx->pscal_pipe_write_hold = NULL;
        ctx->pipes = calloc(stages - 1, sizeof(int[2]));
        if (!ctx->pipes) {
            shellResetPipeline();
            return false;
        }
        for (size_t i = 0; i < stages - 1; ++i) {
            ctx->pipes[i][0] = -1;
            ctx->pipes[i][1] = -1;
            if (!use_pscal_pipes) {
                if (vprocHostPipe(ctx->pipes[i]) != 0) {
                    shellResetPipeline();
                    return false;
                }
            }
        }
        if (use_pscal_pipes) {
            ctx->pscal_pipe_read = (struct pscal_fd **)calloc(stages - 1, sizeof(struct pscal_fd *));
            ctx->pscal_pipe_write = (struct pscal_fd **)calloc(stages - 1, sizeof(struct pscal_fd *));
            ctx->pscal_pipe_write_hold = (struct pscal_fd **)calloc(stages - 1, sizeof(struct pscal_fd *));
            if (!ctx->pscal_pipe_read || !ctx->pscal_pipe_write || !ctx->pscal_pipe_write_hold) {
                shellResetPipeline();
                return false;
            }
            for (size_t i = 0; i < stages - 1; ++i) {
                if (vprocCreateInprocPipe(&ctx->pscal_pipe_read[i], &ctx->pscal_pipe_write[i]) != 0 ||
                    !ctx->pscal_pipe_read[i] || !ctx->pscal_pipe_write[i]) {
                    shellResetPipeline();
                    return false;
                }
                ctx->pscal_pipe_write_hold[i] = pscal_fd_retain(ctx->pscal_pipe_write[i]);
                if (!ctx->pscal_pipe_write_hold[i]) {
                    shellResetPipeline();
                    return false;
                }
            }
        }
#else
        ctx->pipes = calloc(stages - 1, sizeof(int[2]));
        if (!ctx->pipes) {
            shellResetPipeline();
            return false;
        }
        for (size_t i = 0; i < stages - 1; ++i) {
            ctx->pipes[i][0] = -1;
            ctx->pipes[i][1] = -1;
            if (vprocHostPipe(ctx->pipes[i]) != 0) {
                shellResetPipeline();
                return false;
            }
        }
#endif
    }
#if defined(PSCAL_TARGET_IOS)
    if (stages > 0) {
        ctx->threads = (pthread_t *)calloc(stages, sizeof(pthread_t));
        ctx->thread_started = (bool *)calloc(stages, sizeof(bool));
        ctx->stage_status = (int *)calloc(stages, sizeof(int));
        if (!ctx->threads || !ctx->thread_started || !ctx->stage_status) {
            shellResetPipeline();
            return false;
        }
        for (size_t i = 0; i < stages; ++i) {
            ctx->thread_started[i] = false;
            ctx->stage_status[i] = 0;
        }
    }
#endif
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
        if (ctx->in_process_worker && !ctx->background) {
            shellEnsureJobControl();
            bool job_control = gShellRuntime.job_control_enabled && job_pgid > 0;
            bool stopped_job = false;

            if (job_control) {
                shellJobControlSetForeground(job_pgid);
            }

            /* In-process stages already run to completion before we reach this
             * point (tail runs inline, earlier stages are joined). Their vproc
             * tasks may already be discarded, so waiting here is racy and can
             * lose the tail-stage status. Use recorded stage statuses instead. */
            for (size_t i = 0; i < ctx->launched; ++i) {
                int stage_status = 0;
                if (ctx->stage_status && i < ctx->stage_count) {
                    stage_status = ctx->stage_status[i];
                }
                final_status = stage_status;
                if (stage_status == 128 + SIGTSTP) {
                    stopped_job = true;
                }
                if (ctx->pids && i < ctx->stage_count) {
                    ctx->pids[i] = -1;
                }
            }

            if (job_control) {
                shellJobControlRestoreForeground();
            }

            if (!ctx->background && final_status >= 128 && final_status < 128 + NSIG) {
                shellHandlePendingSignal(final_status - 128);
            }

            /* In-process stages don't get WIFSTOPPED; treat 148 (128+SIGTSTP) as a stop. */
            if (job_control && !stopped_job && final_status == 128 + SIGTSTP) {
                stopped_job = true;
            }

            shellRuntimeProcessPendingSignals();

	            if (stopped_job && job_control) {
	                ShellJob *job = NULL;
	                if (tail_cmd) {
	                    job = shellRegisterJob(job_pgid, ctx->pids, ctx->launched, tail_cmd);
	                }
                if (job) {
                    shellJobMarkStopped(job, final_status);
                }
#if defined(PSCAL_TARGET_IOS)
	                shellPrintJobSuspended(job, tail_cmd);
#endif
	                ctx->last_status = final_status;
	                shellCloseVprocStreams();
	                shellResetPipeline();
	                shellUpdateStatus(final_status);
	                return final_status;
	            }

	            if (ctx->negated) {
	                final_status = (final_status == 0) ? 1 : 0;
	            }
	            ctx->last_status = final_status;
	            shellCloseVprocStreams();
	            shellResetPipeline();
	            shellUpdateStatus(final_status);
	            return final_status;
	        }

        if (ctx->background) {
            /* Register the synthetic in-process job so fg/bg/jobs work. */
            if (ctx->pids && ctx->launched > 0) {
                pid_t bg_pgid = (ctx->pgid > 0) ? ctx->pgid : ctx->pids[0];
                    ShellJob *job = shellRegisterJob(bg_pgid, ctx->pids, ctx->launched, tail_cmd);
                    if (job) {
                        shellJobMarkRunning(job);
                        job->last_status = 0;
                    }
                    gShellRuntime.last_bg_pid = bg_pgid;
#if defined(PSCAL_TARGET_IOS)
                    shellPrintJobStarted(job, bg_pgid);
#endif
                }
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
        if (!ctx->background) {
            shellEnsureJobControl();
            shellJobControlRestoreForeground();
        }
	        if (ctx->negated) {
	            final_status = (final_status == 0) ? 1 : 0;
	        }
	        ctx->last_status = final_status;
	        shellCloseVprocStreams();
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

#if defined(PSCAL_TARGET_IOS)
#endif
        for (size_t i = 0; i < ctx->launched; ++i) {
            pid_t pid = ctx->pids[i];
            if (pid <= 0) {
                continue;
            }
            if (gShellExitRequested || gShellRuntime.break_requested) {
                if (ctx->pgid > 0) {
                    shellJobControlSendPgid(ctx->pgid, SIGTERM);
                } else {
                    shellJobControlSendPid(pid, SIGTERM);
                }
                continue;
            }
            ShellWaitResult wait_res;
            int err = shellWaitPidDetailed(pid, job_control, &wait_res);
            if (err != 0) {
                continue;
            }
            if (wait_res.stopped) {
                stopped_job = true;
                final_status = wait_res.status_mapped;
            } else {
                final_status = wait_res.status_mapped;
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
                    shellJobMarkStopped(job, final_status);
                }
#if defined(PSCAL_TARGET_IOS)
            shellPrintJobSuspended(job, tail_cmd);
#endif
            ctx->last_status = final_status;
            shellResetPipeline();
            shellUpdateStatus(final_status);
            return final_status;
        }
    } else if (ctx->launched > 0) {
        gShellRuntime.last_bg_pid = ctx->pids[ctx->launched - 1];
        ShellJob *job = NULL;
        if (tail_cmd) {
            job = shellRegisterJob(job_pgid, ctx->pids, ctx->launched, tail_cmd);
        }
        if (job) {
            shellJobMarkRunning(job);
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
    backup.use_vproc = false;
#if defined(PSCAL_TARGET_IOS)
    VProc *active_vp = vprocCurrent();
    bool want_vproc_restore = gShellSkipHostStdio;
    if (gShellRuntime.pipeline.in_process && !gShellBackgroundWorkerActive && !want_vproc_restore) {
        /* In-process pipelines run with a synthetic vproc whose stdio does not
         * match the shell's host fds. Backup/restore must target the real host
         * descriptors so interactive input is restored after the stage ends. */
        active_vp = NULL;
    }
#else
    VProc *active_vp = NULL;
    (void)active_vp;
#endif
    int min_fd = target_fd + 1;
    if (avoid_fd >= 0 && avoid_fd >= min_fd) {
        min_fd = avoid_fd + 1;
    }
    int dup_fd = -1;
#if defined(PSCAL_TARGET_IOS)
    int source_fd = target_fd;
    if (want_vproc_restore) {
        VProcSessionStdio *session_stdio = vprocSessionStdioCurrent();
        if (session_stdio && !vprocSessionStdioIsDefault(session_stdio)) {
            if (target_fd == STDIN_FILENO) {
                source_fd = session_stdio->stdin_host_fd;
            } else if (target_fd == STDOUT_FILENO) {
                source_fd = session_stdio->stdout_host_fd;
            } else if (target_fd == STDERR_FILENO) {
                source_fd = session_stdio->stderr_host_fd;
            }
        }
        backup.use_vproc = true;
    } else if (active_vp) {
        int host_src = vprocTranslateFd(active_vp, target_fd);
        if (host_src >= 0) {
            source_fd = host_src;
            backup.use_vproc = true;
        }
    }
#else
    int source_fd = target_fd;
#endif
    if (source_fd < 0) {
        return false;
    }
#ifdef F_DUPFD_CLOEXEC
    dup_fd = fcntl(source_fd, F_DUPFD_CLOEXEC, min_fd);
    if (dup_fd < 0 && errno == EINVAL) {
        dup_fd = -1;
    }
#endif
    if (dup_fd < 0) {
        dup_fd = fcntl(source_fd, F_DUPFD, min_fd);
        if (dup_fd >= 0) {
            fcntl(dup_fd, F_SETFD, FD_CLOEXEC);
        }
    }
    if (dup_fd < 0) {
        dup_fd = dup(source_fd);
        if (dup_fd >= 0) {
            fcntl(dup_fd, F_SETFD, FD_CLOEXEC);
        }
    }
    if (dup_fd >= 0 && avoid_fd >= 0 && dup_fd == avoid_fd) {
#ifdef F_DUPFD_CLOEXEC
        int alt_fd = fcntl(source_fd, F_DUPFD_CLOEXEC, avoid_fd + 1);
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
                alt_fd = fcntl(source_fd, F_DUPFD, avoid_fd + 1);
                if (alt_fd >= 0) {
                    fcntl(alt_fd, F_SETFD, FD_CLOEXEC);
                }
            }
            if (alt_fd >= 0) {
                vprocHostClose(dup_fd);
                dup_fd = alt_fd;
            } else {
                vprocHostClose(dup_fd);
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
                vprocHostClose(backup.saved_fd);
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
    /* If stdio fds were remapped (dup2/redirection), drop cached FILE* wrappers
     * so the next access reopens against the current descriptor mapping. */
    shellInvalidateVprocStreamForFd(fd);
    if (fd == STDIN_FILENO) {
        clearerr(stdin);
    } else if (fd == STDOUT_FILENO) {
        clearerr(stdout);
    } else if (fd == STDERR_FILENO) {
        clearerr(stderr);
    }
}

/* Duplicate src onto target, updating both the host fd and (when present) the
 * current vproc's fd table. */
static bool shellDupOntoTarget(VProc *vp, int src_fd, int target_fd, bool dbg, const char *label) {
    if (target_fd < 0) {
        errno = EBADF;
        return false;
    }
    int host_src = src_fd;
#if defined(PSCAL_TARGET_IOS)
    bool skip_host_dup = gShellBackgroundWorkerActive || gShellSkipHostStdio;
    /* In-process pipelines must still duplicate host stdio so downstream stages
     * see the correct descriptors; ignore skip_host_dup in that case. */
    if (gShellRuntime.pipeline.active && gShellRuntime.pipeline.in_process) {
        skip_host_dup = false;
    }
    if (vp) {
        int translated = vprocTranslateFd(vp, src_fd);
        if (translated >= 0) {
            host_src = translated;
        }
    }
#endif
    int hrc = 0;
#if defined(PSCAL_TARGET_IOS)
    if (vp) {
        int vrc = vprocDup2(vp, src_fd, target_fd);
        if (dbg) {
            fprintf(stderr, "[redir] vprocDup2 %d->%d rc=%d host_src=%d (%s)\n",
                    src_fd, target_fd, vrc, host_src, label ? label : "");
        }
        /* Never touch the process-wide stdio when running in a background worker;
         * the interactive shell shares host fds across threads. */
        if (!skip_host_dup && host_src >= 0) {
            hrc = vprocHostDup2(host_src, target_fd);
            if (dbg) {
                fprintf(stderr, "[redir] vprocHostDup2 %d->%d rc=%d (%s)\n",
                        host_src, target_fd, hrc, label ? label : "");
            }
        }
        if (!skip_host_dup && host_src < 0 && src_fd >= 0) {
            hrc = dup2(src_fd, target_fd);
        } else if (!skip_host_dup && host_src >= 0 && hrc < 0) {
            hrc = dup2(host_src, target_fd);
        }
    } else
#endif
    {
        hrc = dup2((host_src >= 0) ? host_src : src_fd, target_fd);
    }
    if (hrc < 0) {
        if (dbg) {
            fprintf(stderr, "[redir] dup %d->%d failed: %s (%s)\n",
                    (host_src >= 0) ? host_src : src_fd,
                    target_fd, strerror(errno), label ? label : "");
        }
        return false;
    }
    shellResetStandardStream(target_fd);
    return true;
}

static void shellRestoreExecRedirections(ShellExecRedirBackup *backups, size_t count) {
    if (!backups) {
        return;
    }
#if defined(PSCAL_TARGET_IOS)
    VProc *active_vp = vprocCurrent();
    bool skip_host_restore = gShellBackgroundWorkerActive || gShellSkipHostStdio;
    if (gShellRuntime.pipeline.active && gShellRuntime.pipeline.in_process) {
        skip_host_restore = false;
    }
#else
    VProc *active_vp = NULL;
    bool skip_host_restore = false;
#endif
    for (size_t i = 0; i < count; ++i) {
        ShellExecRedirBackup *backup = &backups[i];
        if (backup->saved_valid && backup->saved_fd >= 0) {
            if (!skip_host_restore) {
                vprocHostDup2(backup->saved_fd, backup->target_fd);
            }
            if (backup->use_vproc && active_vp) {
                (void)vprocRestoreHostFd(active_vp, backup->target_fd, backup->saved_fd);
            }
            if (!skip_host_restore) {
                shellResetStandardStream(backup->target_fd);
            }
        } else if (backup->was_closed) {
            if (!skip_host_restore) {
                vprocHostClose(backup->target_fd);
                shellResetStandardStream(backup->target_fd);
            }
        }
    }
}

static void shellFreeExecRedirBackups(ShellExecRedirBackup *backups, size_t count) {
    if (!backups) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        if (backups[i].saved_valid && backups[i].saved_fd >= 0) {
            vprocHostClose(backups[i].saved_fd);
            backups[i].saved_fd = -1;
        }
    }
    free(backups);
}

static bool shellApplyExecRedirections(VM *vm, const ShellCommand *cmd,
                                       ShellExecRedirBackup **out_backups,
                                       size_t *out_count) {
    bool dbg = (getenv("PSCALI_REDIR_DEBUG") != NULL) || (getenv("PSCALI_PIPE_DEBUG") != NULL);
#if defined(PSCAL_TARGET_IOS)
    VProc *vp_current = vprocCurrent();
#else
    VProc *vp_current = NULL;
#endif
    if (out_backups) {
        *out_backups = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!cmd || cmd->redir_count == 0) {
        return true;
    }

    if (dbg) {
        fprintf(stderr, "[redir] begin count=%zu\n", cmd ? cmd->redir_count : 0);
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
                if (dbg) {
                    fprintf(stderr, "[redir] open fd=%d path=%s flags=0x%x mode=0%o\n",
                            target_fd, redir->path ? redir->path : "<null>",
                            redir->flags, redir->mode);
                }
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
                int fd = -1;
#if defined(PSCAL_TARGET_IOS)
                fd = vprocHostOpen(open_path, redir->flags, redir->mode);
#else
                fd = open(open_path, redir->flags, redir->mode);
#endif
                if (fd < 0) {
                    int err = errno;
                    if (dbg) {
                        fprintf(stderr, "[redir] open failed: %s\n", strerror(err));
                    }
                    runtimeError(vm, "exec: %s: %s", redir->path, strerror(err));
                    shellUpdateStatus(err ? err : 1);
                    goto redir_error;
                }
                int vfd = -1;
#if defined(PSCAL_TARGET_IOS)
                if (vp_current) {
                    vfd = vprocAdoptHostFd(vp_current, fd);
                    if (vfd < 0) {
                        vprocHostClose(fd);
                        runtimeError(vm, "exec: %s: %s", redir->path, strerror(errno));
                        shellUpdateStatus(errno ? errno : 1);
                        goto redir_error;
                    }
                }
#endif
                if (!shellDupOntoTarget(vp_current, (vfd >= 0) ? vfd : fd, target_fd, dbg, "open")) {
                    int err = errno;
                    if (dbg) {
                        fprintf(stderr, "[redir] dup %d->%d failed: %s\n", fd, target_fd, strerror(err));
                    }
                    runtimeError(vm, "exec: %s: %s", redir->path, strerror(err));
                    shellUpdateStatus(err ? err : 1);
#if defined(PSCAL_TARGET_IOS)
                    if (vp_current && vfd >= 0) {
                        vprocCloseShim(vfd);
                    } else
#endif
                    vprocHostClose(fd);
                    goto redir_error;
                }
#if defined(PSCAL_TARGET_IOS)
                if (vp_current && vfd >= 0) {
                    vprocCloseShim(vfd);
                } else
#endif
                vprocHostClose(fd);
                if (target_fd == STDOUT_FILENO) {
                    stdout_redirected = true;
                }
                break;
            }
            case SHELL_RUNTIME_REDIR_DUP: {
                if (dbg) {
                    fprintf(stderr, "[redir] dup fd=%d target=%d close=%d\n",
                            redir->dup_target_fd, target_fd, (int)redir->close_target);
                }
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
                    if (!shellDupOntoTarget(vp_current, redir->dup_target_fd, target_fd, dbg, "dup")) {
                        int err = errno;
                        if (dbg) {
                            fprintf(stderr, "[redir] dup2 %d->%d failed: %s\n",
                                    redir->dup_target_fd, target_fd, strerror(err));
                        }
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
static bool __attribute__((unused)) shellPipelineWriteBufferToPipe(const char *data, size_t len, int fd) {
    size_t written = 0;
    while (written < len) {
        ssize_t res = vprocHostWrite(fd, data + written, len - written);
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
        ssize_t res = vprocHostRead(fd, buffer + size, capacity - size);
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
    bool tool_dbg = (getenv("PSCALI_TOOL_DEBUG") != NULL) || (cmd && cmd->background);
    bool pipe_dbg = getenv("PSCALI_PIPE_DEBUG") != NULL;
    bool redir_dbg = getenv("PSCALI_REDIR_DEBUG") != NULL;
#if defined(PSCAL_TARGET_IOS)
    bool pipeline_in_process = gShellRuntime.pipeline.active &&
                               gShellRuntime.pipeline.in_process &&
                               !gShellBackgroundWorkerActive;
    VProc *active_vp = vprocCurrent();
    bool prev_skip_host = gShellSkipHostStdio;
    /* Background workers share the interactive stdio; avoid touching host fds. */
    if (gShellBackgroundWorkerActive) {
        gShellSkipHostStdio = true;
    }
    if (pipe_dbg) {
        fprintf(stderr, "[run-builtin] active_vp=%p stdin_fd=%d stdout_fd=%d stderr_fd=%d\n",
                (void *)active_vp, stdin_fd, stdout_fd, stderr_fd);
    }
#else
    bool pipeline_in_process = false;
    VProc *active_vp = NULL;
#endif
    if (redir_dbg) {
        fprintf(stderr, "[redir] cmd=%s argc=%zu redir_count=%zu apply_exec=%d\n",
                (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>",
                cmd ? cmd->argc : 0,
                cmd ? cmd->redir_count : 0,
                (int)apply_exec_redirs);
        if (cmd && cmd->argv) {
            for (size_t ai = 0; ai < (size_t)cmd->argc; ++ai) {
                fprintf(stderr, "[redir]   argv[%zu]=%s\n", ai, cmd->argv[ai] ? cmd->argv[ai] : "<null>");
            }
        }
        if (cmd && cmd->redirs) {
            for (size_t ri = 0; ri < cmd->redir_count; ++ri) {
                ShellRedirection *r = &cmd->redirs[ri];
                fprintf(stderr, "[redir]   redir[%zu] fd=%d kind=%d path=%s dup_target=%d flags=0x%x\n",
                        ri, r->fd, (int)r->kind, r->path ? r->path : "<none>", r->dup_target_fd, r->flags);
            }
        }
    }
    ShellExecRedirBackup *backups = NULL;
    size_t backup_count = 0;
    size_t backup_capacity = 0;
    int status = 127;
    bool applied_redirs = false;
    bool allow_missing_backup = pipeline_in_process;
    if (stdin_fd >= 0) {
        if (!shellEnsureExecRedirBackup(STDIN_FILENO, -1, &backups, &backup_count, &backup_capacity)) {
            if (tool_dbg && pipe_dbg) fprintf(stderr, "[run-builtin] backup stdin failed\n");
            if (!allow_missing_backup) goto cleanup;
        }
        if (!shellDupOntoTarget(active_vp, stdin_fd, STDIN_FILENO, pipe_dbg, "stdin")) {
            if (tool_dbg && pipe_dbg) fprintf(stderr, "[run-builtin] dup stdin failed\n");
            goto cleanup;
        }
    }
    if (stdout_fd >= 0) {
        if (!shellEnsureExecRedirBackup(STDOUT_FILENO, -1, &backups, &backup_count, &backup_capacity)) {
            if (tool_dbg && pipe_dbg) fprintf(stderr, "[run-builtin] backup stdout failed\n");
            if (!allow_missing_backup) goto cleanup;
        }
        if (!shellDupOntoTarget(active_vp, stdout_fd, STDOUT_FILENO, pipe_dbg, "stdout")) {
            if (tool_dbg && pipe_dbg) fprintf(stderr, "[run-builtin] dup stdout failed\n");
            goto cleanup;
        }
    }
    if (stderr_fd >= 0) {
        if (!shellEnsureExecRedirBackup(STDERR_FILENO, -1, &backups, &backup_count, &backup_capacity)) {
            if (tool_dbg && pipe_dbg) fprintf(stderr, "[run-builtin] backup stderr failed\n");
            if (!allow_missing_backup) goto cleanup;
        }
        if (!shellDupOntoTarget(active_vp, stderr_fd, STDERR_FILENO, pipe_dbg, "stderr")) {
            if (tool_dbg && pipe_dbg) fprintf(stderr, "[run-builtin] dup stderr failed\n");
            goto cleanup;
        }
    }

    if (apply_exec_redirs) {
        if (getenv("PSCALI_PIPE_DEBUG")) {
            fprintf(stderr, "[pipeline-bg] apply redirs count=%zu\n", cmd ? cmd->redir_count : 0);
        }
        if (!shellApplyExecRedirections(vm, cmd, &backups, &backup_count)) {
            goto cleanup;
        }
        applied_redirs = true;
    }

    if (tool_dbg && pipe_dbg) {
        fprintf(stderr, "[run-builtin] invoking cmd=%s argc=%zu redirs=%zu apply_exec_redirs=%d\n",
                (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>",
                cmd ? cmd->argc : 0,
                cmd ? cmd->redir_count : 0,
                (int)apply_exec_redirs);
    }
    if (pipe_dbg) {
        fprintf(stderr, "[run-builtin] begin lookup for %s\n",
                (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>");
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
         * pipeline mode. This mirrors the exec path but stays in-process. */
        bool was_background = gShellRuntime.pipeline.background;
        bool isolate_shebang_runtime =
            gShellRuntime.pipeline.active &&
            gShellRuntime.pipeline.in_process &&
            !gShellBackgroundWorkerActive;
        ShellRuntimeState *shebang_ctx = NULL;
        ShellRuntimeState *prev_ctx = NULL;
        if (was_background && gShellRuntime.pipeline.stage_count == 1 && getenv("PSCALI_PIPE_DEBUG")) {
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
                    shellResetPipeline();
                    return 0;
                }
                pthread_attr_destroy(&attr);
                fprintf(stderr, "[shebang-bg] failed to spawn thread, falling back inline\n");
                shellShebangThreadCtxFree(bg_ctx);
            }
        }
        const char *shebang_path = cmd->argv[0];
        char *resolved = shellFindExecutableOnPath(cmd->argv[0]);
        if (resolved) {
            shebang_path = resolved;
        }
        if (isolate_shebang_runtime) {
            shebang_ctx = shellRuntimeCreateContext();
            if (shebang_ctx) {
                prev_ctx = shellRuntimeActivateContext(shebang_ctx);
            }
        }
        int shebang_status = shellMaybeExecShebangTool(shebang_path, cmd->argv);
        if (shebang_ctx) {
            shellRuntimeActivateContext(prev_ctx);
            shellRuntimeDestroyContext(shebang_ctx);
        }
        if (resolved) {
            free(resolved);
        }
        if (shebang_status >= 0) {
            shellUpdateStatus(shebang_status);
            builtin_ran = true;
        }
    }
#endif
    if (!builtin_ran) {
        bool kill_dbg = getenv("PSCALI_KILL_DEBUG") != NULL;
        bool pipe_dbg = getenv("PSCALI_PIPE_DEBUG") != NULL;
        VmBuiltinFn handler = getVmBuiltinHandler(cmd && cmd->argv ? cmd->argv[0] : NULL);
        if (kill_dbg) {
            fprintf(stderr, "[exec] lookup cmd=%s handler=%p\n",
                    (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>",
                    (void *)handler);
        }
        if (pipe_dbg) {
            fprintf(stderr, "[exec] run cmd=%s argc=%zu handler=%p\n",
                    (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>",
                    cmd ? cmd->argc : 0,
                    (void *)handler);
        }
        if (handler) {
            int arg_count = (cmd->argc > 0) ? (int)cmd->argc - 1 : 0;
            Value *args = NULL;
            if (arg_count > 0) {
                args = calloc((size_t)arg_count, sizeof(Value));
                for (int i = 0; args && i < arg_count; ++i) {
                    args[i] = makeString(cmd->argv[i + 1]);
                }
            }
            handler(vm ? vm : gShellCurrentVm, arg_count, args);
            if (args) {
                for (int i = 0; i < arg_count; ++i) {
                    freeValue(&args[i]);
                }
                free(args);
            }
            builtin_ran = true;
        } else {
#if defined(PSCAL_TARGET_IOS)
            /* Fall back to smallclue applets executed in-process when no shell builtin exists. */
            const SmallclueApplet *applet = smallclueFindApplet(cmd && cmd->argv ? cmd->argv[0] : NULL);
            if (pipe_dbg) {
                fprintf(stderr, "[run-builtin] smallclue lookup name=%s applet=%p\n",
                        (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>",
                        (void *)applet);
            }
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
                        int sc_status = 0;
#if defined(PSCAL_TARGET_IOS)
                        char label[96] = {0};
                        shellVprocFormatLabel(cmd, label, sizeof(label));
                        VProcCommandScope vproc_scope;
                        bool inherit_pgid = false;
                        bool scoped = vprocCommandScopeBegin(&vproc_scope,
                                                             label[0] ? label : (cmd->argv && cmd->argv[0] ? cmd->argv[0] : "task"),
                                                             true,
                                                             inherit_pgid);
#endif
                        sc_status = smallclueDispatchApplet(applet, argc, argv_copy);
#if defined(PSCAL_TARGET_IOS)
                        const char *dbg = getenv("SMALLCLUE_DEBUG");
                        if (dbg && *dbg) {
                            fprintf(stderr, "[smallclue] dispatched applet pid=%d pgid=%d\n",
                                    vproc_scope.pid, vprocGetPgid(vproc_scope.pid));
                        }
                        if (scoped) {
                            vprocCommandScopeEnd(&vproc_scope, sc_status);
                        }
#endif
                        shellUpdateStatus(sc_status);
                        builtin_ran = true;
                    }
                    for (int i = 0; i < argc; ++i) {
                        free(argv_copy[i]);
                    }
                    free(argv_copy);
                }
            }
#endif
        }
        if (!builtin_ran) {
#if defined(PSCAL_TARGET_IOS)
            /* Try to interpret scripts in-process (no fork/exec on iOS). */
            char *resolved = NULL;
            const char *orig0 = cmd->argv ? cmd->argv[0] : NULL;
            const char *temp0 = orig0;
            if (orig0 && strchr(orig0, '/') == NULL) {
                resolved = shellFindExecutableOnPath(orig0);
                if (resolved) {
                    temp0 = resolved;
                }
            }
            int fb_status = -1;
            if (temp0) {
                char *saved0 = cmd->argv[0];
                cmd->argv[0] = (char *)temp0;
                fb_status = shellTryRunScriptFallback(vm ? vm : gShellCurrentVm, cmd);
                cmd->argv[0] = saved0;
            }
            if (resolved) free(resolved);
            if (fb_status >= 0) {
                shellUpdateStatus(fb_status);
                builtin_ran = true;
            }
#endif
        }
        if (!builtin_ran) {
            runtimeError(vm, "%s: command unavailable in pipeline", cmd->argv[0]);
            shellUpdateStatus(127);
            if (tool_dbg && pipe_dbg) {
                fprintf(stderr, "[run-builtin] builtin not found for %s\n", cmd && cmd->argv ? cmd->argv[0] : "<nil>");
            }
        }
    }
    shellFlushStandardStreams();
    status = gShellRuntime.last_status;

cleanup:
    if (tool_dbg && pipe_dbg && status == 127) {
        fprintf(stderr, "[run-builtin] exiting with status=127 cmd=%s apply_exec_redirs=%d stdin_fd=%d stdout_fd=%d stderr_fd=%d\n",
                (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>",
                (int)apply_exec_redirs,
                stdin_fd, stdout_fd, stderr_fd);
    }
#if defined(PSCAL_TARGET_IOS)
    if (tool_dbg && pipe_dbg) {
        fprintf(stderr, "[run-builtin] final fd map stdout_fd=%d stderr_fd=%d\n", stdout_fd, stderr_fd);
    }
#endif
    if (applied_redirs) {
        shellRestoreExecRedirections(backups, backup_count);
    } else if (backups) {
        shellRestoreExecRedirections(backups, backup_count);
    }
#if defined(PSCAL_TARGET_IOS)
    if (pipe_dbg && backups && backup_count > 0) {
        for (size_t bi = 0; bi < backup_count; ++bi) {
            fprintf(stderr, "[redir-restore] target=%d saved_fd=%d use_vproc=%d was_closed=%d\n",
                    backups[bi].target_fd,
                    backups[bi].saved_fd,
                    (int)backups[bi].use_vproc,
                    (int)backups[bi].was_closed);
        }
    }
#endif
    shellFreeExecRedirBackups(backups, backup_count);
#if defined(PSCAL_TARGET_IOS)
    /* Per-thread stream caches can hold buffered pipeline output; always close
     * them at stage end so data is flushed before the thread exits. */
    shellCloseVprocStreams();
    gShellSkipHostStdio = prev_skip_host;
#endif
    return status;
}

static int shellPipelineRunStage(VM *vm,
                                 ShellPipelineContext *ctx,
                                 ShellCommand *cmd,
                                 size_t stage_pos,
                                 bool pipeline_head,
                                 bool pipeline_tail) {
    int stdin_fd = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;
    struct pscal_fd *stdin_pscal = NULL;
    struct pscal_fd *stdout_pscal = NULL;
    struct pscal_fd *stderr_pscal = NULL;
    int status = 0;
    bool apply_exec_redirs = cmd && cmd->redir_count > 0;
    bool pipe_dbg = getenv("PSCALI_PIPE_DEBUG") != NULL;
    bool adopt_failed = false;

    bool pipeline_in_process = ctx && ctx->in_process;
    if (pipe_dbg) {
        fprintf(stderr,
                "[pipe-stage] begin stage=%zu cmd=%s head=%d tail=%d in_proc=%d\n",
                stage_pos,
                (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>",
                (int)pipeline_head,
                (int)pipeline_tail,
                (int)pipeline_in_process);
    }
//    fprintf(stderr,
//            "[pipe] stage=%zu head=%d tail=%d in_process=%d stdin_fd=%d stdout_fd=%d stderr_fd=%d\n",
 //           stage_pos, (int)pipeline_head, (int)pipeline_tail,
  //          (int)pipeline_in_process, stdin_fd, stdout_fd, stderr_fd);

    if (!pipeline_head) {
#if defined(PSCAL_TARGET_IOS)
        if (pipeline_in_process && ctx->pscal_pipe_read) {
            stdin_pscal = ctx->pscal_pipe_read[stage_pos - 1];
            stdin_fd = -2;
        } else
#endif
        {
            /* Duplicate the pipe endpoint for this stage and drop the shared
             * reference so EOF can propagate even while other threads run. */
            int fd = ctx->pipes[stage_pos - 1][0];
            if (fd >= 0) {
                int dup_fd = vprocHostDup(fd);
                if (dup_fd >= 0) {
                    stdin_fd = dup_fd;
                    vprocHostClose(fd);
                } else {
                    /* Fallback: transfer ownership of the original endpoint to
                     * this stage to avoid later double-close from shared state. */
                    stdin_fd = fd;
                }
                ctx->pipes[stage_pos - 1][0] = -1;
            }
        }
    }
    if (!pipeline_tail) {
#if defined(PSCAL_TARGET_IOS)
        if (pipeline_in_process && ctx->pscal_pipe_write) {
            stdout_pscal = ctx->pscal_pipe_write[stage_pos];
            stdout_fd = -2;
        } else
#endif
        {
            int fd = ctx->pipes[stage_pos][1];
            if (fd >= 0) {
                int dup_fd = vprocHostDup(fd);
                if (dup_fd >= 0) {
                    stdout_fd = dup_fd;
                    vprocHostClose(fd);
                } else {
                    /* Fallback: transfer ownership of the original endpoint to
                     * this stage to avoid later double-close from shared state. */
                    stdout_fd = fd;
                }
                ctx->pipes[stage_pos][1] = -1;
            }
        }
    }
    if (ctx->merge_stderr && stage_pos < ctx->stage_count && ctx->merge_stderr[stage_pos]) {
#if defined(PSCAL_TARGET_IOS)
        if (stdout_pscal) {
            stderr_pscal = stdout_pscal;
            stderr_fd = -2;
        //    fprintf(stderr, "[pipe] stage=%zu stderr merged to stdout pscal\n",
         //           stage_pos);
        } else
#endif
        {
            stderr_fd = (stdout_fd >= 0) ? stdout_fd : STDOUT_FILENO;
        }
    }

#if defined(PSCAL_TARGET_IOS)
    /* iOS: in-process pipelines are stable when stage vprocs inherit the
     * session PSCAL stdio context. */
    int pid_hint = -1;
    if (ctx && ctx->pids && stage_pos < ctx->stage_count && ctx->pids[stage_pos] > 0) {
        pid_hint = (int)ctx->pids[stage_pos];
    }
    bool inherit_pscal_stdio = true;
    VProc *vp = shellCreateVProcForStage(stdin_fd,
                                         stdout_fd,
                                         stderr_fd,
                                         pscalRuntimeDetectWindowCols(),
                                         pscalRuntimeDetectWindowRows(),
                                         pid_hint,
                                         inherit_pscal_stdio);
    if (!vp) {
        runtimeError(vm, "pipeline: failed to allocate vproc");
        shellUpdateStatus(1);
        return 1;
    }
    int shell_pid = vprocGetShellSelfPid();
    if (shell_pid > 0) {
        vprocSetParent(vprocPid(vp), shell_pid);
        int shell_sid = vprocGetSid(shell_pid);
        if (shell_sid > 0) {
            vprocSetSid(vprocPid(vp), shell_sid);
        }
    }
    pid_t group = (ctx->pgid > 0) ? ctx->pgid
                : ((ctx->launched > 0 && ctx->pids) ? ctx->pids[0] : -1);
    if (group <= 0) {
        group = vprocPid(vp);
    }
    vprocSetPgid(vprocPid(vp), group);
    ctx->pgid = group;
    if (cmd && cmd->argv && cmd->argc > 0) {
        char label[96] = {0};
        shellVprocFormatLabel(cmd, label, sizeof(label));
        if (!label[0] && cmd->argv[0]) {
            strncpy(label, cmd->argv[0], sizeof(label) - 1);
            label[sizeof(label) - 1] = '\0';
        }
        if (label[0]) {
            vprocSetCommandLabel(vprocPid(vp), label);
        }
    }
#if defined(PSCAL_TARGET_IOS)
    /* Keep pipeline stages in the foreground when possible, but always disable
     * stop semantics so SIGTTIN/TTOU do not park inline stages. */
    if (!ctx->background && !cmd->background && shellRuntimeIsInteractive()) {
        shellEnsureJobControl();
        if (gShellRuntime.job_control_enabled && ctx->pgid > 0) {
            shellJobControlSetForeground(ctx->pgid);
        }
    }
    vprocSetStopUnsupported(vprocPid(vp), true);
#endif
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    if (stdin_pscal) {
        int arc = vprocAdoptPscalFd(vp, STDIN_FILENO, stdin_pscal);
        if (arc != 0) {
            adopt_failed = true;
            if (pipe_dbg) {
                fprintf(stderr,
                        "[pipe-stage] adopt stdin failed stage=%zu pscal=%p errno=%d (%s)\n",
                        stage_pos, (void *)stdin_pscal, errno, strerror(errno));
            }
        }
    }
    if (stdout_pscal) {
        int arc = vprocAdoptPscalFd(vp, STDOUT_FILENO, stdout_pscal);
        if (arc != 0) {
            adopt_failed = true;
            if (pipe_dbg) {
                fprintf(stderr,
                        "[pipe-stage] adopt stdout failed stage=%zu pscal=%p errno=%d (%s)\n",
                        stage_pos, (void *)stdout_pscal, errno, strerror(errno));
            }
        }
    }
    if (!pipeline_tail && ctx && ctx->pscal_pipe_write_hold &&
        stage_pos < (ctx->stage_count > 0 ? ctx->stage_count - 1 : 0)) {
        if (ctx->pscal_pipe_write_hold[stage_pos]) {
            pscal_fd_close(ctx->pscal_pipe_write_hold[stage_pos]);
            ctx->pscal_pipe_write_hold[stage_pos] = NULL;
        }
    }
    if (stderr_pscal && stderr_pscal != stdout_pscal) {
        int arc = vprocAdoptPscalFd(vp, STDERR_FILENO, stderr_pscal);
        if (arc != 0) {
            adopt_failed = true;
            if (pipe_dbg) {
                fprintf(stderr,
                        "[pipe-stage] adopt stderr failed stage=%zu pscal=%p errno=%d (%s)\n",
                        stage_pos, (void *)stderr_pscal, errno, strerror(errno));
            }
        }
    }

    if (pipeline_in_process) {
        /* PSCAL pipe path: drop host dup immediately. */
        if (stdin_pscal && stdin_fd >= 0) {
            vprocHostClose(stdin_fd);
            stdin_fd = -1;
        }
        if (stdout_pscal && stdout_fd >= 0) {
            vprocHostClose(stdout_fd);
            stdout_fd = -1;
        }
        if (stderr_pscal && stderr_fd >= 0 && stderr_fd != stdout_fd) {
            vprocHostClose(stderr_fd);
            stderr_fd = -1;
        }
    }
#endif

    if (adopt_failed) {
        runtimeError(vm, "pipeline: failed to attach in-process stdio");
        status = 1;
    } else {
        status = shellRunBuiltinInProcess(vm, cmd, stdin_fd, stdout_fd, stderr_fd, apply_exec_redirs);
    }

    bool stderr_shared_with_stdout =
        (stderr_pscal && stdout_pscal && stderr_pscal == stdout_pscal);

    if (!pipeline_tail && stdout_fd >= 0) {
        vprocHostClose(stdout_fd);
    }
    if (!pipeline_head && stdin_fd >= 0) {
        vprocHostClose(stdin_fd);
    }
#if defined(PSCAL_TARGET_IOS)
    if (!pipeline_tail && stdout_pscal) {
        pscal_fd_close(stdout_pscal);
        if (ctx->pscal_pipe_write) {
            ctx->pscal_pipe_write[stage_pos] = NULL;
        }
        stdout_pscal = NULL;
    }
    if (!pipeline_head && stdin_pscal) {
        pscal_fd_close(stdin_pscal);
        if (ctx->pscal_pipe_read) {
            ctx->pscal_pipe_read[stage_pos - 1] = NULL;
        }
        stdin_pscal = NULL;
    }
    if (stderr_pscal && !stderr_shared_with_stdout) {
        pscal_fd_close(stderr_pscal);
        stderr_pscal = NULL;
    }
    if (stderr_shared_with_stdout) {
        stderr_pscal = NULL;
    }
#endif
#if defined(PSCAL_TARGET_IOS)
    if (ctx->pids && stage_pos < ctx->stage_count) {
        ctx->pids[stage_pos] = vprocPid(vp);
        ctx->launched = stage_pos + 1;
    }
    vprocMarkExit(vp, status);
    vprocDeactivate();
    vprocDiscard(vprocPid(vp));
    vprocDestroy(vp);
#else
    if (ctx->pids && stage_pos < ctx->stage_count) {
        ctx->pids[stage_pos] = 0;
        ctx->launched = stage_pos + 1;
    }
#endif
    if (pipe_dbg) {
        fprintf(stderr,
                "[pipe-stage] end stage=%zu cmd=%s status=%d\n",
                stage_pos,
                (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>",
                status);
    }
    return status;
}

static ShellPipelineContext *shellCloneSingleStagePipelineContext(const ShellPipelineContext *src) {
    ShellPipelineContext *clone = (ShellPipelineContext *)calloc(1, sizeof(ShellPipelineContext));
    if (!clone) {
        return NULL;
    }
    clone->active = true;
    clone->stage_count = 1;
    clone->negated = src ? src->negated : false;
    clone->background = src ? src->background : true;
    clone->last_status = src ? src->last_status : 0;
    clone->pgid = src ? src->pgid : -1;
#if defined(PSCAL_TARGET_IOS)
    clone->in_process = src ? src->in_process : true;
    clone->in_process_worker = false;
    clone->session_stdio = src ? src->session_stdio : NULL;
#endif
    clone->pids = (pid_t *)calloc(1, sizeof(pid_t));
    if (!clone->pids) {
        free(clone);
        return NULL;
    }
    if (src && src->pids) {
        clone->pids[0] = src->pids[0];
    }
    return clone;
}

static void shellFreeClonedPipelineContext(ShellPipelineContext *ctx) {
    if (!ctx) {
        return;
    }
    free(ctx->pids);
    free(ctx);
}

static void *shellPipelineStageThreadMain(void *arg) {
    ShellPipelineStageThreadCtx *tctx = (ShellPipelineStageThreadCtx *)arg;
    if (!tctx) {
        return NULL;
    }
#if defined(PSCAL_TARGET_IOS)
    VM *stage_vm = tctx->vm;
    ShellRuntimeState *bg_shell_ctx = NULL;
    ShellRuntimeState *prev_shell_ctx = NULL;
    VM *bg_vm = NULL;
    VM *prev_vm = NULL;
    bool worker_ctx_ready = true;
    static pthread_once_t bg_builtin_once = PTHREAD_ONCE_INIT;
    pthread_once(&bg_builtin_once, registerExtendedBuiltins);
    bg_shell_ctx = shellRuntimeCreateContext();
    if (bg_shell_ctx) {
        prev_shell_ctx = shellRuntimeActivateContext(bg_shell_ctx);
        bg_vm = (VM *)calloc(1, sizeof(VM));
        if (bg_vm) {
            initVM(bg_vm);
            bg_vm->frontendContext = bg_shell_ctx;
            prev_vm = shellSwapCurrentVm(bg_vm);
            stage_vm = bg_vm;
        } else {
            shellRuntimeActivateContext(prev_shell_ctx);
            prev_shell_ctx = NULL;
            free(bg_shell_ctx);
            bg_shell_ctx = NULL;
            worker_ctx_ready = false;
        }
    } else {
        worker_ctx_ready = false;
    }

    PSCALRuntimeContext *prev_runtime_ctx = NULL;
    bool swapped_runtime = false;
    if (PSCALRuntimeGetCurrentRuntimeContext) {
        prev_runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext();
    }
    if (PSCALRuntimeSetCurrentRuntimeContext && tctx->runtime_ctx) {
        PSCALRuntimeSetCurrentRuntimeContext(tctx->runtime_ctx);
        swapped_runtime = true;
    }
    VProcSessionStdio *prev_session_stdio = vprocSessionStdioCurrent();
    bool swapped_stdio = false;
    if (tctx->session_stdio && tctx->session_stdio != prev_session_stdio) {
        vprocSessionStdioActivate(tctx->session_stdio);
        swapped_stdio = true;
    }
#endif
    int status = 1;
#if defined(PSCAL_TARGET_IOS)
    if (!worker_ctx_ready) {
        if (getenv("PSCALI_PIPE_DEBUG")) {
            fprintf(stderr,
                    "[pipe-thread] stage=%zu cmd=%s: unable to allocate isolated worker context\n",
                    tctx->stage_pos,
                    (tctx->cmd && tctx->cmd->argv && tctx->cmd->argv[0]) ? tctx->cmd->argv[0] : "<nil>");
        }
    } else
#endif
    {
        vprocSetPipelineStage(true);
        status = shellPipelineRunStage(stage_vm,
                                       tctx->ctx,
                                       tctx->cmd,
                                       tctx->stage_pos,
                                       tctx->pipeline_head,
                                       tctx->pipeline_tail);
        vprocSetPipelineStage(false);
    }
    if (getenv("PSCALI_PIPE_DEBUG")) {
        fprintf(stderr,
                "[pipe-thread] stage=%zu cmd=%s status=%d tail=%d bg=%d\n",
                tctx->stage_pos,
                (tctx->cmd && tctx->cmd->argv && tctx->cmd->argv[0]) ? tctx->cmd->argv[0] : "<nil>",
                status,
                (int)tctx->pipeline_tail,
                (int)(tctx->ctx ? tctx->ctx->background : false));
    }
#if defined(PSCAL_TARGET_IOS)
    if (bg_vm || prev_vm || bg_shell_ctx || prev_shell_ctx) {
        if (bg_vm || prev_vm) {
            shellRestoreCurrentVm(prev_vm);
        }
        if (prev_shell_ctx) {
            shellRuntimeActivateContext(prev_shell_ctx);
        }
        if (bg_vm) {
            freeVM(bg_vm);
            free(bg_vm);
        }
        if (bg_shell_ctx) {
            free(bg_shell_ctx);
        }
    }
    if (swapped_stdio) {
        vprocSessionStdioActivate(prev_session_stdio);
    }
    if (swapped_runtime && PSCALRuntimeSetCurrentRuntimeContext) {
        PSCALRuntimeSetCurrentRuntimeContext(prev_runtime_ctx);
    }
#endif
    /*
     * Do not mutate shared pipeline bookkeeping from worker threads.
     * Background in-process stages can outlive the foreground command loop and
     * race with later pipeline reuse/reset, which risks touching stale pointers.
     * Foreground pipeline completion/teardown is still handled on the caller
     * path where stage joins are explicit.
     */
    if (tctx->cmd) {
        shellFreeCommand(tctx->cmd);
        free(tctx->cmd);
    }
    if (tctx->owns_ctx_snapshot) {
        shellFreeClonedPipelineContext(tctx->ctx);
        tctx->ctx = NULL;
    }
    free(tctx);
    return NULL;
}

static bool shellPipelineExecuteStageInProcess(VM *vm,
                                               ShellPipelineContext *ctx,
                                               ShellCommand *cmd,
                                               size_t stage_pos,
                                               bool pipeline_head,
                                               bool pipeline_tail) {
//    fprintf(stderr,
 //           "[pipe] launch stage=%zu head=%d tail=%d in_process=%d read[%zu]=%p write[%zu]=%p\n",
  //          stage_pos,
   //         (int)pipeline_head,
    //        (int)pipeline_tail,
     //       (ctx ? (int)ctx->in_process : 0),
      //      (stage_pos > 0) ? stage_pos - 1 : 0,
       //     (ctx && stage_pos > 0 && ctx->pscal_pipe_read) ? (void *)ctx->pscal_pipe_read[stage_pos - 1] : NULL,
        //    stage_pos,
         //   (ctx && ctx->pscal_pipe_write) ? (void *)ctx->pscal_pipe_write[stage_pos] : NULL);
    if (!ctx || !ctx->active || stage_pos >= ctx->stage_count) {
        runtimeError(vm, "pipeline: invalid context");
        shellUpdateStatus(1);
        if (ctx) {
            shellAbortPipeline();
        }
        return false;
    }
    if (ctx->stage_count > 1 && !ctx->pipes) {
        runtimeError(vm, "pipeline: pipes unavailable");
        shellUpdateStatus(1);
        shellAbortPipeline();
        return false;
    }

    /* Run single-stage background tails on a worker thread so the shell prompt returns. */
    bool async_tail = ctx->background && pipeline_tail && ctx->stage_count == 1;
    pid_t async_bg_pgid = -1;
    if (!pipeline_tail || async_tail) {
        if (async_tail && ctx->pids && stage_pos < ctx->stage_count) {
            if (ctx->pids[stage_pos] <= 0) {
                int pid_hint = vprocReservePid();
                if (pid_hint > 0) {
                    ctx->pids[stage_pos] = (pid_t)pid_hint;
                }
            }
            if (ctx->pids[stage_pos] > 0 && ctx->launched < (stage_pos + 1)) {
                ctx->launched = stage_pos + 1;
            }
            if (ctx->pgid <= 0 && ctx->pids[stage_pos] > 0) {
                ctx->pgid = ctx->pids[stage_pos];
            }
            if (ctx->pids[stage_pos] > 0) {
                async_bg_pgid = (ctx->pgid > 0) ? ctx->pgid : ctx->pids[stage_pos];
                int shell_pid = vprocGetShellSelfPid();
                if (shell_pid > 0 && ctx->pids[stage_pos] != shell_pid) {
                    int shell_sid = vprocGetSid(shell_pid);
                    if (shell_sid > 0) {
                        (void)vprocSetSid((int)ctx->pids[stage_pos], shell_sid);
                    }
                    (void)vprocSetParent((int)ctx->pids[stage_pos], shell_pid);
                }
                if (async_bg_pgid > 0) {
                    (void)vprocSetPgid((int)ctx->pids[stage_pos], (int)async_bg_pgid);
                }
            }
        }

        ShellPipelineStageThreadCtx *tctx =
            (ShellPipelineStageThreadCtx *)calloc(1, sizeof(ShellPipelineStageThreadCtx));
        ShellCommand *cloned = shellCloneCommandForBackground(cmd);
        if (!tctx || !cloned) {
            free(tctx);
            shellFreeCommand(cloned);
            free(cloned);
            runtimeError(vm, "pipeline: unable to launch stage");
            shellUpdateStatus(1);
            shellAbortPipeline();
            return false;
        }
        tctx->vm = vm;
        tctx->ctx = ctx;
        tctx->cmd = cloned;
        tctx->stage_pos = stage_pos;
        tctx->pipeline_head = pipeline_head;
        tctx->pipeline_tail = pipeline_tail;
        tctx->owns_ctx_snapshot = false;
#if defined(PSCAL_TARGET_IOS)
        tctx->session_stdio = ctx->session_stdio;
        tctx->runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext
            ? PSCALRuntimeGetCurrentRuntimeContext()
            : NULL;
#endif
        if (async_tail) {
            ShellPipelineContext *ctx_clone = shellCloneSingleStagePipelineContext(ctx);
            if (!ctx_clone) {
                shellFreeCommand(cloned);
                free(cloned);
                free(tctx);
                runtimeError(vm, "pipeline: unable to snapshot background stage");
                shellUpdateStatus(1);
                shellAbortPipeline();
                return false;
            }
            tctx->ctx = ctx_clone;
            tctx->owns_ctx_snapshot = true;
        }

        pthread_t stage_thread = 0;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, SHELL_TOOL_THREAD_STACK_SZ);
        if (async_tail) {
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        }
        pthread_t *thread_slot = async_tail ? &stage_thread : &ctx->threads[stage_pos];
        int rc = pthread_create(thread_slot, &attr, shellPipelineStageThreadMain, tctx);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            if (tctx->cmd) {
                shellFreeCommand(tctx->cmd);
                free(tctx->cmd);
                tctx->cmd = NULL;
            }
            if (tctx->owns_ctx_snapshot) {
                shellFreeClonedPipelineContext(tctx->ctx);
                tctx->ctx = NULL;
            }
            free(tctx);
            runtimeError(vm, "pipeline: failed to create stage thread");
            shellUpdateStatus(1);
            shellAbortPipeline();
            return false;
        }
        if (!async_tail) {
            ctx->thread_started[stage_pos] = true;
        }
        if (async_tail) {
            if (ctx->pids && stage_pos < ctx->stage_count && ctx->pids[stage_pos] > 0) {
                pid_t bg_pgid = (async_bg_pgid > 0) ? async_bg_pgid : ctx->pids[stage_pos];
                pid_t bg_pids[1] = { ctx->pids[stage_pos] };
                ShellJob *job = shellRegisterJob(bg_pgid, bg_pids, 1, cmd);
                if (job) {
                    shellJobMarkRunning(job);
                    job->last_status = 0;
                }
                gShellRuntime.last_bg_pid = bg_pgid;
#if defined(PSCAL_TARGET_IOS)
                shellPrintJobStarted(job, bg_pgid);
#endif
            }
            /* Background single-stage workers run detached from shared pipeline state. */
            shellResetPipeline();
            shellUpdateStatus(0);
        }
        return true;
    }

    /* Tail stage runs inline so we can join upstream threads afterward. */
    vprocSetPipelineStage(true);
    int status = shellPipelineRunStage(vm, ctx, cmd, stage_pos, pipeline_head, pipeline_tail);
    vprocSetPipelineStage(false);
    ctx->last_status = status;
    if (ctx->stage_status && stage_pos < ctx->stage_count) {
        ctx->stage_status[stage_pos] = status;
    }

    for (size_t i = 0; i < stage_pos; ++i) {
        if (ctx->thread_started && ctx->thread_started[i]) {
            if (getenv("PSCALI_PIPE_DEBUG")) {
                fprintf(stderr, "[pipe-join] waiting stage=%zu\n", i);
            }
            pthread_join(ctx->threads[i], NULL);
            ctx->thread_started[i] = false;
            if (getenv("PSCALI_PIPE_DEBUG")) {
                fprintf(stderr, "[pipe-join] joined stage=%zu\n", i);
            }
        }
    }

    /* Proactively close any remaining pipe endpoints now that the pipeline is
     * complete. This ensures blocked readers see EOF even if an upstream
     * stage bailed before cleaning up. */
    if (ctx->pipes) {
        size_t pipe_count = (ctx->stage_count > 0) ? (ctx->stage_count - 1) : 0;
        for (size_t i = 0; i < pipe_count; ++i) {
            if (ctx->pipes[i][0] >= 0) {
                vprocHostClose(ctx->pipes[i][0]);
                ctx->pipes[i][0] = -1;
            }
            if (ctx->pipes[i][1] >= 0) {
                vprocHostClose(ctx->pipes[i][1]);
                ctx->pipes[i][1] = -1;
            }
        }
    }
    return true;
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
    bool prefer_worker = shellShouldRunSmallclueInWorker(cmd);
    bool force_pipeline_builtin = false;
    if (cmd->argc > 0 && !shellCommandIsExecBuiltin(cmd)) {
        const char *cmd_name = (cmd->argv && cmd->argv[0]) ? cmd->argv[0] : NULL;
        if (shellIsRuntimeBuiltin(cmd_name) || smallclueFindApplet(cmd_name) != NULL) {
            force_pipeline_builtin = true;
        }
    }
#endif
#if defined(PSCAL_TARGET_IOS)
    if (cmd->background) {
        bool need_new = (!ctx->active) || (!ctx->background) || (ctx->stage_count != 1) ||
                        (!cmd->is_pipeline_head) || (!cmd->is_pipeline_tail);
        if (need_new) {
            shellResetPipeline();
            if (!shellEnsurePipeline(1, false)) {
                runtimeError(vm, "pipeline: unable to allocate background context");
                shellUpdateStatus(1);
                shellFreeCommand(cmd);
                shellScalarCacheInvalidate();
                return makeVoid();
            }
            ctx = &gShellRuntime.pipeline;
            ctx->active = true;
            ctx->in_process = true;
            ctx->stage_count = 1;
            ctx->launched = 0;
            ctx->background = true;
            cmd->pipeline_index = 0;
            cmd->is_pipeline_head = true;
            cmd->is_pipeline_tail = true;
            if (getenv("PSCALI_PIPE_DEBUG")) {
                fprintf(stderr, "[pipeline-bg] auto in-process pipeline reset for background '%s'\n",
                        (cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<null>");
            }
        }
    }
#endif
#if defined(PSCAL_TARGET_IOS)
    if (force_pipeline_builtin && !ctx->active && !cmd->background) {
        if (!shellEnsurePipeline(1, false)) {
            force_pipeline_builtin = false;
        } else {
            ctx = &gShellRuntime.pipeline;
            cmd->pipeline_index = 0;
            cmd->is_pipeline_head = true;
            cmd->is_pipeline_tail = true;
        }
    }
#endif
#if defined(PSCAL_TARGET_IOS)
    if (prefer_worker && !ctx->active && !cmd->background) {
        if (!shellEnsurePipeline(1, false)) {
            prefer_worker = false;
        } else {
            ctx = &gShellRuntime.pipeline;
            cmd->pipeline_index = 0;
            cmd->is_pipeline_head = true;
            cmd->is_pipeline_tail = true;
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
    /* Allow VAR=val words attached to local/declare/typeset to bind into the
     * current function's local scope instead of the environment. */
    bool previous_local_scope_flag = gShellRuntime.local_scope_active;
    bool localizes_assignments = false;
    if (cmd->argc > 0 && cmd->argv && cmd->argv[0]) {
        const char *canon = shellBuiltinCanonicalName(cmd->argv[0]);
        if (canon && (strcmp(canon, "local") == 0 ||
                      strcmp(canon, "declare") == 0 ||
                      strcmp(canon, "typeset") == 0)) {
            if (shellLocalScopeDepth() > 0) {
                localizes_assignments = true;
                gShellRuntime.local_scope_active = true;
            }
        }
    }
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
            if (localizes_assignments) {
                gShellRuntime.local_scope_active = previous_local_scope_flag;
            }
            return makeVoid();
        }
        assignments_applied = true;
    }
    if (localizes_assignments) {
        gShellRuntime.local_scope_active = previous_local_scope_flag;
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
        if (ctx->stage_count == 1 && !ctx->background && !cmd->background
#if defined(PSCAL_TARGET_IOS)
            && !force_pipeline_builtin
#endif
            ) {
#if defined(PSCAL_TARGET_IOS)
            if (!prefer_worker) {
                ShellVprocLabelScope label_scope;
                shellVprocLabelScopeBegin(&label_scope, cmd);
                bool ran_builtin = shellInvokeBuiltin(vm, cmd);
                shellVprocLabelScopeEnd(&label_scope);
                if (ran_builtin) {
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
            }
#else
            bool ran_builtin = shellInvokeBuiltin(vm, cmd);
            if (ran_builtin) {
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
#endif
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
        bool pipeline_uses_pipes = ctx->pipes != NULL;
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
                if (ctx->background) {
                    shellScalarCacheInvalidate();
                    return makeVoid();
                }
                /* Foreground in-process pipelines may have non-tail stages
                 * running on worker threads. Always finish via shellFinishPipeline
                 * so those stages are waited/joined before teardown. */
                shellFinishPipeline(cmd);
                shellRuntimeProcessPendingSignals();
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
        {
#if defined(PSCAL_TARGET_IOS)
            ShellVprocLabelScope label_scope;
            shellVprocLabelScopeBegin(&label_scope, cmd);
#endif
            bool ran_builtin = shellInvokeBuiltin(vm, cmd);
#if defined(PSCAL_TARGET_IOS)
            shellVprocLabelScopeEnd(&label_scope);
#endif
            if (ran_builtin) {
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
            ShellWaitResult wait_res_child;
            shellWaitPidDetailed(child, job_control, &wait_res_child);
            status = wait_res_child.status_mapped;
            stopped = wait_res_child.stopped;
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
                    shellJobMarkStopped(job, status);
                }
#if defined(PSCAL_TARGET_IOS)
                shellPrintJobSuspended(job, cmd);
#endif
                shellUpdateStatus(status);
                shellFreeCommand(cmd);
                shellScalarCacheInvalidate();
                return makeVoid();
            }
        } else {
        pid_t job_pids[1];
        job_pids[0] = child;
        gShellRuntime.last_bg_pid = child;
        ShellJob *job = shellRegisterJob(child, job_pids, 1, cmd);
        if (job) {
            shellJobMarkRunning(job);
            job->last_status = 0;
        }
    #if defined(PSCAL_TARGET_IOS)
        shellPrintJobStarted(job, child);
    #endif
        status = 0;
        }
        shellUpdateStatus(status);
        shellRuntimeProcessPendingSignals();
    }

    shellFreeCommand(cmd);
    shellScalarCacheInvalidate();
    return makeVoid();
}
