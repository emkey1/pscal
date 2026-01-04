/* Auto-generated include: runtime state and globals. Included from shell.c. */
#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "shell/word_encoding.h"
#include "shell/quote_markers.h"
#include "shell/function.h"
#include "shell/builtins.h"
#include "shell/runner.h"
#include "vm/vm.h"
#include "Pascal/globals.h"
#include "pscal_paths.h"
#include "common/runtime_tty.h"
#include "ios/vproc.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <regex.h>
#include <stdbool.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/times.h>
#include <termios.h>
#include <unistd.h>
#include <pwd.h>
#include <pthread.h>
#include <time.h>

#if defined(PSCAL_TARGET_IOS)
typedef struct {
    FILE *fp;
    int host_fd;
    int std_fd;
} ShellVprocStream;

typedef struct {
    int std_fd;
    bool can_read;
    bool can_write;
} ShellVprocShimCookie;

static __thread ShellVprocStream gShellVprocStdout = { NULL, -1, STDOUT_FILENO };
static __thread ShellVprocStream gShellVprocStderr = { NULL, -1, STDERR_FILENO };
static __thread ShellVprocStream gShellVprocStdin = { NULL, -1, STDIN_FILENO };

static FILE *shellVprocStreamFallback(int std_fd) {
    if (std_fd == STDIN_FILENO) {
        return stdin;
    }
    if (std_fd == STDOUT_FILENO) {
        return stdout;
    }
    return stderr;
}

static int shellVprocShimRead(void *cookie, char *buf, int len) {
    ShellVprocShimCookie *ctx = (ShellVprocShimCookie *)cookie;
    if (!ctx || !ctx->can_read || !buf || len <= 0) {
        errno = EBADF;
        return -1;
    }
    ssize_t res = vprocReadShim(ctx->std_fd, buf, (size_t)len);
    if (res < 0) {
        return -1;
    }
    if (res > INT_MAX) {
        res = INT_MAX;
    }
    return (int)res;
}

static int shellVprocShimWrite(void *cookie, const char *buf, int len) {
    ShellVprocShimCookie *ctx = (ShellVprocShimCookie *)cookie;
    if (!ctx || !ctx->can_write || !buf || len <= 0) {
        errno = EBADF;
        return -1;
    }
    ssize_t res = vprocWriteShim(ctx->std_fd, buf, (size_t)len);
    if (res < 0) {
        return -1;
    }
    if (res > INT_MAX) {
        res = INT_MAX;
    }
    return (int)res;
}

static int shellVprocShimClose(void *cookie) {
    free(cookie);
    return 0;
}

static FILE *shellVprocStreamOpenShim(int std_fd,
                                      ShellVprocStream *cache,
                                      const char *mode,
                                      int buf_mode) {
    if (!cache || !mode) {
        return shellVprocStreamFallback(std_fd);
    }

    if (cache->fp && cache->host_fd < 0) {
        return cache->fp;
    }

    if (cache->fp) {
        fflush(cache->fp);
        fclose(cache->fp);
        cache->fp = NULL;
    }
    cache->host_fd = -1;

    ShellVprocShimCookie *cookie = (ShellVprocShimCookie *)calloc(1, sizeof(ShellVprocShimCookie));
    if (!cookie) {
        return shellVprocStreamFallback(std_fd);
    }
    cookie->std_fd = std_fd;
    cookie->can_read = strchr(mode, 'r') != NULL;
    cookie->can_write = (strchr(mode, 'w') != NULL) || (strchr(mode, 'a') != NULL);

    FILE *fp = funopen(cookie,
                       cookie->can_read ? shellVprocShimRead : NULL,
                       cookie->can_write ? shellVprocShimWrite : NULL,
                       NULL,
                       shellVprocShimClose);
    if (!fp) {
        free(cookie);
        return shellVprocStreamFallback(std_fd);
    }
    if (buf_mode >= 0) {
        setvbuf(fp, NULL, buf_mode, 0);
    }
    cache->fp = fp;
    cache->host_fd = -1;
    return fp;
}

static FILE *shellVprocStreamOpen(int std_fd,
                                  ShellVprocStream *cache,
                                  const char *mode,
                                  int buf_mode) {
    if (!cache || !mode) {
        return shellVprocStreamFallback(std_fd);
    }

    int host_fd = std_fd;
    bool use_host_stream = true;
    VProc *vp = vprocCurrent();
    if (vp) {
        int translated = vprocTranslateFd(vp, std_fd);
        if (translated >= 0) {
            host_fd = translated;
        } else {
            use_host_stream = false;
        }
    }

    if (!vp) {
        use_host_stream = true;
    }

    if (!use_host_stream) {
        return shellVprocStreamOpenShim(std_fd, cache, mode, buf_mode);
    }

    if (cache->fp && cache->host_fd == host_fd) {
        return cache->fp;
    }

    if (cache->fp) {
        fflush(cache->fp);
        fclose(cache->fp);
        cache->fp = NULL;
    }
    cache->host_fd = -1;

    int dup_fd = -1;
#ifdef F_DUPFD_CLOEXEC
    dup_fd = fcntl(host_fd, F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0 && errno == EINVAL) {
        dup_fd = -1;
    }
#endif
    if (dup_fd < 0) {
        dup_fd = dup(host_fd);
    }
    if (dup_fd < 0) {
        return shellVprocStreamFallback(std_fd);
    }

    FILE *fp = fdopen(dup_fd, mode);
    if (!fp) {
        close(dup_fd);
        return shellVprocStreamFallback(std_fd);
    }
    if (buf_mode >= 0) {
        setvbuf(fp, NULL, buf_mode, 0);
    }
    cache->fp = fp;
    cache->host_fd = host_fd;
    return fp;
}

static FILE *shellVprocStdout(void) {
    int buf_mode = pscalRuntimeStdoutIsInteractive() ? _IOLBF : _IOFBF;
    return shellVprocStreamOpen(STDOUT_FILENO, &gShellVprocStdout, "w", buf_mode);
}

static FILE *shellVprocStderr(void) {
    return shellVprocStreamOpen(STDERR_FILENO, &gShellVprocStderr, "w", _IONBF);
}

static FILE *shellVprocStdin(void) {
    return shellVprocStreamOpen(STDIN_FILENO, &gShellVprocStdin, "r", -1);
}

static int shellVprocPuts(const char *text) {
    FILE *out = shellVprocStdout();
    const char *value = text ? text : "(null)";
    if (fputs(value, out) == EOF) {
        return EOF;
    }
    return fputc('\n', out);
}

static void shellVprocPerror(const char *label) {
    int saved_errno = errno;
    FILE *errfp = shellVprocStderr();
    if (label && *label) {
        fprintf(errfp, "%s: %s\n", label, strerror(saved_errno));
    } else {
        fprintf(errfp, "%s\n", strerror(saved_errno));
    }
    errno = saved_errno;
}

static void shellFlushVprocStreams(void) {
    if (gShellVprocStdout.fp) {
        fflush(gShellVprocStdout.fp);
    }
    if (gShellVprocStderr.fp) {
        fflush(gShellVprocStderr.fp);
    }
}

static void shellCloseVprocStream(ShellVprocStream *stream) {
    if (!stream || !stream->fp) {
        return;
    }
    fclose(stream->fp);
    stream->fp = NULL;
    stream->host_fd = -1;
}

static void shellCloseVprocStreams(void) {
    shellCloseVprocStream(&gShellVprocStdout);
    shellCloseVprocStream(&gShellVprocStderr);
    shellCloseVprocStream(&gShellVprocStdin);
}
#else
static void shellFlushVprocStreams(void) {}
static void __attribute__((unused)) shellCloseVprocStreams(void) {}
#endif

static void shellFlushStandardStreams(void) {
    shellFlushVprocStreams();
    fflush(stdout);
    fflush(stderr);
}

static void shellResetStandardStream(int fd);

#if defined(__GLIBC__) || defined(__ANDROID__)
#define SHELL_HAVE_SECURE_GETENV 1
extern char *secure_getenv(const char *name);
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#define SHELL_HAVE_ISSETUGID 1
#endif

static bool shellProcessIsPrivileged(void) {
#if defined(SHELL_HAVE_ISSETUGID)
    if (issetugid()) {
        return true;
    }
#endif
    if (geteuid() != getuid() || getegid() != getgid()) {
        return true;
    }
    return false;
}

static const char *shellSafeGetenv(const char *name) {
    if (!name) {
        return NULL;
    }
#if defined(SHELL_HAVE_SECURE_GETENV)
    if (shellProcessIsPrivileged()) {
        return NULL;
    }
    const char *value = secure_getenv(name);
    if (!value) {
        return NULL;
    }
    return value;
#else
    if (shellProcessIsPrivileged()) {
        return NULL;
    }
    return getenv(name);
#endif
}

extern char **environ;

#define SHELL_ARRAY_ELEMENT_SEP '\x1d'

typedef enum {
    SHELL_ARRAY_KIND_INDEXED,
    SHELL_ARRAY_KIND_ASSOCIATIVE
} ShellArrayKind;

typedef struct {
    char *name;
    char **values;
    char **keys;
    size_t count;
    ShellArrayKind kind;
} ShellArrayVariable;

typedef struct {
    char *name;
} ShellReadonlyEntry;

typedef struct ShellAlias {
    char *name;
    char *value;
} ShellAlias;

typedef struct {
    char *name;
    char *path;
    unsigned long hits;
} ShellHashEntry;

typedef struct {
    char **entries;
    size_t count;
    size_t capacity;
} ShellHistory;

typedef struct ShellTrapEntry {
    ShellTrapAction action;
    char *command;
} ShellTrapEntry;

typedef struct ShellJob {
    pid_t pgid;
    pid_t *pids;
    size_t pid_count;
    size_t id;
    bool running;
    bool stopped;
    bool disowned;
    int last_status;
    char *command;
} ShellJob;

typedef struct ShellLocalScopeFrame ShellLocalScopeFrame;
typedef struct ShellLoopFrame ShellLoopFrame;

typedef struct {
    bool active;
    size_t stage_count;
    bool negated;
    bool *merge_stderr;
    pid_t *pids;
    int (*pipes)[2];
    size_t launched;
    bool background;
    int last_status;
    pid_t pgid;
#if defined(PSCAL_TARGET_IOS)
    bool in_process;
    bool in_process_worker;
    int spool_fd;
    size_t spool_size;
#endif
} ShellPipelineContext;

typedef struct ShellRuntimeState {
    int last_status;
    ShellPipelineContext pipeline;
    pid_t shell_pgid;
    pid_t last_bg_pid;
    int tty_fd;
    bool job_control_enabled;
    bool job_control_initialized;
    bool errexit_enabled;
    bool errexit_pending;
    bool errexit_defer_active;
    bool trap_enabled;
    bool posix_mode;
    bool local_scope_active;
    bool break_requested;
    bool continue_requested;
    bool pipeline_subshell_active;
    int break_requested_levels;
    int continue_requested_levels;
    char **dir_stack;
    size_t dir_stack_count;
    size_t dir_stack_capacity;
    bool dir_stack_initialised;
    ShellHashEntry *hash_entries;
    size_t hash_count;
    size_t hash_capacity;
    char **disabled_builtins;
    size_t disabled_builtin_count;
    size_t disabled_builtin_capacity;
    int getopts_index;
    int getopts_offset;
    unsigned long scalar_cache_hits;
    unsigned long scalar_cache_misses;
    unsigned long scalar_cache_stores;
    size_t scalar_cache_generation;
    size_t scalar_capture_peak_depth;
    ShellHistory history;
    char *arg0;
    ShellTrapEntry signal_traps[NSIG];
    ShellTrapEntry exit_trap;
    bool trap_executing;
    int script_depth;
    int condition_depth;
    size_t job_next_id;
    ShellJob *jobs;
    size_t job_count;
    ShellArrayVariable *array_vars;
    size_t array_var_count;
    size_t array_var_capacity;
    int associative_array_support;
    int bind_interactive_status;
    ShellReadonlyEntry *readonly_vars;
    size_t readonly_var_count;
    size_t readonly_var_capacity;
    char *readonly_error_name;
    /* Per-context locals/loops/history stacks */
    ShellLocalScopeFrame *local_scope_stack;
    size_t local_scope_depth;
    size_t local_scope_capacity;
    ShellLoopFrame *loop_stack;
    size_t loop_stack_size;
    size_t loop_stack_capacity;
} ShellRuntimeState;

static ShellRuntimeState *shellGetContext(void);
static void shellSetContext(ShellRuntimeState *ctx);

#define SC() (shellGetContext())

#define gShellLocalScopeStack (SC()->local_scope_stack)
#define gShellLocalScopeDepth (SC()->local_scope_depth)
#define gShellLocalScopeCapacity (SC()->local_scope_capacity)
#define gShellArrayVars (SC()->array_vars)
#define gShellArrayVarCount (SC()->array_var_count)
#define gShellArrayVarCapacity (SC()->array_var_capacity)
#define gShellAssociativeArraySupport (SC()->associative_array_support)
#define gShellBindInteractiveStatus (SC()->bind_interactive_status)
#define gShellReadonlyVars (SC()->readonly_vars)
#define gShellReadonlyVarCount (SC()->readonly_var_count)
#define gShellReadonlyVarCapacity (SC()->readonly_var_capacity)
#define gShellReadonlyErrorName (SC()->readonly_error_name)
#define gShellHistory (SC()->history)
#define gShellArg0 (SC()->arg0)
#define gShellSignalTraps (SC()->signal_traps)
#define gShellExitTrap (SC()->exit_trap)
#define gShellTrapExecuting (SC()->trap_executing)
#define gShellScriptDepth (SC()->script_depth)
#define gShellConditionDepth (SC()->condition_depth)
#define gShellJobs (SC()->jobs)
#define gShellJobCount (SC()->job_count)

static const char *kShellCommandDefaultPath =
    "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

typedef enum {
    SHELL_COMMAND_RESULT_ALIAS,
    SHELL_COMMAND_RESULT_FUNCTION,
    SHELL_COMMAND_RESULT_BUILTIN,
    SHELL_COMMAND_RESULT_FILE
} ShellCommandResultKind;

typedef struct {
    ShellCommandResultKind kind;
    char *detail;
} ShellCommandResult;

typedef struct {
    char *function;
    char *caller_function;
    char *source;
    int line;
    int column;
} ShellCallFrame;

static ShellCallFrame *gShellCallStack = NULL;
static size_t gShellCallStackCount = 0;
static size_t gShellCallStackCapacity = 0;

static char **gShellSourceStack = NULL;
static size_t gShellSourceDepth = 0;
static size_t gShellSourceCapacity = 0;

typedef struct {
    char *name;
    char *parameter_metadata;
    ShellCompiledFunction *compiled;
} ShellFunctionEntry;

static ShellFunctionEntry *gShellFunctions = NULL;
static size_t gShellFunctionCount = 0;

static int shellParseEnvBool(const char *value) {
    if (!value) {
        return -1;
    }
    while (*value && isspace((unsigned char)*value)) {
        value++;
    }
    if (*value == '\0') {
        return -1;
    }
    if (strcasecmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0) {
        return 1;
    }
    if (strcasecmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0) {
        return 0;
    }
    return -1;
}

static void shellRuntimeFreeCallFrame(ShellCallFrame *frame) {
    if (!frame) {
        return;
    }
    free(frame->function);
    free(frame->caller_function);
    free(frame->source);
    frame->function = NULL;
    frame->caller_function = NULL;
    frame->source = NULL;
    frame->line = 0;
    frame->column = 0;
}

static bool shellRuntimeEnsureCallStackCapacity(size_t needed) {
    if (gShellCallStackCapacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellCallStackCapacity ? (gShellCallStackCapacity * 2) : 4;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }
    ShellCallFrame *resized = (ShellCallFrame *)realloc(gShellCallStack,
                                                        new_capacity * sizeof(ShellCallFrame));
    if (!resized) {
        return false;
    }
    for (size_t i = gShellCallStackCapacity; i < new_capacity; ++i) {
        resized[i].function = NULL;
        resized[i].caller_function = NULL;
        resized[i].source = NULL;
        resized[i].line = 0;
        resized[i].column = 0;
    }
    gShellCallStack = resized;
    gShellCallStackCapacity = new_capacity;
    return true;
}

static bool shellRuntimeEnsureSourceCapacity(size_t needed) {
    if (gShellSourceCapacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellSourceCapacity ? (gShellSourceCapacity * 2) : 4;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }
    char **resized = (char **)realloc(gShellSourceStack, new_capacity * sizeof(char *));
    if (!resized) {
        return false;
    }
    for (size_t i = gShellSourceCapacity; i < new_capacity; ++i) {
        resized[i] = NULL;
    }
    gShellSourceStack = resized;
    gShellSourceCapacity = new_capacity;
    return true;
}

static bool shellRuntimePushSourcePath(const char *path) {
    const char *label = (path && *path) ? path : "";
    if (!shellRuntimeEnsureSourceCapacity(gShellSourceDepth + 1)) {
        return false;
    }
    char *copy = strdup(label);
    if (!copy) {
        return false;
    }
    gShellSourceStack[gShellSourceDepth++] = copy;
    return true;
}

static void shellRuntimePopSourcePath(void) {
    if (gShellSourceDepth == 0) {
        return;
    }
    size_t index = gShellSourceDepth - 1;
    free(gShellSourceStack[index]);
    gShellSourceStack[index] = NULL;
    gShellSourceDepth--;
    if (gShellSourceDepth == 0 && gShellSourceStack) {
        free(gShellSourceStack);
        gShellSourceStack = NULL;
        gShellSourceCapacity = 0;
    }
}

static const char *shellRuntimeCurrentSourcePath(void) {
    if (gShellSourceDepth == 0) {
        return NULL;
    }
    return gShellSourceStack[gShellSourceDepth - 1];
}

bool shellRuntimeTrackSourcePush(const char *path) {
    return shellRuntimePushSourcePath(path);
}

void shellRuntimeTrackSourcePop(void) {
    shellRuntimePopSourcePath();
}

static bool shellRuntimePushCallFrame(const char *function_name,
                                      int call_line,
                                      int call_column) {
    if (!shellRuntimeEnsureCallStackCapacity(gShellCallStackCount + 1)) {
        return false;
    }
    ShellCallFrame *frame = &gShellCallStack[gShellCallStackCount];
    frame->function = NULL;
    frame->caller_function = NULL;
    frame->source = NULL;
    frame->line = (call_line >= 0) ? call_line : 0;
    frame->column = (call_column >= 0) ? call_column : 0;

    if (function_name && *function_name) {
        frame->function = strdup(function_name);
        if (!frame->function) {
            return false;
        }
    }

    const ShellCallFrame *previous =
        (gShellCallStackCount > 0) ? &gShellCallStack[gShellCallStackCount - 1] : NULL;
    const char *caller_name = NULL;
    if (previous && previous->function && *previous->function) {
        caller_name = previous->function;
    } else if (!previous) {
        caller_name = "main";
    }
    if (caller_name) {
        frame->caller_function = strdup(caller_name);
        if (!frame->caller_function) {
            free(frame->function);
            frame->function = NULL;
            return false;
        }
    }

    const char *source = shellRuntimeCurrentSourcePath();
    if (source && *source) {
        frame->source = strdup(source);
        if (!frame->source) {
            free(frame->function);
            free(frame->caller_function);
            frame->function = NULL;
            frame->caller_function = NULL;
            return false;
        }
    }

    gShellCallStackCount++;
    return true;
}

static void shellRuntimePopCallFrame(void) {
    if (gShellCallStackCount == 0) {
        return;
    }
    ShellCallFrame *frame = &gShellCallStack[gShellCallStackCount - 1];
    shellRuntimeFreeCallFrame(frame);
    gShellCallStackCount--;
    if (gShellCallStackCount == 0 && gShellCallStack) {
        free(gShellCallStack);
        gShellCallStack = NULL;
        gShellCallStackCapacity = 0;
    }
}

static size_t shellRuntimeCallStackDepth(void) {
    return gShellCallStackCount;
}

static const ShellCallFrame *shellRuntimeCallFrameAtDepth(size_t depth) {
    if (depth >= gShellCallStackCount) {
        return NULL;
    }
    return &gShellCallStack[gShellCallStackCount - 1 - depth];
}

static const char *shellResolveBashPath(void) {
    static char resolved[PATH_MAX];
    static bool initialised = false;
    if (initialised) {
        return resolved[0] ? resolved : "/bin/bash";
    }
    const char *env_bash = shellSafeGetenv("BASH");
    const char *candidates[] = {
        env_bash,
        "/opt/homebrew/bin/bash",
        "/usr/local/bin/bash",
        "/bin/bash",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        const char *candidate = candidates[i];
        if (!candidate || *candidate == '\0') {
            continue;
        }
        if (access(candidate, X_OK) == 0) {
            strncpy(resolved, candidate, sizeof(resolved) - 1);
            resolved[sizeof(resolved) - 1] = '\0';
            setenv("BASH", resolved, 1);
            break;
        }
    }
    if (resolved[0] == '\0') {
        strncpy(resolved, "/bin/bash", sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }
    initialised = true;
    return resolved;
}

static bool shellAssociativeArraysSupported(void) {
    if (gShellAssociativeArraySupport != -1) {
        return gShellAssociativeArraySupport == 1;
    }

    int override_value = shellParseEnvBool(shellSafeGetenv("PSCALSHELL_ASSOC_ARRAYS"));
    if (override_value == 0 || override_value == 1) {
        gShellAssociativeArraySupport = override_value ? 1 : 0;
        return gShellAssociativeArraySupport == 1;
    }

    const char *bash_path = shellResolveBashPath();
    pid_t pid = fork();
    if (pid < 0) {
        gShellAssociativeArraySupport = 0;
        return false;
    }
    if (pid == 0) {
        execl(bash_path,
              bash_path,
              "--noprofile",
              "--norc",
              "-c",
              "declare -A __exsh_assoc_probe=([__exsh_key__]=value) >/dev/null 2>/dev/null",
              (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        gShellAssociativeArraySupport = 0;
        return false;
    }
    bool supported = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    gShellAssociativeArraySupport = supported ? 1 : 0;
    return supported;
}

static bool shellBindRequiresInteractive(void) {
    if (gShellBindInteractiveStatus != -1) {
        return gShellBindInteractiveStatus == 1;
    }

    int override_value = shellParseEnvBool(shellSafeGetenv("PSCALSHELL_BIND_INTERACTIVE"));
    if (override_value == 0 || override_value == 1) {
        gShellBindInteractiveStatus = override_value ? 1 : 0;
        return gShellBindInteractiveStatus == 1;
    }

    bool requires = false;
    const char *bash_path = shellResolveBashPath();
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        gShellBindInteractiveStatus = 0;
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        gShellBindInteractiveStatus = 0;
        return false;
    }
    if (pid == 0) {
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        execl(bash_path,
              bash_path,
              "--noprofile",
              "--norc",
              "-c",
              "bind 'set show-all-if-ambiguous on'",
              (char *)NULL);
        _exit(127);
    }
    close(pipe_fds[1]);
    char buffer[256];
    while (true) {
        ssize_t n = read(pipe_fds[0], buffer, sizeof(buffer));
        if (n > 0) {
            requires = true;
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
    close(pipe_fds[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) >= 0) {
        if (!requires) {
            requires = !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        }
    }
    gShellBindInteractiveStatus = requires ? 1 : 0;
    return requires;
}

static void shellArrayVariableClear(ShellArrayVariable *var);
static bool shellArrayRegistryEnsureCapacity(size_t needed);
static ShellArrayVariable *shellArrayRegistryFindMutable(const char *name);
static const ShellArrayVariable *shellArrayRegistryFindConst(const char *name);
static bool shellArrayRegistryStore(const char *name,
                                   char **items,
                                   char **keys,
                                   size_t count,
                                   ShellArrayKind kind);
static void shellArrayRegistryRemove(const char *name);
static const ShellArrayVariable *shellArrayRegistryLookup(const char *name, size_t len);
static void shellArrayRegistryAssignFromText(const char *name, const char *value);
static bool shellSetTrackedVariable(const char *name, const char *value, bool is_array_literal);
static void shellUnsetTrackedVariable(const char *name);
static char *shellLookupRawEnvironmentValue(const char *name, size_t len);
static char *shellLookupParameterValue(const char *name, size_t len);
static bool shellAssignmentIsArrayLiteral(const char *raw_assignment, uint8_t word_flags);
static bool shellArrayRegistrySetElement(const char *name, const char *subscript, const char *value);
static bool shellExtractArrayNameAndSubscript(const char *text,
                                             char **out_name,
                                             char **out_subscript);
static bool shellParseArrayLiteral(const char *value,
                                   char ***out_items,
                                   char ***out_keys,
                                   size_t *out_count,
                                   ShellArrayKind *out_kind);
static char *shellBuildArrayLiteral(const ShellArrayVariable *var);
static bool shellArrayRegistryInitializeAssociative(const char *name);
static ShellAlias *shellFindAlias(const char *name);
static void shellReportRecoverableError(VM *vm, bool with_location, const char *fmt, ...);

static bool shellReadonlyEnsureCapacity(size_t needed);
static ShellReadonlyEntry *shellReadonlyFindMutable(const char *name);
static bool shellReadonlyAdd(const char *name);
static bool shellReadonlyContains(const char *name);
static void shellReadonlySetErrorName(const char *name);
static const char *shellReadonlyGetErrorName(void);
static void shellReadonlyPrintVariables(void);

static bool shellIsValidEnvName(const char *name);
static void shellExportPrintEnvironment(void);
static bool shellParseReturnStatus(const char *text, int *out_status);
static bool shellArithmeticParseValueString(const char *text, long long *out_value);
static char *shellEvaluateArithmetic(const char *expr, bool *out_error);
static void shellMarkArithmeticError(void);

static bool gShellPositionalOwned = false;

typedef struct {
    const char *name;
    bool enabled;
} ShellOptionEntry;

static ShellOptionEntry gShellOptions[] = {
    {"assoc_expand_once", false},
    {"autocd", false},
    {"cdable_vars", false},
    {"cdspell", false},
    {"checkhash", false},
    {"checkjobs", false},
    {"checkwinsize", false},
    {"cmdhist", true},
    {"compat31", false},
    {"compat32", false},
    {"compat40", false},
    {"compat41", false},
    {"compat42", false},
    {"compat43", false},
    {"complete_fullquote", false},
    {"direxpand", false},
    {"dirspell", false},
    {"dotglob", false},
    {"execfail", false},
    {"expand_aliases", true},
    {"extdebug", false},
    {"extglob", false},
    {"extquote", true},
    {"failglob", false},
    {"force_fignore", true},
    {"globasciiranges", false},
    {"globskipdots", false},
    {"globstar", false},
    {"gnu_errfmt", false},
    {"histappend", false},
    {"histreedit", false},
    {"histverify", false},
    {"hostcomplete", true},
    {"huponexit", false},
    {"inherit_errexit", false},
    {"interactive_comments", true},
    {"lastpipe", false},
    {"lithist", false},
    {"localvar_inherit", false},
    {"localvar_unset", false},
    {"login_shell", false},
    {"mailwarn", false},
    {"no_empty_cmd_completion", false},
    {"nocaseglob", false},
    {"nocasematch", false},
    {"nullglob", false},
    {"progcomp", true},
    {"promptvars", true},
    {"restricted_shell", false},
    {"shift_verbose", false},
    {"sourcepath", true},
    {"xpg_echo", false}
};

static const size_t gShellOptionCount = sizeof(gShellOptions) / sizeof(gShellOptions[0]);

static bool shellShoptOptionEnabled(const char *name);

typedef struct {
    char *name;
    char *value;
} ShellBindOption;

static ShellBindOption *gShellBindOptions = NULL;
static size_t gShellBindOptionCount = 0;
static int gShellCurrentCommandLine = 0;
static int gShellCurrentCommandColumn = 0;
static const char *gShellCurrentCommandName = NULL;

static void shellRuntimeSetCurrentCommandLocation(int line, int column) {
    gShellCurrentCommandLine = line;
    gShellCurrentCommandColumn = column;
}

static void shellRuntimeSetCurrentCommandName(const char *name) {
    gShellCurrentCommandName = name;
}

static const char *shellRuntimeCurrentCommandName(void) {
    return gShellCurrentCommandName;
}

static void shellFreeParameterArray(char **values, int count) {
    if (!values) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        free(values[i]);
    }
    free(values);
}

static void shellFreeOwnedPositionalParameters(void) {
    if (!gShellPositionalOwned) {
        return;
    }
    if (gParamValues && gParamCount > 0) {
        shellFreeParameterArray(gParamValues, gParamCount);
    } else if (gParamValues) {
        free(gParamValues);
    }
    gParamValues = NULL;
    gParamCount = 0;
    gShellPositionalOwned = false;
}

static char *shellRemovePatternPrefix(const char *value, const char *pattern, bool longest) {
    if (!value) {
        return strdup("");
    }
    if (!pattern) {
        return strdup(value);
    }
    size_t value_len = strlen(value);
    size_t pattern_len = strlen(pattern);
    if (pattern_len == 0) {
        return strdup(value);
    }
    size_t match_len = SIZE_MAX;
    if (longest) {
        for (size_t len = value_len; ; --len) {
            char *prefix = (char *)malloc(len + 1);
            if (!prefix) {
                return NULL;
            }
            if (len > 0) {
                memcpy(prefix, value, len);
            }
            prefix[len] = '\0';
            int rc = fnmatch(pattern, prefix, 0);
            free(prefix);
            if (rc == 0) {
                match_len = len;
                break;
            }
            if (len == 0) {
                break;
            }
        }
    } else {
        for (size_t len = 0; len <= value_len; ++len) {
            char *prefix = (char *)malloc(len + 1);
            if (!prefix) {
                return NULL;
            }
            if (len > 0) {
                memcpy(prefix, value, len);
            }
            prefix[len] = '\0';
            int rc = fnmatch(pattern, prefix, 0);
            free(prefix);
            if (rc == 0) {
                match_len = len;
                break;
            }
        }
    }
    if (match_len == SIZE_MAX) {
        return strdup(value);
    }
    if (match_len >= value_len) {
        return strdup("");
    }
    const char *remainder = value + match_len;
    return strdup(remainder);
}

static void shellBufferAppendChar(char **buffer, size_t *length, size_t *capacity, char c);
static void shellBufferAppendString(char **buffer, size_t *length, size_t *capacity, const char *str);
static char *shellExpandParameter(const char *input,
                                  size_t *out_consumed,
                                  bool *out_is_array_expansion,
                                  size_t *out_array_count);

static char *shellRemovePatternSuffix(const char *value, const char *pattern, bool longest) {
    if (!value) {
        return strdup("");
    }
    if (!pattern) {
        return strdup(value);
    }
    size_t value_len = strlen(value);
    size_t pattern_len = strlen(pattern);
    if (pattern_len == 0) {
        return strdup(value);
    }
    size_t match_len = SIZE_MAX;
    if (longest) {
        for (size_t len = value_len; ; --len) {
            size_t offset = value_len - len;
            const char *suffix = value + offset;
            if (fnmatch(pattern, suffix, 0) == 0) {
                match_len = len;
                break;
            }
            if (len == 0) {
                break;
            }
        }
    } else {
        for (size_t len = 0; len <= value_len; ++len) {
            size_t offset = value_len - len;
            const char *suffix = value + offset;
            if (fnmatch(pattern, suffix, 0) == 0) {
                match_len = len;
                break;
            }
        }
    }
    if (match_len == SIZE_MAX) {
        return strdup(value);
    }
    if (match_len >= value_len) {
        return strdup("");
    }
    size_t keep_len = value_len - match_len;
    char *result = (char *)malloc(keep_len + 1);
    if (!result) {
        return NULL;
    }
    if (keep_len > 0) {
        memcpy(result, value, keep_len);
    }
    result[keep_len] = '\0';
    return result;
}

static char *shellExpandPatternText(const char *pattern, size_t len) {
    char *buffer = NULL;
    size_t length = 0;
    size_t capacity = 0;
    bool in_single = false;
    bool in_double = false;
    size_t i = 0;
    while (i < len) {
        char c = pattern[i];
        if (!in_double && c == '\'') {
            in_single = !in_single;
            i++;
            continue;
        }
        if (!in_single && c == '"') {
            in_double = !in_double;
            i++;
            continue;
        }
        if (c == '\\' && in_double && i + 1 < len) {
            char next = pattern[i + 1];
            if (next == '\\' || next == '"' || next == '$' || next == '`') {
                if (in_single || in_double) {
                    if (next == '*' || next == '?' || next == '[' || next == ']') {
                        shellBufferAppendChar(&buffer, &length, &capacity, '\\');
                    }
                }
                shellBufferAppendChar(&buffer, &length, &capacity, next);
                i += 2;
                continue;
            }
        }
        if (!in_single) {
            if (c == '$') {
                size_t consumed = 0;
                char *expanded = shellExpandParameter(pattern + i + 1,
                                                       &consumed,
                                                       NULL,
                                                       NULL);
                if (expanded) {
                    shellBufferAppendString(&buffer, &length, &capacity, expanded);
                    free(expanded);
                    i += consumed + 1;
                    continue;
                }
            }
        }
        if (in_single || in_double) {
            if (c == '*' || c == '?' || c == '[' || c == ']') {
                shellBufferAppendChar(&buffer, &length, &capacity, '\\');
            }
        }
        shellBufferAppendChar(&buffer, &length, &capacity, c);
        i++;
    }
    if (!buffer) {
        return strdup("");
    }
    return buffer;
}

typedef enum {
    SHELL_RUNTIME_REDIR_OPEN,
    SHELL_RUNTIME_REDIR_DUP,
    SHELL_RUNTIME_REDIR_HEREDOC,
    SHELL_RUNTIME_REDIR_HERE_STRING
} ShellRuntimeRedirectionKind;

typedef struct {
    int fd;
    ShellRuntimeRedirectionKind kind;
    int flags;
    mode_t mode;
    char *path;
    int dup_target_fd;
    bool close_target;
    char *here_doc;
    size_t here_doc_length;
    bool here_doc_quoted;
    char *here_string_literal;
    char *here_string;
    size_t here_string_length;
} ShellRedirection;

typedef struct {
    char *text;
    bool is_array_literal;
} ShellAssignmentEntry;

typedef struct {
    char **argv;
    size_t argc;
    ShellAssignmentEntry *assignments;
    size_t assignment_count;
    ShellRedirection *redirs;
    size_t redir_count;
    bool background;
    bool disable_field_splitting;
    bool ignore_functions;
    int pipeline_index;
    bool is_pipeline_head;
    bool is_pipeline_tail;
    int line;
    int column;
} ShellCommand;

typedef enum {
    SHELL_LOOP_KIND_WHILE,
    SHELL_LOOP_KIND_UNTIL,
    SHELL_LOOP_KIND_FOR,
    SHELL_LOOP_KIND_CFOR
} ShellLoopKind;

typedef enum {
    SHELL_LOOP_CONDITION_NONE = 0,
    SHELL_LOOP_CONDITION_TEST = 1,
    SHELL_LOOP_CONDITION_BRACKET = 2,
    SHELL_LOOP_CONDITION_COLON = 3,
    SHELL_LOOP_CONDITION_TRUE = 4,
    SHELL_LOOP_CONDITION_FALSE = 5,
    SHELL_LOOP_CONDITION_ARITH = 6
} ShellLoopConditionKind;

typedef enum {
    SHELL_LOOP_BODY_KIND_NONE = 0,
    SHELL_LOOP_BODY_KIND_COLON,
    SHELL_LOOP_BODY_KIND_TRUE,
    SHELL_LOOP_BODY_KIND_FALSE,
    SHELL_LOOP_BODY_KIND_TEST,
    SHELL_LOOP_BODY_KIND_BRACKET,
    SHELL_LOOP_BODY_KIND_ARITH,
    SHELL_LOOP_BODY_KIND_TEST_ARITH,
    SHELL_LOOP_BODY_KIND_BRACKET_ARITH
} ShellLoopBodyKind;

typedef struct {
    char *name;
    char *previous_value;
    bool had_previous;
    bool previous_was_array;
} ShellAssignmentBackup;

typedef struct {
    char *name;
    char *value;
    bool is_array_literal;
} ShellLocalVariable;

typedef struct ShellLocalScopeFrame {
    ShellLocalVariable *variables;
    size_t count;
    size_t capacity;
} ShellLocalScopeFrame;

static void shellLocalVariableDispose(ShellLocalVariable *var) {
    if (!var) {
        return;
    }
    free(var->name);
    free(var->value);
    var->name = NULL;
    var->value = NULL;
    var->is_array_literal = false;
}

static void shellLocalScopeFrameReset(ShellLocalScopeFrame *frame) {
    if (!frame) {
        return;
    }
    for (size_t i = 0; i < frame->count; ++i) {
        shellLocalVariableDispose(&frame->variables[i]);
    }
    free(frame->variables);
    frame->variables = NULL;
    frame->count = 0;
    frame->capacity = 0;
}

static bool shellLocalScopeEnsureCapacity(ShellLocalScopeFrame *frame, size_t needed) {
    if (!frame) {
        return false;
    }
    if (frame->capacity >= needed) {
        return true;
    }
    size_t new_capacity = frame->capacity ? frame->capacity * 2 : 4;
    if (new_capacity < needed) {
        new_capacity = needed;
    }
    if (new_capacity > SIZE_MAX / sizeof(ShellLocalVariable)) {
        return false;
    }
    ShellLocalVariable *resized =
        (ShellLocalVariable *)realloc(frame->variables, new_capacity * sizeof(ShellLocalVariable));
    if (!resized) {
        return false;
    }
    for (size_t i = frame->capacity; i < new_capacity; ++i) {
        resized[i].name = NULL;
        resized[i].value = NULL;
        resized[i].is_array_literal = false;
    }
    frame->variables = resized;
    frame->capacity = new_capacity;
    return true;
}

static ShellLocalVariable *shellLocalScopeFrameFind(ShellLocalScopeFrame *frame, const char *name) {
    if (!frame || !name) {
        return NULL;
    }
    for (size_t i = 0; i < frame->count; ++i) {
        ShellLocalVariable *var = &frame->variables[i];
        if (var->name && strcmp(var->name, name) == 0) {
            return var;
        }
    }
    return NULL;
}

static ShellLocalVariable *shellLocalScopeFind(const char *name) {
    if (!name) {
        return NULL;
    }
    for (size_t depth = gShellLocalScopeDepth; depth > 0; --depth) {
        ShellLocalScopeFrame *frame = &gShellLocalScopeStack[depth - 1];
        ShellLocalVariable *found = shellLocalScopeFrameFind(frame, name);
        if (found) {
            return found;
        }
    }
    return NULL;
}

static ShellLocalVariable *shellLocalScopeFindInOtherFrames(const char *name,
                                                           const ShellLocalScopeFrame *exclude) {
    if (!name) {
        return NULL;
    }
    for (size_t depth = gShellLocalScopeDepth; depth > 0; --depth) {
        ShellLocalScopeFrame *frame = &gShellLocalScopeStack[depth - 1];
        if (frame == exclude) {
            continue;
        }
        ShellLocalVariable *found = shellLocalScopeFrameFind(frame, name);
        if (found) {
            return found;
        }
    }
    return NULL;
}

static void shellArrayRegistryRestoreVisibleValue(const char *name,
                                                  const ShellLocalScopeFrame *excluded_frame) {
    if (!name) {
        return;
    }
    ShellLocalVariable *shadow = excluded_frame ? shellLocalScopeFindInOtherFrames(name, excluded_frame)
                                                : shellLocalScopeFind(name);
    if (shadow && shadow->is_array_literal) {
        const char *text = shadow->value ? shadow->value : "";
        shellArrayRegistryAssignFromText(name, text);
        return;
    }
    const char *env_text = shellSafeGetenv(name);
    if (env_text) {
        shellArrayRegistryAssignFromText(name, env_text);
    } else {
        shellArrayRegistryRemove(name);
    }
}

static bool shellLocalScopePush(void) {
    if (gShellLocalScopeDepth == gShellLocalScopeCapacity) {
        size_t new_capacity = gShellLocalScopeCapacity ? gShellLocalScopeCapacity * 2 : 4;
        if (new_capacity > SIZE_MAX / sizeof(ShellLocalScopeFrame)) {
            return false;
        }
        ShellLocalScopeFrame *resized =
            (ShellLocalScopeFrame *)realloc(gShellLocalScopeStack, new_capacity * sizeof(ShellLocalScopeFrame));
        if (!resized) {
            return false;
        }
        for (size_t i = gShellLocalScopeCapacity; i < new_capacity; ++i) {
            resized[i].variables = NULL;
            resized[i].count = 0;
            resized[i].capacity = 0;
        }
        gShellLocalScopeStack = resized;
        gShellLocalScopeCapacity = new_capacity;
    }
    ShellLocalScopeFrame *frame = &gShellLocalScopeStack[gShellLocalScopeDepth];
    frame->variables = NULL;
    frame->count = 0;
    frame->capacity = 0;
    gShellLocalScopeDepth++;
    return true;
}

static void shellLocalScopePop(void) {
    if (gShellLocalScopeDepth == 0) {
        return;
    }
    ShellLocalScopeFrame *frame = &gShellLocalScopeStack[gShellLocalScopeDepth - 1];
    for (size_t i = 0; i < frame->count; ++i) {
        ShellLocalVariable *var = &frame->variables[i];
        if (var->name && var->is_array_literal) {
            shellArrayRegistryRemove(var->name);
            shellArrayRegistryRestoreVisibleValue(var->name, frame);
        }
    }
    shellLocalScopeFrameReset(frame);
    gShellLocalScopeDepth--;
}

static size_t shellLocalScopeDepth(void) {
    return gShellLocalScopeDepth;
}

static bool shellLocalScopeSetOnCurrentFrame(const char *name, const char *value, bool is_array_literal) {
    if (!name || gShellLocalScopeDepth == 0) {
        return false;
    }
    ShellLocalScopeFrame *frame = &gShellLocalScopeStack[gShellLocalScopeDepth - 1];
    ShellLocalVariable *existing = shellLocalScopeFrameFind(frame, name);
    bool created_new = false;
    if (!existing) {
        if (!shellLocalScopeEnsureCapacity(frame, frame->count + 1)) {
            return false;
        }
        existing = &frame->variables[frame->count++];
        existing->name = strdup(name);
        if (!existing->name) {
            frame->count--;
            return false;
        }
        existing->value = NULL;
        existing->is_array_literal = false;
        created_new = true;
    }
    char *copy = strdup(value ? value : "");
    if (!copy) {
        if (created_new) {
            free(existing->name);
            existing->name = NULL;
            frame->count--;
        }
        return false;
    }
    free(existing->value);
    existing->value = copy;
    existing->is_array_literal = is_array_literal;
    if (is_array_literal) {
        const char *text = existing->value ? existing->value : "";
        shellArrayRegistryAssignFromText(existing->name, text);
    } else {
        shellArrayRegistryRemove(existing->name);
    }
    return true;
}

static bool shellLocalScopeAssignExisting(const char *name, const char *value, bool is_array_literal) {
    ShellLocalVariable *existing = shellLocalScopeFind(name);
    if (!existing) {
        return false;
    }
    char *copy = strdup(value ? value : "");
    if (!copy) {
        return false;
    }
    free(existing->value);
    existing->value = copy;
    existing->is_array_literal = is_array_literal;
    return true;
}

static bool shellLocalScopeGetCopy(const char *name, char **out_value, bool *out_is_array) {
    if (out_value) {
        *out_value = NULL;
    }
    if (out_is_array) {
        *out_is_array = false;
    }
    ShellLocalVariable *existing = shellLocalScopeFind(name);
    if (!existing) {
        return false;
    }
    if (out_value) {
        *out_value = strdup(existing->value ? existing->value : "");
        if (!*out_value) {
            return false;
        }
    }
    if (out_is_array) {
        *out_is_array = existing->is_array_literal;
    }
    return true;
}

static bool shellLocalScopeUnset(const char *name) {
    if (!name) {
        return false;
    }
    for (size_t depth = gShellLocalScopeDepth; depth > 0; --depth) {
        ShellLocalScopeFrame *frame = &gShellLocalScopeStack[depth - 1];
        for (size_t i = 0; i < frame->count; ++i) {
            ShellLocalVariable *var = &frame->variables[i];
            if (var->name && strcmp(var->name, name) == 0) {
                bool was_array = var->is_array_literal;
                if (was_array) {
                    shellArrayRegistryRemove(var->name);
                }
                shellLocalVariableDispose(var);
                if (i + 1 < frame->count) {
                    memmove(&frame->variables[i], &frame->variables[i + 1],
                            (frame->count - i - 1) * sizeof(ShellLocalVariable));
                }
                frame->count--;
                if (frame->count < frame->capacity) {
                    frame->variables[frame->count].name = NULL;
                    frame->variables[frame->count].value = NULL;
                    frame->variables[frame->count].is_array_literal = false;
                }
                if (was_array) {
                    shellArrayRegistryRestoreVisibleValue(name, NULL);
                }
                return true;
            }
        }
    }
    return false;
}

typedef struct {
    int target_fd;
    int saved_fd;
    bool saved_valid;
    bool was_closed;
    bool use_vproc;
} ShellExecRedirBackup;

static void shellRestoreExecRedirections(ShellExecRedirBackup *backups, size_t count);
static void shellFreeExecRedirBackups(ShellExecRedirBackup *backups, size_t count);
static void shellFreeRedirections(ShellCommand *cmd);
static void shellAbortPipeline(void);

typedef struct ShellLoopFrame {
    ShellLoopKind kind;
    bool skip_body;
    bool break_pending;
    bool continue_pending;
    char *for_variable;
    char **for_values;
    size_t for_count;
    size_t for_index;
    bool for_active;
    char *cfor_init;
    char *cfor_condition;
    char *cfor_update;
    bool cfor_condition_cached;
    bool cfor_condition_value;
    ShellLoopConditionKind cond_kind;
    char **cond_words;
    size_t cond_word_count;
    char *cond_arith_expression;
    ShellLoopBodyKind body_kind;
    char **body_words;
    size_t body_word_count;
    char *body_arith_expression;
    bool redirs_active;
    ShellRedirection *applied_redirs;
    size_t applied_redir_count;
    ShellExecRedirBackup *redir_backups;
    size_t redir_backup_count;
    size_t redir_backup_capacity;
    bool pipeline_subshell;
    bool pipeline_scope_pushed;
    bool pipeline_prev_local_scope_flag;
    bool pipeline_prev_pipeline_flag;
} ShellLoopFrame;

#define gShellLoopStack (SC()->loop_stack)
#define gShellLoopStackSize (SC()->loop_stack_size)
#define gShellLoopStackCapacity (SC()->loop_stack_capacity)

static ShellRuntimeState gShellRuntimeDefault = {
    .last_status = 0,
    .pipeline = {0},
    .shell_pgid = 0,
    .last_bg_pid = 0,
    .tty_fd = -1,
    .job_control_enabled = false,
    .job_control_initialized = false,
    .errexit_enabled = false,
    .errexit_pending = false,
    .errexit_defer_active = false,
    .trap_enabled = false,
    .posix_mode = false,
    .local_scope_active = false,
    .break_requested = false,
    .continue_requested = false,
    .pipeline_subshell_active = false,
    .break_requested_levels = 0,
    .continue_requested_levels = 0,
    .dir_stack = NULL,
    .dir_stack_count = 0,
    .dir_stack_capacity = 0,
    .dir_stack_initialised = false,
    .hash_entries = NULL,
    .hash_count = 0,
    .hash_capacity = 0,
    .disabled_builtins = NULL,
    .disabled_builtin_count = 0,
    .disabled_builtin_capacity = 0,
    .getopts_index = 1,
    .getopts_offset = 0,
    .scalar_cache_hits = 0,
    .scalar_cache_misses = 0,
    .scalar_cache_stores = 0,
    .scalar_cache_generation = 0,
    .scalar_capture_peak_depth = 0,
    .history = {NULL, 0, 0},
    .arg0 = NULL,
    .signal_traps = { {SHELL_TRAP_ACTION_DEFAULT, NULL} },
    .exit_trap = {SHELL_TRAP_ACTION_DEFAULT, NULL},
    .trap_executing = false,
    .script_depth = 0,
    .condition_depth = 0,
    .job_next_id = 1,
    .jobs = NULL,
    .job_count = 0,
    .array_vars = NULL,
    .array_var_count = 0,
    .array_var_capacity = 0,
    .associative_array_support = -1,
    .bind_interactive_status = -1,
    .readonly_vars = NULL,
    .readonly_var_count = 0,
    .readonly_var_capacity = 0,
    .readonly_error_name = NULL
};

/* Default to the global runtime state to avoid pathological recursion when
 * shellGetContext() is first called before an explicit activation. */
static __thread ShellRuntimeState *gShellRuntimeCtx = &gShellRuntimeDefault;
static __thread ShellRuntimeState *gShellContextStack[8] = {0};
static __thread size_t gShellContextDepth = 0;
static __thread bool gShellCurrentBuiltinBackground = false;
static __thread bool gShellBackgroundWorkerActive = false;
/* Per-thread to avoid cross-session stdio restoration on iOS. */
static __thread int gShellSavedStdFds[3] = { -1, -1, -1 };

#define gShellRuntime (*shellGetContext())

static unsigned long gShellStatusVersion = 0;
static bool gShellStatusSticky = false;

static bool gShellExitRequested = false;
static volatile sig_atomic_t gShellExitOnSignalFlag = 0;
static bool gShellArithmeticErrorPending = false;
static VM *gShellCurrentVm = NULL;
static volatile sig_atomic_t gShellPendingSignals[NSIG] = {0};
void shellResetJobIds(void) {
    gShellRuntimeDefault.job_next_id = 1;
    if (SC()) {
        SC()->job_next_id = 1;
    }
}

static ShellRuntimeState *shellGetContext(void) {
    static __thread bool resolving = false;
    if (resolving) {
        return &gShellRuntimeDefault;
    }
    resolving = true;
    ShellRuntimeState *ctx = gShellRuntimeCtx ? gShellRuntimeCtx : &gShellRuntimeDefault;
    resolving = false;
    return ctx;
}

static void shellSetContext(ShellRuntimeState *ctx) {
    gShellRuntimeCtx = ctx ? ctx : &gShellRuntimeDefault;
}

static void shellContextPush(ShellRuntimeState *ctx) {
    if (gShellContextDepth < (sizeof(gShellContextStack) / sizeof(gShellContextStack[0]))) {
        gShellContextStack[gShellContextDepth++] = ctx;
    }
}

static ShellRuntimeState *shellContextPop(void) {
    if (gShellContextDepth == 0) {
        return &gShellRuntimeDefault;
    }
    ShellRuntimeState *ctx = gShellContextStack[gShellContextDepth - 1];
    gShellContextDepth--;
    return ctx ? ctx : &gShellRuntimeDefault;
}

ShellRuntimeState *shellRuntimeCurrentContext(void) {
    return shellGetContext();
}

ShellRuntimeState *shellRuntimeActivateContext(ShellRuntimeState *ctx) {
    ShellRuntimeState *previous = shellGetContext();
    shellSetContext(ctx);
    return previous;
}

bool shellRuntimeCurrentBuiltinBackground(void) {
    return gShellCurrentBuiltinBackground;
}
static unsigned int gShellRandomSeed = 0;
static bool gShellRandomSeedInitialized = false;
static bool gShellInteractiveMode = false;

static bool shellRuntimeEnsureDisabledCapacity(size_t needed) {
    if (gShellRuntime.disabled_builtin_capacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellRuntime.disabled_builtin_capacity ?
                              gShellRuntime.disabled_builtin_capacity * 2 :
                              4;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    char **entries = realloc(gShellRuntime.disabled_builtins,
                             new_capacity * sizeof(char *));
    if (!entries) {
        return false;
    }
    gShellRuntime.disabled_builtins = entries;
    gShellRuntime.disabled_builtin_capacity = new_capacity;
    return true;
}

static bool shellRuntimeDisableBuiltin(const char *name) {
    if (!name || !*name) {
        return false;
    }
    const char *canonical = shellBuiltinCanonicalName(name);
    const char *target = canonical ? canonical : name;
    if (!shellIsBuiltinName(target)) {
        return false;
    }
    for (size_t i = 0; i < gShellRuntime.disabled_builtin_count; ++i) {
        char *entry = gShellRuntime.disabled_builtins[i];
        if (entry && strcasecmp(entry, target) == 0) {
            return true;
        }
    }
    if (!shellRuntimeEnsureDisabledCapacity(gShellRuntime.disabled_builtin_count + 1)) {
        return false;
    }
    char *copy = strdup(target);
    if (!copy) {
        return false;
    }
    gShellRuntime.disabled_builtins[gShellRuntime.disabled_builtin_count++] = copy;
    return true;
}

static bool shellRuntimeEnableBuiltin(const char *name) {
    if (!name || !*name) {
        return false;
    }
    const char *canonical = shellBuiltinCanonicalName(name);
    const char *target = canonical ? canonical : name;
    if (!shellIsBuiltinName(target)) {
        return false;
    }
    for (size_t i = 0; i < gShellRuntime.disabled_builtin_count; ++i) {
        char *entry = gShellRuntime.disabled_builtins[i];
        if (entry && strcasecmp(entry, target) == 0) {
            free(entry);
            if (i + 1 < gShellRuntime.disabled_builtin_count) {
                memmove(&gShellRuntime.disabled_builtins[i],
                        &gShellRuntime.disabled_builtins[i + 1],
                        (gShellRuntime.disabled_builtin_count - i - 1) * sizeof(char *));
            }
            gShellRuntime.disabled_builtin_count--;
            return true;
        }
    }
    return true;
}

static bool shellRuntimeBuiltinDisabled(const char *name) {
    if (!name || !*name) {
        return false;
    }
    const char *canonical = shellBuiltinCanonicalName(name);
    const char *target = canonical ? canonical : name;
    for (size_t i = 0; i < gShellRuntime.disabled_builtin_count; ++i) {
        char *entry = gShellRuntime.disabled_builtins[i];
        if (entry && strcasecmp(entry, target) == 0) {
            return true;
        }
    }
    return false;
}

static void shellUpdateStatus(int status);
static void shellSignalHandler(int signo);

static void shellTrapClearEntry(ShellTrapEntry *entry) {
    if (!entry) {
        return;
    }
    if (entry->command) {
        free(entry->command);
        entry->command = NULL;
    }
    entry->action = SHELL_TRAP_ACTION_DEFAULT;
}

static bool shellTrapSetCommand(ShellTrapEntry *entry, const char *command) {
    if (!entry) {
        return false;
    }
    shellTrapClearEntry(entry);
    if (!command) {
        entry->action = SHELL_TRAP_ACTION_COMMAND;
        entry->command = strdup("");
        return entry->command != NULL;
    }
    char *copy = strdup(command);
    if (!copy) {
        return false;
    }
    entry->action = SHELL_TRAP_ACTION_COMMAND;
    entry->command = copy;
    return true;
}

void shellRuntimePushScript(void) {
    gShellScriptDepth++;
}

void shellRuntimePopScript(void) {
    if (gShellScriptDepth > 0) {
        gShellScriptDepth--;
    }
}

bool shellRuntimeIsOutermostScript(void) {
    return gShellScriptDepth == 1;
}

void shellRuntimeEnterCondition(void) {
    gShellConditionDepth++;
}

void shellRuntimeLeaveCondition(void) {
    if (gShellConditionDepth > 0) {
        gShellConditionDepth--;
    }
}

bool shellRuntimeEvaluatingCondition(void) {
    return gShellConditionDepth > 0;
}

void shellRuntimeAbandonConditionEvaluation(void) {
    gShellConditionDepth = 0;
}

void shellRuntimeRequestExit(void) {
    shellRuntimeAbandonConditionEvaluation();
    gShellExitRequested = true;
}

void shellRuntimeSetLastStatus(int status) {
    gShellStatusVersion++;
    if (gShellArithmeticErrorPending) {
        status = 1;
        gShellArithmeticErrorPending = false;
    }
    gShellRuntime.last_status = status;
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", status);
    shellSetTrackedVariable("EXSH_LAST_STATUS", buffer, false);
    bool suppress_errexit = shellRuntimeEvaluatingCondition();
    if (status != 0) {
        if (gShellRuntime.errexit_enabled && !suppress_errexit) {
            gShellRuntime.errexit_pending = true;
            gShellRuntime.errexit_defer_active = true;
        }
    } else {
        gShellRuntime.errexit_pending = false;
        gShellRuntime.errexit_defer_active = false;
    }
    gShellStatusSticky = false;
}

void shellRuntimeSetLastStatusSticky(int status) {
    shellRuntimeSetLastStatus(status);
    gShellStatusSticky = true;
}

void shellRuntimeRefreshTrapEnabled(void) {
    gShellRuntime.trap_enabled = false;
#ifdef SIGINT
    if (gShellSignalTraps[SIGINT].action == SHELL_TRAP_ACTION_COMMAND) {
        gShellRuntime.trap_enabled = true;
        return;
    }
#endif
#ifdef SIGQUIT
    if (gShellSignalTraps[SIGQUIT].action == SHELL_TRAP_ACTION_COMMAND) {
        gShellRuntime.trap_enabled = true;
        return;
    }
#endif
#ifdef SIGTSTP
    if (gShellSignalTraps[SIGTSTP].action == SHELL_TRAP_ACTION_COMMAND) {
        gShellRuntime.trap_enabled = true;
        return;
    }
#endif
}

static void shellRuntimeExecuteTrap(const char *command, const char *label) {
    if (!command || gShellTrapExecuting) {
        return;
    }
    gShellTrapExecuting = true;

    ShellRunOptions opts = (ShellRunOptions){0};
    opts.no_cache = 1;
    opts.quiet = true;
    opts.exit_on_signal = shellRuntimeExitOnSignal();
    opts.suppress_warnings = true;
    const char *frontend = shellRuntimeGetArg0();
    opts.frontend_path = frontend ? frontend : "exsh";

    bool saved_exit_requested = gShellExitRequested;
    bool saved_break_requested = gShellRuntime.break_requested;
    int saved_break_levels = gShellRuntime.break_requested_levels;
    bool saved_continue_requested = gShellRuntime.continue_requested;
    int saved_continue_levels = gShellRuntime.continue_requested_levels;
    bool saved_errexit_pending = gShellRuntime.errexit_pending;
    bool saved_errexit_defer_active = gShellRuntime.errexit_defer_active;
    VM *current_vm = gShellCurrentVm;
    bool saved_vm_exit = false;
    bool saved_vm_abort = false;
    const char *saved_vm_builtin = NULL;
    if (current_vm) {
        saved_vm_exit = current_vm->exit_requested;
        saved_vm_abort = current_vm->abort_requested;
        saved_vm_builtin = current_vm->current_builtin_name;
        current_vm->exit_requested = false;
        current_vm->abort_requested = false;
        current_vm->current_builtin_name = NULL;
    }
    gShellExitRequested = false;
    gShellRuntime.break_requested = false;
    gShellRuntime.break_requested_levels = 0;
    gShellRuntime.continue_requested = false;
    gShellRuntime.continue_requested_levels = 0;
    gShellRuntime.errexit_pending = false;
    gShellRuntime.errexit_defer_active = false;

    bool exit_requested = false;
    int status = EXIT_FAILURE;

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

    if (can_run || shellSymbolTableScopeIsActive()) {
        status = shellRunSource(command, label ? label : "<trap>", &opts, &exit_requested);
    }

    if (scope_pushed) {
        shellSymbolTableScopePop(&table_scope);
    }

    if (!can_run && !shellSymbolTableScopeIsActive()) {
        status = EXIT_FAILURE;
    }

    if (exit_requested) {
        shellRuntimeRequestExit();
    }

    bool trap_exit_requested = gShellExitRequested;
    bool trap_break_requested = gShellRuntime.break_requested;
    int trap_break_levels = gShellRuntime.break_requested_levels;
    bool trap_continue_requested = gShellRuntime.continue_requested;
    int trap_continue_levels = gShellRuntime.continue_requested_levels;
    bool trap_errexit_pending = gShellRuntime.errexit_pending;
    bool trap_errexit_defer_active = gShellRuntime.errexit_defer_active;
    bool trap_vm_exit = current_vm ? current_vm->exit_requested : false;
    bool trap_vm_abort = current_vm ? current_vm->abort_requested : false;
    const char *trap_vm_builtin = current_vm ? current_vm->current_builtin_name : NULL;

    gShellExitRequested = saved_exit_requested || trap_exit_requested;
    if (saved_break_requested || trap_break_requested) {
        gShellRuntime.break_requested = true;
        if (saved_break_requested && trap_break_requested) {
            gShellRuntime.break_requested_levels =
                (saved_break_levels > trap_break_levels) ? saved_break_levels : trap_break_levels;
        } else if (trap_break_requested) {
            gShellRuntime.break_requested_levels = trap_break_levels;
        } else {
            gShellRuntime.break_requested_levels = saved_break_levels;
        }
    } else {
        gShellRuntime.break_requested = false;
        gShellRuntime.break_requested_levels = 0;
    }
    if (saved_continue_requested || trap_continue_requested) {
        gShellRuntime.continue_requested = true;
        if (saved_continue_requested && trap_continue_requested) {
            gShellRuntime.continue_requested_levels =
                (saved_continue_levels > trap_continue_levels) ? saved_continue_levels : trap_continue_levels;
        } else if (trap_continue_requested) {
            gShellRuntime.continue_requested_levels = trap_continue_levels;
        } else {
            gShellRuntime.continue_requested_levels = saved_continue_levels;
        }
    } else {
        gShellRuntime.continue_requested = false;
        gShellRuntime.continue_requested_levels = 0;
    }
    gShellRuntime.errexit_pending = saved_errexit_pending || trap_errexit_pending;
    gShellRuntime.errexit_defer_active = saved_errexit_defer_active || trap_errexit_defer_active;
    if (current_vm) {
        current_vm->exit_requested = saved_vm_exit || trap_vm_exit;
        current_vm->abort_requested = saved_vm_abort || trap_vm_abort;
        if (!current_vm->current_builtin_name) {
            current_vm->current_builtin_name = trap_vm_builtin ? trap_vm_builtin : saved_vm_builtin;
        }
    }

    shellUpdateStatus(status);

    gShellTrapExecuting = false;
}

bool shellRuntimeShouldDeferExit(VM *vm) {
    if (!gShellRuntime.errexit_defer_active) {
        return false;
    }
    gShellRuntime.errexit_defer_active = false;
    if (vm) {
        vm->exit_requested = false;
        vm->abort_requested = false;
    }
    if (gShellCurrentVm) {
        gShellCurrentVm->exit_requested = false;
        gShellCurrentVm->abort_requested = false;
        gShellCurrentVm->current_builtin_name = NULL;
    }
    return true;
}

bool shellRuntimeMaybeRequestPendingExit(VM *vm) {
    if (!gShellRuntime.errexit_pending) {
        return false;
    }
    if (gShellRuntime.errexit_defer_active) {
        if (!shellRuntimeEvaluatingCondition()) {
            gShellRuntime.errexit_defer_active = false;
        }
        return false;
    }
    gShellRuntime.errexit_pending = false;
    shellRuntimeRequestExit();
    if (vm) {
        vm->exit_requested = true;
        vm->abort_requested = true;
    }
    if (gShellCurrentVm) {
        gShellCurrentVm->exit_requested = true;
        gShellCurrentVm->abort_requested = true;
        if (!gShellCurrentVm->current_builtin_name) {
            gShellCurrentVm->current_builtin_name = "errexit";
        }
    }
    return true;
}

const volatile bool *shellRuntimePendingExitFlag(void) {
    return &gShellRuntime.errexit_pending;
}

ShellTrapAction shellRuntimeGetSignalTrapAction(int signo) {
    if (signo <= 0 || signo >= NSIG) {
        return SHELL_TRAP_ACTION_DEFAULT;
    }
    return gShellSignalTraps[signo].action;
}

void shellRuntimeRunSignalTrap(int signo) {
    if (signo <= 0 || signo >= NSIG) {
        return;
    }
    ShellTrapEntry *entry = &gShellSignalTraps[signo];
    if (entry->action != SHELL_TRAP_ACTION_COMMAND || !entry->command) {
        return;
    }
    char label[32];
    snprintf(label, sizeof(label), "<trap-%d>", signo);
    shellRuntimeExecuteTrap(entry->command, label);
}

void shellRuntimeRunExitTrap(void) {
    if (gShellExitTrap.action != SHELL_TRAP_ACTION_COMMAND || !gShellExitTrap.command) {
        return;
    }
    shellRuntimeExecuteTrap(gShellExitTrap.command, "<exit-trap>");
}

bool shellRuntimeSetExitTrap(ShellTrapAction action, const char *command) {
    shellTrapClearEntry(&gShellExitTrap);
    if (action == SHELL_TRAP_ACTION_COMMAND) {
        if (!shellTrapSetCommand(&gShellExitTrap, command)) {
            return false;
        }
    } else if (action == SHELL_TRAP_ACTION_IGNORE) {
        gShellExitTrap.action = SHELL_TRAP_ACTION_IGNORE;
    }
    return true;
}

typedef struct {
    const char *name;
    int signo;
} ShellSignalNameEntry;

static const ShellSignalNameEntry kShellSignalNames[] = {
#ifdef SIGHUP
    {"HUP", SIGHUP},
#endif
#ifdef SIGINT
    {"INT", SIGINT},
#endif
#ifdef SIGQUIT
    {"QUIT", SIGQUIT},
#endif
#ifdef SIGILL
    {"ILL", SIGILL},
#endif
#ifdef SIGTRAP
    {"TRAP", SIGTRAP},
#endif
#ifdef SIGABRT
    {"ABRT", SIGABRT},
#endif
#ifdef SIGIOT
    {"IOT", SIGIOT},
#endif
#ifdef SIGBUS
    {"BUS", SIGBUS},
#endif
#ifdef SIGFPE
    {"FPE", SIGFPE},
#endif
#ifdef SIGKILL
    {"KILL", SIGKILL},
#endif
#ifdef SIGUSR1
    {"USR1", SIGUSR1},
#endif
#ifdef SIGSEGV
    {"SEGV", SIGSEGV},
#endif
#ifdef SIGUSR2
    {"USR2", SIGUSR2},
#endif
#ifdef SIGPIPE
    {"PIPE", SIGPIPE},
#endif
#ifdef SIGALRM
    {"ALRM", SIGALRM},
#endif
#ifdef SIGTERM
    {"TERM", SIGTERM},
#endif
#ifdef SIGSTKFLT
    {"STKFLT", SIGSTKFLT},
#endif
#ifdef SIGCHLD
    {"CHLD", SIGCHLD},
#endif
#ifdef SIGCLD
    {"CLD", SIGCLD},
#endif
#ifdef SIGCONT
    {"CONT", SIGCONT},
#endif
#ifdef SIGSTOP
    {"STOP", SIGSTOP},
#endif
#ifdef SIGTSTP
    {"TSTP", SIGTSTP},
#endif
#ifdef SIGTTIN
    {"TTIN", SIGTTIN},
#endif
#ifdef SIGTTOU
    {"TTOU", SIGTTOU},
#endif
#ifdef SIGURG
    {"URG", SIGURG},
#endif
#ifdef SIGXCPU
    {"XCPU", SIGXCPU},
#endif
#ifdef SIGXFSZ
    {"XFSZ", SIGXFSZ},
#endif
#ifdef SIGVTALRM
    {"VTALRM", SIGVTALRM},
#endif
#ifdef SIGPROF
    {"PROF", SIGPROF},
#endif
#ifdef SIGWINCH
    {"WINCH", SIGWINCH},
#endif
#ifdef SIGIO
    {"IO", SIGIO},
#endif
#ifdef SIGPOLL
    {"POLL", SIGPOLL},
#endif
#ifdef SIGPWR
    {"PWR", SIGPWR},
#endif
#ifdef SIGSYS
    {"SYS", SIGSYS},
#endif
};

static bool shellRuntimeIsDigitString(const char *text) {
    if (!text || !*text) {
        return false;
    }
    const unsigned char *p = (const unsigned char *)text;
    if (*p == '+' || *p == '-') {
        ++p;
    }
    if (!*p) {
        return false;
    }
    while (*p) {
        if (!isdigit(*p)) {
            return false;
        }
        ++p;
    }
    return true;
}

bool shellRuntimeParseSignal(const char *text, int *out_signo) {
    if (!text || !out_signo) {
        return false;
    }
    if (strcasecmp(text, "EXIT") == 0 || strcmp(text, "0") == 0) {
        *out_signo = 0;
        return true;
    }
    if (shellRuntimeIsDigitString(text)) {
        long value = strtol(text, NULL, 10);
        if (value >= 0 && value < NSIG) {
            *out_signo = (int)value;
            return true;
        }
        return false;
    }
    const char *name = text;
    if (strncasecmp(name, "SIG", 3) == 0 && name[3] != '\0') {
        name += 3;
    }
    for (size_t i = 0; i < sizeof(kShellSignalNames) / sizeof(kShellSignalNames[0]); ++i) {
        if (strcasecmp(name, kShellSignalNames[i].name) == 0) {
            *out_signo = kShellSignalNames[i].signo;
            return true;
        }
    }
    return false;
}

bool shellRuntimeSignalName(int signo, bool include_prefix, char *buffer, size_t buffer_len) {
    if (!buffer || buffer_len == 0) {
        return false;
    }
    if (signo == 0) {
        snprintf(buffer, buffer_len, "EXIT");
        return true;
    }
    if (signo < 0 || signo >= NSIG) {
        return false;
    }
    for (size_t i = 0; i < sizeof(kShellSignalNames) / sizeof(kShellSignalNames[0]); ++i) {
        if (kShellSignalNames[i].signo == signo) {
            if (include_prefix) {
                snprintf(buffer, buffer_len, "SIG%s", kShellSignalNames[i].name);
            } else {
                snprintf(buffer, buffer_len, "%s", kShellSignalNames[i].name);
            }
            return true;
        }
    }
#if defined(SIGRTMIN) && defined(SIGRTMAX)
    int rtmin = SIGRTMIN;
    int rtmax = SIGRTMAX;
    if (signo >= rtmin && signo <= rtmax) {
        if (signo == rtmin) {
            snprintf(buffer, buffer_len, include_prefix ? "SIGRTMIN" : "RTMIN");
            return true;
        }
        if (signo == rtmax) {
            snprintf(buffer, buffer_len, include_prefix ? "SIGRTMAX" : "RTMAX");
            return true;
        }
        int offset_from_min = signo - rtmin;
        int offset_from_max = rtmax - signo;
        if (offset_from_min <= offset_from_max) {
            if (include_prefix) {
                snprintf(buffer, buffer_len, "SIGRTMIN+%d", offset_from_min);
            } else {
                snprintf(buffer, buffer_len, "RTMIN+%d", offset_from_min);
            }
        } else {
            if (include_prefix) {
                snprintf(buffer, buffer_len, "SIGRTMAX-%d", offset_from_max);
            } else {
                snprintf(buffer, buffer_len, "RTMAX-%d", offset_from_max);
            }
        }
        return true;
    }
#endif
    return false;
}

bool shellRuntimeSetSignalTrap(int signo, ShellTrapAction action, const char *command) {
    if (signo <= 0 || signo >= NSIG) {
        return false;
    }

    char *command_copy = NULL;
    if (action == SHELL_TRAP_ACTION_COMMAND) {
        const char *text = command ? command : "";
        command_copy = strdup(text);
        if (!command_copy) {
            return false;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    if (action == SHELL_TRAP_ACTION_COMMAND) {
        sa.sa_handler = shellSignalHandler;
        sa.sa_flags |= SA_RESTART;
    } else if (action == SHELL_TRAP_ACTION_IGNORE) {
        sa.sa_handler = SIG_IGN;
    } else {
        if (signo == SIGINT || signo == SIGQUIT || signo == SIGTSTP) {
            sa.sa_handler = shellSignalHandler;
            sa.sa_flags |= SA_RESTART;
        } else {
            sa.sa_handler = SIG_DFL;
        }
    }

    if (sigaction(signo, &sa, NULL) != 0) {
        if (command_copy) {
            free(command_copy);
        }
        return false;
    }

    shellTrapClearEntry(&gShellSignalTraps[signo]);
    if (action == SHELL_TRAP_ACTION_COMMAND) {
        gShellSignalTraps[signo].action = SHELL_TRAP_ACTION_COMMAND;
        gShellSignalTraps[signo].command = command_copy;
    } else {
        gShellSignalTraps[signo].action = action;
        if (command_copy) {
            free(command_copy);
        }
    }

    shellRuntimeRefreshTrapEnabled();
    return true;
}

static void shellRuntimeInitialisePosixMode(void) __attribute__((constructor));

static void shellRuntimeInitialisePosixMode(void) {
    const char *posix_env = shellSafeGetenv("POSIXLY_CORRECT");
    if (posix_env) {
        gShellRuntime.posix_mode = true;
    }
}

void shellRuntimeSetPosixMode(bool enabled) {
    gShellRuntime.posix_mode = enabled;
    if (enabled) {
        const char *current = shellSafeGetenv("POSIXLY_CORRECT");
        if (!current) {
            setenv("POSIXLY_CORRECT", "y", 1);
        }
    } else {
        unsetenv("POSIXLY_CORRECT");
    }
}

bool shellRuntimePosixMode(void) {
    return gShellRuntime.posix_mode;
}

void shellRuntimeSetInteractive(bool interactive) {
    gShellInteractiveMode = interactive;
}

bool shellRuntimeIsInteractive(void) {
    return gShellInteractiveMode;
}

static void shellRuntimeRememberStandardFds(void) {
    if (gShellSavedStdFds[0] < 0) gShellSavedStdFds[0] = dup(STDIN_FILENO);
    if (gShellSavedStdFds[1] < 0) gShellSavedStdFds[1] = dup(STDOUT_FILENO);
    if (gShellSavedStdFds[2] < 0) gShellSavedStdFds[2] = dup(STDERR_FILENO);
    for (int i = 0; i < 3; ++i) {
        if (gShellSavedStdFds[i] >= 0) {
            fcntl(gShellSavedStdFds[i], F_SETFD, FD_CLOEXEC);
        }
    }
}

void shellRuntimeEnsureStandardFds(void) {
    shellRuntimeRememberStandardFds();
    int targets[3] = { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };
    for (int i = 0; i < 3; ++i) {
        if (targets[i] < 0) {
            continue;
        }
        int rc = fcntl(targets[i], F_GETFD);
        if (rc < 0 && errno == EBADF && gShellSavedStdFds[i] >= 0) {
            dup2(gShellSavedStdFds[i], targets[i]);
            shellResetStandardStream(targets[i]);
        }
    }
}

static void shellRandomEnsureSeeded(void) {
    if (gShellRandomSeedInitialized) {
        return;
    }
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    if (seed == 0) {
        seed = 1;
    }
    gShellRandomSeed = seed;
    gShellRandomSeedInitialized = true;
}

static void shellRandomReseed(unsigned int seed) {
    gShellRandomSeed = seed;
    gShellRandomSeedInitialized = true;
}

static void shellRandomAssignFromText(const char *value) {
    if (!value) {
        shellRandomReseed(0u);
        return;
    }
    errno = 0;
    char *endptr = NULL;
    unsigned long parsed = strtoul(value, &endptr, 10);
    if (errno != 0 || endptr == value) {
        parsed = 0;
    }
    shellRandomReseed((unsigned int)parsed);
}

static unsigned int shellRandomNextValue(void) {
    shellRandomEnsureSeeded();
    gShellRandomSeed = gShellRandomSeed * 1103515245u + 12345u;
    return (gShellRandomSeed / 65536u) % 32768u;
}

static bool shellHandleSpecialAssignment(const char *name, const char *value) {
    if (!name) {
        return false;
    }
    if (strcmp(name, "RANDOM") == 0) {
        shellRandomAssignFromText(value);
        return true;
    }
    return false;
}

static void shellArrayVariableClear(ShellArrayVariable *var) {
    if (!var) {
        return;
    }
    if (var->values) {
        for (size_t i = 0; i < var->count; ++i) {
            free(var->values[i]);
        }
        free(var->values);
    }
    if (var->keys) {
        for (size_t i = 0; i < var->count; ++i) {
            free(var->keys[i]);
        }
        free(var->keys);
    }
    var->values = NULL;
    var->keys = NULL;
    var->count = 0;
    var->kind = SHELL_ARRAY_KIND_INDEXED;
}

static bool shellArrayRegistryEnsureCapacity(size_t needed) {
    if (gShellArrayVarCapacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellArrayVarCapacity ? (gShellArrayVarCapacity * 2) : 8;
    if (new_capacity < needed) {
        new_capacity = needed;
    }
    ShellArrayVariable *resized =
        (ShellArrayVariable *)realloc(gShellArrayVars, new_capacity * sizeof(ShellArrayVariable));
    if (!resized) {
        return false;
    }
    for (size_t i = gShellArrayVarCapacity; i < new_capacity; ++i) {
        resized[i].name = NULL;
        resized[i].values = NULL;
        resized[i].keys = NULL;
        resized[i].count = 0;
        resized[i].kind = SHELL_ARRAY_KIND_INDEXED;
    }
    gShellArrayVars = resized;
    gShellArrayVarCapacity = new_capacity;
    return true;
}

static ShellArrayVariable *shellArrayRegistryFindMutable(const char *name) {
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < gShellArrayVarCount; ++i) {
        ShellArrayVariable *var = &gShellArrayVars[i];
        if (var->name && strcmp(var->name, name) == 0) {
            return var;
        }
    }
    return NULL;
}

static const ShellArrayVariable *shellArrayRegistryFindConst(const char *name) {
    return shellArrayRegistryFindMutable(name);
}

static bool shellArrayRegistryStore(const char *name,
                                   char **items,
                                   char **keys,
                                   size_t count,
                                   ShellArrayKind kind) {
    if (!name) {
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
        var->kind = SHELL_ARRAY_KIND_INDEXED;
    } else {
        shellArrayVariableClear(var);
    }

    var->kind = kind;
    if (count == 0) {
        return true;
    }

    var->values = (char **)calloc(count, sizeof(char *));
    if (!var->values) {
        shellArrayRegistryRemove(name);
        return false;
    }
    if (kind == SHELL_ARRAY_KIND_ASSOCIATIVE) {
        var->keys = (char **)calloc(count, sizeof(char *));
        if (!var->keys) {
            shellArrayRegistryRemove(name);
            return false;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        const char *item = items[i] ? items[i] : "";
        var->values[i] = strdup(item);
        if (!var->values[i]) {
            shellArrayRegistryRemove(name);
            return false;
        }
        if (kind == SHELL_ARRAY_KIND_ASSOCIATIVE) {
            const char *key = (keys && keys[i]) ? keys[i] : "";
            var->keys[i] = strdup(key);
            if (!var->keys[i]) {
                shellArrayRegistryRemove(name);
                return false;
            }
        }
    }
    var->count = count;
    return true;
}

static void shellArrayRegistryRemove(const char *name) {
    if (!name) {
        return;
    }
    for (size_t i = 0; i < gShellArrayVarCount; ++i) {
        ShellArrayVariable *var = &gShellArrayVars[i];
        if (!var->name || strcmp(var->name, name) != 0) {
            continue;
        }
        free(var->name);
        var->name = NULL;
        shellArrayVariableClear(var);
        if (i + 1 < gShellArrayVarCount) {
            gShellArrayVars[i] = gShellArrayVars[gShellArrayVarCount - 1];
        }
        gShellArrayVarCount--;
        return;
    }
}

static const ShellArrayVariable *shellArrayRegistryLookup(const char *name, size_t len) {
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < gShellArrayVarCount; ++i) {
        ShellArrayVariable *var = &gShellArrayVars[i];
        if (!var->name) {
            continue;
        }
        size_t stored_len = strlen(var->name);
        if (stored_len == len && strncmp(var->name, name, len) == 0) {
            return var;
        }
    }
    return NULL;
}

static bool shellAssignmentIsArrayLiteral(const char *raw_assignment, uint8_t word_flags) {
    if (!raw_assignment) {
        return false;
    }
    const char *eq = strchr(raw_assignment, '=');
    if (!eq) {
        return false;
    }

    bool base_single = (word_flags & SHELL_WORD_FLAG_SINGLE_QUOTED) != 0;
    bool base_double = (word_flags & SHELL_WORD_FLAG_DOUBLE_QUOTED) != 0;
    bool saw_single_marker = false;
    bool saw_double_marker = false;
    bool in_single_segment = false;
    bool in_double_segment = false;

    for (const char *cursor = raw_assignment; cursor < eq && *cursor; ++cursor) {
        if (*cursor == SHELL_QUOTE_MARK_SINGLE) {
            saw_single_marker = true;
            in_single_segment = !in_single_segment;
        } else if (*cursor == SHELL_QUOTE_MARK_DOUBLE) {
            saw_double_marker = true;
            in_double_segment = !in_double_segment;
        }
    }

    const char *value_start = eq + 1;
    const char *first_char = NULL;
    bool first_quoted = false;
    const char *last_char = NULL;
    bool last_quoted = false;

    for (const char *cursor = value_start; *cursor; ++cursor) {
        char ch = *cursor;
        if (ch == SHELL_QUOTE_MARK_SINGLE) {
            saw_single_marker = true;
            in_single_segment = !in_single_segment;
            continue;
        }
        if (ch == SHELL_QUOTE_MARK_DOUBLE) {
            saw_double_marker = true;
            in_double_segment = !in_double_segment;
            continue;
        }

        bool effective_single = in_single_segment || (!saw_single_marker && base_single);
        bool effective_double = in_double_segment || (!saw_double_marker && base_double);
        bool quoted = effective_single || effective_double;

        if (!first_char) {
            if (!quoted && isspace((unsigned char)ch)) {
                continue;
            }
            first_char = cursor;
            first_quoted = quoted;
        }

        if (!quoted && isspace((unsigned char)ch)) {
            continue;
        }

        last_char = cursor;
        last_quoted = quoted;
    }

    if (!first_char || !last_char) {
        return false;
    }
    if (first_quoted || last_quoted) {
        return false;
    }
    return *first_char == '(' && *last_char == ')';
}

static bool shellLoopEnsureCapacity(size_t needed) {
    if (gShellLoopStackCapacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellLoopStackCapacity ? gShellLoopStackCapacity * 2 : 4;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }
    ShellLoopFrame *resized = (ShellLoopFrame *)realloc(gShellLoopStack, new_capacity * sizeof(ShellLoopFrame));
    if (!resized) {
        return false;
    }
    gShellLoopStack = resized;
    gShellLoopStackCapacity = new_capacity;
    return true;
}

static void shellLoopFrameFreeData(ShellLoopFrame *frame) {
    if (!frame) {
        return;
    }
    if (frame->pipeline_scope_pushed) {
        shellLocalScopePop();
        frame->pipeline_scope_pushed = false;
    }
    if (frame->pipeline_subshell) {
        gShellRuntime.pipeline_subshell_active = frame->pipeline_prev_pipeline_flag;
        gShellRuntime.local_scope_active = frame->pipeline_prev_local_scope_flag;
        frame->pipeline_subshell = false;
    }
    if (frame->for_variable) {
        free(frame->for_variable);
        frame->for_variable = NULL;
    }
    if (frame->for_values) {
        for (size_t i = 0; i < frame->for_count; ++i) {
            free(frame->for_values[i]);
        }
        free(frame->for_values);
        frame->for_values = NULL;
    }
    frame->for_count = 0;
    frame->for_index = 0;
    frame->for_active = false;
    if (frame->cfor_init) {
        free(frame->cfor_init);
        frame->cfor_init = NULL;
    }
    if (frame->cfor_condition) {
        free(frame->cfor_condition);
        frame->cfor_condition = NULL;
    }
    if (frame->cfor_update) {
        free(frame->cfor_update);
        frame->cfor_update = NULL;
    }
    frame->cfor_condition_cached = false;
    frame->cfor_condition_value = false;
    frame->cond_kind = SHELL_LOOP_CONDITION_NONE;
    if (frame->cond_words) {
        for (size_t i = 0; i < frame->cond_word_count; ++i) {
            free(frame->cond_words[i]);
        }
        free(frame->cond_words);
        frame->cond_words = NULL;
    }
    frame->cond_word_count = 0;
    if (frame->cond_arith_expression) {
        free(frame->cond_arith_expression);
        frame->cond_arith_expression = NULL;
    }
    frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
    if (frame->body_words) {
        for (size_t i = 0; i < frame->body_word_count; ++i) {
            free(frame->body_words[i]);
        }
        free(frame->body_words);
        frame->body_words = NULL;
    }
    frame->body_word_count = 0;
    if (frame->body_arith_expression) {
        free(frame->body_arith_expression);
        frame->body_arith_expression = NULL;
    }
    if (frame->cond_arith_expression) {
        free(frame->cond_arith_expression);
        frame->cond_arith_expression = NULL;
    }
    if (frame->redirs_active) {
        shellRestoreExecRedirections(frame->redir_backups, frame->redir_backup_count);
    }
    if (frame->redir_backups) {
        shellFreeExecRedirBackups(frame->redir_backups, frame->redir_backup_count);
        frame->redir_backups = NULL;
        frame->redir_backup_count = 0;
        frame->redir_backup_capacity = 0;
    }
    if (frame->applied_redirs) {
        ShellCommand temp;
        memset(&temp, 0, sizeof(temp));
        temp.redirs = frame->applied_redirs;
        temp.redir_count = frame->applied_redir_count;
        shellFreeRedirections(&temp);
        frame->applied_redirs = NULL;
        frame->applied_redir_count = 0;
    }
    frame->redirs_active = false;
}

ShellRuntimeState *shellRuntimeCreateContext(void) {
    ShellRuntimeState *ctx = (ShellRuntimeState *)calloc(1, sizeof(ShellRuntimeState));
    if (!ctx) {
        return NULL;
    }
    *ctx = gShellRuntimeDefault;
    ctx->pipeline = (ShellPipelineContext){0};
    /* Deep-copy owned runtime state so contexts do not share pointers. */
    ctx->dir_stack = NULL;
    ctx->dir_stack_count = 0;
    ctx->dir_stack_capacity = 0;
    ctx->dir_stack_initialised = gShellRuntimeDefault.dir_stack_initialised;
    if (gShellRuntimeDefault.dir_stack_count > 0 &&
        gShellRuntimeDefault.dir_stack) {
        ctx->dir_stack = (char **)calloc(gShellRuntimeDefault.dir_stack_count, sizeof(char *));
        if (ctx->dir_stack) {
            ctx->dir_stack_capacity = gShellRuntimeDefault.dir_stack_count;
            for (size_t i = 0; i < gShellRuntimeDefault.dir_stack_count; ++i) {
                if (gShellRuntimeDefault.dir_stack[i]) {
                    ctx->dir_stack[i] = strdup(gShellRuntimeDefault.dir_stack[i]);
                    if (ctx->dir_stack[i]) {
                        ctx->dir_stack_count++;
                    }
                }
            }
        }
    }

    ctx->hash_entries = NULL;
    ctx->hash_count = 0;
    ctx->hash_capacity = 0;
    if (gShellRuntimeDefault.hash_count > 0 && gShellRuntimeDefault.hash_entries) {
        ctx->hash_entries =
            (ShellHashEntry *)calloc(gShellRuntimeDefault.hash_count, sizeof(ShellHashEntry));
        if (ctx->hash_entries) {
            ctx->hash_capacity = gShellRuntimeDefault.hash_count;
            ctx->hash_count = gShellRuntimeDefault.hash_count;
            for (size_t i = 0; i < ctx->hash_count; ++i) {
                ShellHashEntry *dst = &ctx->hash_entries[i];
                const ShellHashEntry *src = &gShellRuntimeDefault.hash_entries[i];
                dst->hits = src->hits;
                if (src->name) {
                    dst->name = strdup(src->name);
                }
                if (src->path) {
                    dst->path = strdup(src->path);
                }
            }
        }
    }

    ctx->disabled_builtins = NULL;
    ctx->disabled_builtin_count = 0;
    ctx->disabled_builtin_capacity = 0;
    if (gShellRuntimeDefault.disabled_builtin_count > 0 &&
        gShellRuntimeDefault.disabled_builtins) {
        ctx->disabled_builtins = (char **)calloc(gShellRuntimeDefault.disabled_builtin_count,
                                                 sizeof(char *));
        if (ctx->disabled_builtins) {
            ctx->disabled_builtin_capacity = gShellRuntimeDefault.disabled_builtin_count;
            for (size_t i = 0; i < gShellRuntimeDefault.disabled_builtin_count; ++i) {
                if (gShellRuntimeDefault.disabled_builtins[i]) {
                    ctx->disabled_builtins[i] = strdup(gShellRuntimeDefault.disabled_builtins[i]);
                    if (ctx->disabled_builtins[i]) {
                        ctx->disabled_builtin_count++;
                    }
                }
            }
        }
    }

    ctx->history.entries = NULL;
    ctx->history.count = 0;
    ctx->history.capacity = 0;
    if (gShellRuntimeDefault.history.count > 0 && gShellRuntimeDefault.history.entries) {
        ctx->history.entries =
            (char **)calloc(gShellRuntimeDefault.history.count, sizeof(char *));
        if (ctx->history.entries) {
            ctx->history.capacity = gShellRuntimeDefault.history.count;
            for (size_t i = 0; i < gShellRuntimeDefault.history.count; ++i) {
                if (gShellRuntimeDefault.history.entries[i]) {
                    ctx->history.entries[i] = strdup(gShellRuntimeDefault.history.entries[i]);
                    if (ctx->history.entries[i]) {
                        ctx->history.count++;
                    }
                }
            }
        }
    }

    ctx->arg0 = gShellRuntimeDefault.arg0 ? strdup(gShellRuntimeDefault.arg0) : NULL;

    for (int i = 0; i < NSIG; ++i) {
        ctx->signal_traps[i].action = gShellRuntimeDefault.signal_traps[i].action;
        ctx->signal_traps[i].command = gShellRuntimeDefault.signal_traps[i].command ?
                                           strdup(gShellRuntimeDefault.signal_traps[i].command) :
                                           NULL;
    }
    ctx->exit_trap.action = gShellRuntimeDefault.exit_trap.action;
    ctx->exit_trap.command =
        gShellRuntimeDefault.exit_trap.command ? strdup(gShellRuntimeDefault.exit_trap.command) : NULL;

    ctx->jobs = NULL;
    ctx->job_count = 0;
    if (gShellRuntimeDefault.job_count > 0 && gShellRuntimeDefault.jobs) {
        ctx->jobs = (ShellJob *)calloc(gShellRuntimeDefault.job_count, sizeof(ShellJob));
        if (ctx->jobs) {
            ctx->job_count = gShellRuntimeDefault.job_count;
            for (size_t i = 0; i < ctx->job_count; ++i) {
                const ShellJob *src = &gShellRuntimeDefault.jobs[i];
                ShellJob *dst = &ctx->jobs[i];
                dst->pgid = src->pgid;
                dst->running = src->running;
                dst->stopped = src->stopped;
                dst->disowned = src->disowned;
                dst->last_status = src->last_status;
                if (src->pid_count > 0 && src->pids) {
                    dst->pids = (pid_t *)calloc(src->pid_count, sizeof(pid_t));
                    if (dst->pids) {
                        memcpy(dst->pids, src->pids, src->pid_count * sizeof(pid_t));
                        dst->pid_count = src->pid_count;
                    }
                }
                dst->command = src->command ? strdup(src->command) : NULL;
            }
        }
    }

    ctx->array_vars = NULL;
    ctx->array_var_count = 0;
    ctx->array_var_capacity = 0;
    if (gShellRuntimeDefault.array_var_count > 0 && gShellRuntimeDefault.array_vars) {
        ctx->array_vars = (ShellArrayVariable *)calloc(gShellRuntimeDefault.array_var_count,
                                                       sizeof(ShellArrayVariable));
        if (ctx->array_vars) {
            ctx->array_var_capacity = gShellRuntimeDefault.array_var_count;
            for (size_t i = 0; i < gShellRuntimeDefault.array_var_count; ++i) {
                ShellArrayVariable *dst = &ctx->array_vars[i];
                const ShellArrayVariable *src = &gShellRuntimeDefault.array_vars[i];
                dst->name = src->name ? strdup(src->name) : NULL;
                dst->kind = src->kind;
                dst->count = src->count;
                if (src->count > 0 && src->values) {
                    dst->values = (char **)calloc(src->count, sizeof(char *));
                    if (dst->values) {
                        for (size_t j = 0; j < src->count; ++j) {
                            if (src->values[j]) {
                                dst->values[j] = strdup(src->values[j]);
                            }
                        }
                    }
                }
                if (src->kind == SHELL_ARRAY_KIND_ASSOCIATIVE && src->count > 0 && src->keys) {
                    dst->keys = (char **)calloc(src->count, sizeof(char *));
                    if (dst->keys) {
                        for (size_t j = 0; j < src->count; ++j) {
                            if (src->keys[j]) {
                                dst->keys[j] = strdup(src->keys[j]);
                            }
                        }
                    }
                }
                ctx->array_var_count++;
            }
        }
    }

    ctx->readonly_vars = NULL;
    ctx->readonly_var_count = 0;
    ctx->readonly_var_capacity = 0;
    if (gShellRuntimeDefault.readonly_var_count > 0 && gShellRuntimeDefault.readonly_vars) {
        ctx->readonly_vars = (ShellReadonlyEntry *)calloc(gShellRuntimeDefault.readonly_var_count,
                                                          sizeof(ShellReadonlyEntry));
        if (ctx->readonly_vars) {
            ctx->readonly_var_capacity = gShellRuntimeDefault.readonly_var_count;
            for (size_t i = 0; i < gShellRuntimeDefault.readonly_var_count; ++i) {
                if (gShellRuntimeDefault.readonly_vars[i].name) {
                    ctx->readonly_vars[i].name = strdup(gShellRuntimeDefault.readonly_vars[i].name);
                    if (ctx->readonly_vars[i].name) {
                        ctx->readonly_var_count++;
                    }
                }
            }
        }
    }
    ctx->readonly_error_name =
        gShellRuntimeDefault.readonly_error_name ? strdup(gShellRuntimeDefault.readonly_error_name) : NULL;

    ctx->local_scope_stack = NULL;
    ctx->local_scope_depth = 0;
    ctx->local_scope_capacity = 0;
    ctx->loop_stack = NULL;
    ctx->loop_stack_size = 0;
    ctx->loop_stack_capacity = 0;
    return ctx;
}

void shellRuntimeDestroyContext(ShellRuntimeState *ctx) {
    if (!ctx || ctx == &gShellRuntimeDefault) {
        return;
    }
    if (ctx->history.entries) {
        for (size_t i = 0; i < ctx->history.count; ++i) {
            free(ctx->history.entries[i]);
        }
        free(ctx->history.entries);
        ctx->history.entries = NULL;
        ctx->history.count = 0;
        ctx->history.capacity = 0;
    }
    if (ctx->arg0) {
        free(ctx->arg0);
        ctx->arg0 = NULL;
    }
    for (int i = 0; i < NSIG; ++i) {
        shellTrapClearEntry(&ctx->signal_traps[i]);
    }
    shellTrapClearEntry(&ctx->exit_trap);
    if (ctx->jobs) {
        for (size_t i = 0; i < ctx->job_count; ++i) {
            ShellJob *job = &ctx->jobs[i];
            free(job->pids);
            job->pids = NULL;
            job->pid_count = 0;
            free(job->command);
            job->command = NULL;
        }
        free(ctx->jobs);
        ctx->jobs = NULL;
        ctx->job_count = 0;
    }
    if (ctx->array_vars) {
        for (size_t i = 0; i < ctx->array_var_count; ++i) {
            shellArrayVariableClear(&ctx->array_vars[i]);
        }
        free(ctx->array_vars);
        ctx->array_vars = NULL;
        ctx->array_var_count = 0;
        ctx->array_var_capacity = 0;
    }
    if (ctx->readonly_vars) {
        for (size_t i = 0; i < ctx->readonly_var_count; ++i) {
            free(ctx->readonly_vars[i].name);
        }
        free(ctx->readonly_vars);
        ctx->readonly_vars = NULL;
        ctx->readonly_var_count = 0;
        ctx->readonly_var_capacity = 0;
    }
    if (ctx->readonly_error_name) {
        free(ctx->readonly_error_name);
        ctx->readonly_error_name = NULL;
    }
    if (ctx->local_scope_stack) {
        size_t depth = ctx->local_scope_depth;
        for (size_t i = 0; i < depth; ++i) {
            shellLocalScopeFrameReset(&ctx->local_scope_stack[i]);
        }
        free(ctx->local_scope_stack);
        ctx->local_scope_stack = NULL;
        ctx->local_scope_depth = 0;
        ctx->local_scope_capacity = 0;
    }
    if (ctx->loop_stack) {
        size_t size = ctx->loop_stack_size;
        for (size_t i = 0; i < size; ++i) {
            shellLoopFrameFreeData(&ctx->loop_stack[i]);
        }
        free(ctx->loop_stack);
        ctx->loop_stack = NULL;
        ctx->loop_stack_size = 0;
        ctx->loop_stack_capacity = 0;
    }
    if (ctx->dir_stack) {
        for (size_t i = 0; i < ctx->dir_stack_count; ++i) {
            free(ctx->dir_stack[i]);
        }
        free(ctx->dir_stack);
        ctx->dir_stack = NULL;
        ctx->dir_stack_count = 0;
        ctx->dir_stack_capacity = 0;
    }
    if (ctx->disabled_builtins) {
        for (size_t i = 0; i < ctx->disabled_builtin_count; ++i) {
            free(ctx->disabled_builtins[i]);
        }
        free(ctx->disabled_builtins);
        ctx->disabled_builtins = NULL;
        ctx->disabled_builtin_count = 0;
        ctx->disabled_builtin_capacity = 0;
    }
    if (ctx->hash_entries) {
        for (size_t i = 0; i < ctx->hash_count; ++i) {
            free(ctx->hash_entries[i].name);
            free(ctx->hash_entries[i].path);
        }
        free(ctx->hash_entries);
        ctx->hash_entries = NULL;
        ctx->hash_count = 0;
        ctx->hash_capacity = 0;
    }
    free(ctx);
}

typedef enum {
    SHELL_READ_LINE_OK,
    SHELL_READ_LINE_EOF,
    SHELL_READ_LINE_ERROR
} ShellReadLineResult;

static ShellReadLineResult shellReadLineFromFd(int fd,
                                               char **out_line,
                                               size_t *out_length) {
    if (out_line) {
        *out_line = NULL;
    }
    if (out_length) {
        *out_length = 0;
    }
    if (fd < 0) {
        return SHELL_READ_LINE_ERROR;
    }

    size_t capacity = 128;
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        return SHELL_READ_LINE_ERROR;
    }

    size_t length = 0;
    bool saw_any = false;
    while (true) {
        char ch = '\0';
        ssize_t n = read(fd, &ch, 1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buffer);
            return SHELL_READ_LINE_ERROR;
        }
        if (n == 0) {
            if (!saw_any) {
                free(buffer);
                return SHELL_READ_LINE_EOF;
            }
            break;
        }
        saw_any = true;
        if (length + 1 >= capacity) {
            size_t new_capacity = capacity * 2;
            char *resized = (char *)realloc(buffer, new_capacity);
            if (!resized) {
                free(buffer);
                return SHELL_READ_LINE_ERROR;
            }
            buffer = resized;
            capacity = new_capacity;
        }
        buffer[length++] = ch;
        if (ch == '\n') {
            break;
        }
    }

    buffer[length] = '\0';
    if (out_line) {
        *out_line = buffer;
    } else {
        free(buffer);
    }
    if (out_length) {
        *out_length = length;
    }
    return SHELL_READ_LINE_OK;
}

static ShellReadLineResult shellReadLineFromStream(FILE *stream,
                                                   char **out_line,
                                                   size_t *out_length) {
    if (out_line) {
        *out_line = NULL;
    }
    if (out_length) {
        *out_length = 0;
    }
    if (!stream) {
        return SHELL_READ_LINE_ERROR;
    }

    size_t capacity = 128;
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        return SHELL_READ_LINE_ERROR;
    }

    size_t length = 0;
    bool saw_any = false;
    while (true) {
        int ch = fgetc(stream);
        if (ch == EOF) {
            if (ferror(stream)) {
                free(buffer);
                clearerr(stream);
                return SHELL_READ_LINE_ERROR;
            }
            if (!saw_any) {
                free(buffer);
                return SHELL_READ_LINE_EOF;
            }
            break;
        }
        saw_any = true;
        if (length + 1 >= capacity) {
            size_t new_capacity = capacity * 2;
            char *resized = (char *)realloc(buffer, new_capacity);
            if (!resized) {
                free(buffer);
                return SHELL_READ_LINE_ERROR;
            }
            buffer = resized;
            capacity = new_capacity;
        }
        buffer[length++] = (char)ch;
        if (ch == '\n') {
            break;
        }
    }

    buffer[length] = '\0';
    if (out_line) {
        *out_line = buffer;
    } else {
        free(buffer);
    }
    if (out_length) {
        *out_length = length;
    }
    return SHELL_READ_LINE_OK;
}

static bool shellAssignLoopVariable(const char *name, const char *value) {
    if (!name) {
        return false;
    }
    return shellSetTrackedVariable(name, value, false);
}

static void shellLoopTrimBounds(const char **start_ptr, const char **end_ptr) {
    if (!start_ptr || !end_ptr) {
        return;
    }
    const char *start = *start_ptr;
    const char *end = *end_ptr;
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    *start_ptr = start;
    *end_ptr = end;
}

static bool shellLoopGetNumericVariable(const char *name, long long *out_value) {
    if (!name || !out_value) {
        return false;
    }
    char *raw = shellLookupParameterValue(name, strlen(name));
    bool ok = shellArithmeticParseValueString(raw ? raw : "0", out_value);
    free(raw);
    return ok;
}

static bool shellLoopEvalNumeric(const char *expr, long long *out_value) {
    if (!expr) {
        if (out_value) {
            *out_value = 0;
        }
        return true;
    }
    bool eval_error = false;
    char *result = shellEvaluateArithmetic(expr, &eval_error);
    if (eval_error || !result) {
        if (result) {
            free(result);
        }
        shellMarkArithmeticError();
        return false;
    }
    long long value = 0;
    bool ok = shellArithmeticParseValueString(result, &value);
    free(result);
    if (!ok) {
        shellMarkArithmeticError();
        return false;
    }
    if (out_value) {
        *out_value = value;
    }
    return true;
}

static bool shellLoopEvalSubstring(const char *start, const char *end, long long *out_value) {
    if (!start || !end || end < start) {
        return false;
    }
    shellLoopTrimBounds(&start, &end);
    size_t len = (size_t)(end - start);
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        return false;
    }
    memcpy(copy, start, len);
    copy[len] = '\0';
    long long value = 0;
    bool ok = shellLoopEvalNumeric(copy, &value);
    free(copy);
    if (!ok) {
        return false;
    }
    if (out_value) {
        *out_value = value;
    }
    return true;
}

static bool shellLoopAssignNumericValue(const char *name, long long value) {
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

static const char *shellLoopParseVariableName(const char *start, const char *end, char **out_name) {
    if (!start || !end || start >= end) {
        return NULL;
    }
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    if (start >= end || (!isalpha((unsigned char)*start) && *start != '_')) {
        return NULL;
    }
    const char *cursor = start + 1;
    while (cursor < end && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    size_t len = (size_t)(cursor - start);
    char *name = (char *)malloc(len + 1);
    if (!name) {
        return NULL;
    }
    memcpy(name, start, len);
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

static bool shellLoopExecuteCForExpressionRange(const char *start, const char *end);

static bool shellLoopExecuteCForSingleExpression(const char *start, const char *end) {
    shellLoopTrimBounds(&start, &end);
    if (!start || !end || start >= end) {
        return true;
    }

    size_t length = (size_t)(end - start);
    if (length >= 2 && start[0] == '+' && start[1] == '+') {
        const char *after_op = start + 2;
        char *name = NULL;
        const char *rest = shellLoopParseVariableName(after_op, end, &name);
        if (!rest || !name) {
            free(name);
            return false;
        }
        if (rest != end) {
            free(name);
            return false;
        }
        long long value = 0;
        if (!shellLoopGetNumericVariable(name, &value)) {
            free(name);
            return false;
        }
        value += 1;
        bool ok = shellLoopAssignNumericValue(name, value);
        free(name);
        return ok;
    }
    if (length >= 2 && start[0] == '-' && start[1] == '-') {
        const char *after_op = start + 2;
        char *name = NULL;
        const char *rest = shellLoopParseVariableName(after_op, end, &name);
        if (!rest || !name) {
            free(name);
            return false;
        }
        if (rest != end) {
            free(name);
            return false;
        }
        long long value = 0;
        if (!shellLoopGetNumericVariable(name, &value)) {
            free(name);
            return false;
        }
        value -= 1;
        bool ok = shellLoopAssignNumericValue(name, value);
        free(name);
        return ok;
    }

    char *name = NULL;
    const char *rest = shellLoopParseVariableName(start, end, &name);
    if (rest && name) {
        const char *cursor = rest;
        if (cursor < end && cursor + 1 <= end && cursor[0] == '+' && cursor + 1 < end && cursor[1] == '+') {
            cursor += 2;
            while (cursor < end && isspace((unsigned char)*cursor)) {
                cursor++;
            }
            if (cursor != end) {
                free(name);
                return false;
            }
            long long value = 0;
            if (!shellLoopGetNumericVariable(name, &value)) {
                free(name);
                return false;
            }
            value += 1;
            bool ok = shellLoopAssignNumericValue(name, value);
            free(name);
            return ok;
        }
        if (cursor < end && cursor + 1 <= end && cursor[0] == '-' && cursor + 1 < end && cursor[1] == '-') {
            cursor += 2;
            while (cursor < end && isspace((unsigned char)*cursor)) {
                cursor++;
            }
            if (cursor != end) {
                free(name);
                return false;
            }
            long long value = 0;
            if (!shellLoopGetNumericVariable(name, &value)) {
                free(name);
                return false;
            }
            value -= 1;
            bool ok = shellLoopAssignNumericValue(name, value);
            free(name);
            return ok;
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
            shellLoopTrimBounds(&rhs_start, &rhs_end);
            if (rhs_start >= rhs_end) {
                free(name);
                return false;
            }
            long long rhs_value = 0;
            if (!shellLoopEvalSubstring(rhs_start, rhs_end, &rhs_value)) {
                free(name);
                return false;
            }
            long long result = rhs_value;
            if (assign_op != '=') {
                long long current = 0;
                if (!shellLoopGetNumericVariable(name, &current)) {
                    free(name);
                    return false;
                }
                switch (assign_op) {
                    case '+':
                        result = current + rhs_value;
                        break;
                    case '-':
                        result = current - rhs_value;
                        break;
                    case '*':
                        result = current * rhs_value;
                        break;
                    case '/':
                        if (rhs_value == 0) {
                            free(name);
                            return false;
                        }
                        result = current / rhs_value;
                        break;
                    case '%':
                        if (rhs_value == 0) {
                            free(name);
                            return false;
                        }
                        result = current % rhs_value;
                        break;
                    default:
                        free(name);
                        return false;
                }
            }
            bool ok = shellLoopAssignNumericValue(name, result);
            free(name);
            return ok;
        }
        if (cursor == end) {
            long long value = 0;
            bool ok = shellLoopGetNumericVariable(name, &value);
            free(name);
            return ok;
        }
    }
    free(name);

    long long value = 0;
    return shellLoopEvalSubstring(start, end, &value);
}

static bool shellLoopExecuteCForExpressionRange(const char *start, const char *end) {
    shellLoopTrimBounds(&start, &end);
    if (!start || !end || start >= end) {
        return true;
    }
    int depth = 0;
    const char *segment_start = start;
    for (const char *cursor = start; cursor < end; ++cursor) {
        char ch = *cursor;
        if (ch == '(') {
            depth++;
        } else if (ch == ')') {
            if (depth > 0) {
                depth--;
            }
        } else if (ch == ',' && depth == 0) {
            if (!shellLoopExecuteCForSingleExpression(segment_start, cursor)) {
                return false;
            }
            segment_start = cursor + 1;
        }
    }
    return shellLoopExecuteCForSingleExpression(segment_start, end);
}

static bool shellLoopExecuteCForExpression(const char *expr) {
    if (!expr) {
        return true;
    }
    const char *start = expr;
    const char *end = expr + strlen(expr);
    return shellLoopExecuteCForExpressionRange(start, end);
}

static const char *shellLoopFindTopLevelOperator(const char *start, const char *end,
                                                const char **ops,
                                                const size_t *lengths,
                                                size_t count,
                                                size_t *out_index) {
    if (out_index) {
        *out_index = SIZE_MAX;
    }
    if (!start || !end || start >= end || !ops || !lengths) {
        return NULL;
    }
    int depth = 0;
    for (const char *cursor = start; cursor < end; ++cursor) {
        char ch = *cursor;
        if (ch == '(') {
            depth++;
            continue;
        }
        if (ch == ')') {
            if (depth > 0) {
                depth--;
            }
            continue;
        }
        if (depth != 0) {
            continue;
        }
        for (size_t i = 0; i < count; ++i) {
            size_t len = lengths[i];
            if (len == 0 || cursor + len > end) {
                continue;
            }
            if (strncmp(cursor, ops[i], len) == 0) {
                if (out_index) {
                    *out_index = i;
                }
                return cursor;
            }
        }
    }
    return NULL;
}

static bool shellLoopEvaluateConditionRange(const char *start, const char *end, bool *out_ready) {
    shellLoopTrimBounds(&start, &end);
    if (!start || !end || start >= end) {
        if (out_ready) {
            *out_ready = true;
        }
        return true;
    }

    if (*start == '(') {
        int depth = 0;
        const char *cursor = start;
        bool enclosed = false;
        while (cursor < end) {
            char ch = *cursor;
            if (ch == '(') {
                depth++;
            } else if (ch == ')') {
                depth--;
                if (depth == 0) {
                    enclosed = (cursor == end - 1);
                    break;
                }
            }
            cursor++;
        }
        if (enclosed) {
            return shellLoopEvaluateConditionRange(start + 1, end - 1, out_ready);
        }
    }

    while (start < end && *start == '!') {
        const char *next = start + 1;
        while (next < end && isspace((unsigned char)*next)) {
            next++;
        }
        bool inner = false;
        if (!shellLoopEvaluateConditionRange(next, end, &inner)) {
            return false;
        }
        if (out_ready) {
            *out_ready = !inner;
        }
        return true;
    }

    const char *or_ops[] = {"||"};
    size_t or_lens[] = {2};
    size_t op_index = SIZE_MAX;
    const char *pos = shellLoopFindTopLevelOperator(start, end, or_ops, or_lens, 1, &op_index);
    if (pos) {
        bool left = false;
        if (!shellLoopEvaluateConditionRange(start, pos, &left)) {
            return false;
        }
        if (left) {
            if (out_ready) {
                *out_ready = true;
            }
            return true;
        }
        bool right = false;
        if (!shellLoopEvaluateConditionRange(pos + or_lens[0], end, &right)) {
            return false;
        }
        if (out_ready) {
            *out_ready = right;
        }
        return true;
    }

    const char *and_ops[] = {"&&"};
    size_t and_lens[] = {2};
    pos = shellLoopFindTopLevelOperator(start, end, and_ops, and_lens, 1, &op_index);
    if (pos) {
        bool left = false;
        if (!shellLoopEvaluateConditionRange(start, pos, &left)) {
            return false;
        }
        if (!left) {
            if (out_ready) {
                *out_ready = false;
            }
            return true;
        }
        bool right = false;
        if (!shellLoopEvaluateConditionRange(pos + and_lens[0], end, &right)) {
            return false;
        }
        if (out_ready) {
            *out_ready = right;
        }
        return true;
    }

    const char *equality_ops[] = {"==", "!="};
    size_t equality_lens[] = {2, 2};
    pos = shellLoopFindTopLevelOperator(start, end, equality_ops, equality_lens, 2, &op_index);
    if (pos) {
        long long lhs = 0;
        long long rhs = 0;
        if (!shellLoopEvalSubstring(start, pos, &lhs) ||
            !shellLoopEvalSubstring(pos + equality_lens[op_index], end, &rhs)) {
            return false;
        }
        bool truth = (op_index == 0) ? (lhs == rhs) : (lhs != rhs);
        if (out_ready) {
            *out_ready = truth;
        }
        return true;
    }

    const char *rel_ops[] = {"<=", ">=", "<", ">"};
    size_t rel_lens[] = {2, 2, 1, 1};
    pos = shellLoopFindTopLevelOperator(start, end, rel_ops, rel_lens, 4, &op_index);
    if (pos) {
        long long lhs = 0;
        long long rhs = 0;
        if (!shellLoopEvalSubstring(start, pos, &lhs) ||
            !shellLoopEvalSubstring(pos + rel_lens[op_index], end, &rhs)) {
            return false;
        }
        bool truth = false;
        switch (op_index) {
            case 0: truth = (lhs <= rhs); break;
            case 1: truth = (lhs >= rhs); break;
            case 2: truth = (lhs < rhs); break;
            case 3: truth = (lhs > rhs); break;
        }
        if (out_ready) {
            *out_ready = truth;
        }
        return true;
    }

    long long value = 0;
    if (!shellLoopEvalSubstring(start, end, &value)) {
        return false;
    }
    if (out_ready) {
        *out_ready = (value != 0);
    }
    return true;
}

static bool shellLoopEvaluateConditionText(const char *expr, bool *out_ready) {
    if (!expr) {
        if (out_ready) {
            *out_ready = true;
        }
        shellUpdateStatus(0);
        return true;
    }
    const char *start = expr;
    const char *end = expr + strlen(expr);
    shellRuntimeEnterCondition();
    bool ready = true;
    if (!shellLoopEvaluateConditionRange(start, end, &ready)) {
        shellRuntimeLeaveCondition();
        return false;
    }
    if (out_ready) {
        *out_ready = ready;
    }
    shellUpdateStatus(0);
    if (!gShellExitRequested) {
        gShellRuntime.errexit_pending = false;
        gShellRuntime.errexit_defer_active = false;
        if (gShellCurrentVm) {
            gShellCurrentVm->exit_requested = false;
            gShellCurrentVm->abort_requested = false;
            gShellCurrentVm->current_builtin_name = NULL;
        }
    }
    shellRuntimeLeaveCondition();
    return true;
}

static bool shellLoopEvaluateCForCondition(ShellLoopFrame *frame, bool *out_ready) {
    if (!frame || !out_ready) {
        return false;
    }
    if (frame->cfor_condition_cached) {
        *out_ready = frame->cfor_condition_value;
        return true;
    }
    bool ready = false;
    if (!shellLoopEvaluateConditionText(frame->cfor_condition, &ready)) {
        return false;
    }
    frame->cfor_condition_cached = true;
    frame->cfor_condition_value = ready;
    *out_ready = ready;
    return true;
}
static bool shellLoopExecuteCForInitializer(ShellLoopFrame *frame) {
    if (!frame) {
        return false;
    }
    frame->cfor_condition_cached = false;
    if (!frame->cfor_init || frame->cfor_init[0] == '\0') {
        return true;
    }
    if (!shellLoopExecuteCForExpression(frame->cfor_init)) {
        frame->skip_body = true;
        frame->break_pending = true;
        return false;
    }
    return true;
}

static bool shellLoopExecuteCForUpdate(ShellLoopFrame *frame) {
    if (!frame) {
        return false;
    }
    frame->cfor_condition_cached = false;
    if (!frame->cfor_update || frame->cfor_update[0] == '\0') {
        return true;
    }
    return shellLoopExecuteCForExpression(frame->cfor_update);
}

static ShellLoopFrame *shellLoopPushFrame(ShellLoopKind kind) {
    if (!shellLoopEnsureCapacity(gShellLoopStackSize + 1)) {
        return NULL;
    }
    ShellLoopFrame frame;
    frame.kind = kind;
    frame.skip_body = false;
    frame.break_pending = false;
    frame.continue_pending = false;
    frame.for_variable = NULL;
    frame.for_values = NULL;
    frame.for_count = 0;
    frame.for_index = 0;
    frame.for_active = false;
    frame.cfor_init = NULL;
    frame.cfor_condition = NULL;
    frame.cfor_update = NULL;
    frame.cfor_condition_cached = false;
    frame.cfor_condition_value = false;
    frame.cond_kind = SHELL_LOOP_CONDITION_NONE;
    frame.cond_words = NULL;
    frame.cond_word_count = 0;
    frame.cond_arith_expression = NULL;
    frame.body_kind = SHELL_LOOP_BODY_KIND_NONE;
    frame.body_words = NULL;
    frame.body_word_count = 0;
    frame.body_arith_expression = NULL;
    frame.redirs_active = false;
    frame.applied_redirs = NULL;
    frame.applied_redir_count = 0;
    frame.redir_backups = NULL;
    frame.redir_backup_count = 0;
    frame.redir_backup_capacity = 0;
    frame.pipeline_subshell = false;
    frame.pipeline_scope_pushed = false;
    frame.pipeline_prev_local_scope_flag = false;
    frame.pipeline_prev_pipeline_flag = false;
    gShellLoopStack[gShellLoopStackSize++] = frame;
    return &gShellLoopStack[gShellLoopStackSize - 1];
}

static ShellLoopFrame *shellLoopTop(void) {
    if (gShellLoopStackSize == 0) {
        return NULL;
    }
    return &gShellLoopStack[gShellLoopStackSize - 1];
}

static void shellLoopPopFrame(void) {
    if (gShellLoopStackSize == 0) {
        return;
    }
    ShellLoopFrame *frame = &gShellLoopStack[gShellLoopStackSize - 1];
    shellLoopFrameFreeData(frame);
    gShellLoopStackSize--;
    if (gShellLoopStackSize == 0) {
        gShellRuntime.break_requested = false;
        gShellRuntime.continue_requested = false;
        gShellRuntime.break_requested_levels = 0;
        gShellRuntime.continue_requested_levels = 0;
    }
}

static void shellSignalHandler(int signo) {
    if (signo <= 0 || signo >= NSIG) {
        return;
    }
    gShellPendingSignals[signo] = 1;
}

static bool shellLoopSkipActive(void) {
    for (size_t i = gShellLoopStackSize; i > 0; --i) {
        if (gShellLoopStack[i - 1].skip_body) {
            return true;
        }
    }
    return false;
}

static void shellLoopRequestBreakLevels(int levels) {
    if (levels <= 0) {
        levels = 1;
    }
    size_t idx = gShellLoopStackSize;
    while (idx > 0 && levels > 0) {
        ShellLoopFrame *frame = &gShellLoopStack[idx - 1];
        frame->skip_body = true;
        frame->break_pending = true;
        frame->continue_pending = false;
        idx--;
        levels--;
    }
    if (levels > 0) {
        shellRuntimeRequestExit();
    }
}

static void shellLoopRequestContinueLevels(int levels) {
    if (levels <= 0) {
        levels = 1;
    }
    if (gShellLoopStackSize == 0) {
        return;
    }
    size_t idx = gShellLoopStackSize;
    int remaining = levels;
    while (idx > 0 && remaining > 1) {
        ShellLoopFrame *frame = &gShellLoopStack[idx - 1];
        frame->skip_body = true;
        frame->break_pending = true;
        frame->continue_pending = false;
        idx--;
        remaining--;
    }
    if (idx > 0) {
        ShellLoopFrame *target = &gShellLoopStack[idx - 1];
        target->skip_body = true;
        target->continue_pending = true;
        target->break_pending = false;
    }
}

VM *shellSwapCurrentVm(VM *vm) {
    VM *previous = gShellCurrentVm;
    shellContextPush(shellGetContext());
    gShellCurrentVm = vm;
    ShellRuntimeState *ctx = NULL;
    if (vm && vm->frontendContext) {
        ctx = (ShellRuntimeState *)vm->frontendContext;
    }
    shellSetContext(ctx);
    return previous;
}

void shellRestoreCurrentVm(VM *vm) {
    gShellCurrentVm = vm;
    ShellRuntimeState *restore = shellContextPop();
    shellSetContext(restore);
}

static void shellInitJobControlState(void) {
    if (gShellRuntime.job_control_initialized) {
        return;
    }

    gShellRuntime.job_control_initialized = true;
    gShellRuntime.tty_fd = STDIN_FILENO;

    if (gShellRuntime.tty_fd < 0) {
        gShellRuntime.tty_fd = -1;
        return;
    }

#if defined(PSCAL_TARGET_IOS)
    bool virtual_tty = pscalRuntimeVirtualTTYEnabled();
    if (!virtual_tty && !pscalRuntimeFdHasRealTTY(gShellRuntime.tty_fd)) {
        gShellRuntime.tty_fd = -1;
        return;
    }
#else
    if (!pscalRuntimeFdHasRealTTY(gShellRuntime.tty_fd)) {
        gShellRuntime.tty_fd = -1;
        return;
    }
#endif

    struct sigaction ignore_action;
    memset(&ignore_action, 0, sizeof(ignore_action));
    sigemptyset(&ignore_action.sa_mask);
    ignore_action.sa_handler = SIG_IGN;
    (void)sigaction(SIGTTIN, &ignore_action, NULL);
    (void)sigaction(SIGTTOU, &ignore_action, NULL);

#if defined(PSCAL_TARGET_IOS)
    pid_t shell_pid = (pid_t)vprocGetShellSelfPid();
    if (shell_pid <= 0) {
        shell_pid = getpid();
    }
    pid_t current_pgid = (shell_pid > 0) ? (pid_t)vprocGetPgid((int)shell_pid) : -1;
    if (current_pgid <= 0) {
        current_pgid = shell_pid;
    }
    gShellRuntime.shell_pgid = current_pgid;
    gShellRuntime.job_control_enabled = true;
    if (shell_pid > 0 && current_pgid > 0) {
        int sid = vprocGetSid((int)shell_pid);
        if (sid > 0) {
            vprocSetForegroundPgid(sid, (int)current_pgid);
        }
    }
    return;
#else
    pid_t shell_pid = getpid();
    pid_t current_pgid = getpgrp();
    if (current_pgid != shell_pid) {
        if (setpgid(0, 0) == 0) {
            current_pgid = shell_pid;
        } else {
            current_pgid = getpgrp();
        }
    }

    gShellRuntime.shell_pgid = current_pgid;
#endif
}

static void shellEnsureJobControl(void) {
    if (!shellRuntimeIsInteractive()) {
        gShellRuntime.job_control_enabled = false;
        return;
    }

    shellInitJobControlState();

#if defined(PSCAL_TARGET_IOS)
    /* iOS has no controlling TTY for background threads; keep job tracking enabled so
     * synthetic vproc pids can be managed with fg/bg/kill/wait semantics even without
     * tcsetpgrp support. */
    gShellRuntime.job_control_enabled = true;
    if (gShellRuntime.shell_pgid <= 0) {
        int shell_pid = vprocGetShellSelfPid();
        if (shell_pid > 0) {
            gShellRuntime.shell_pgid = vprocGetPgid(shell_pid);
        } else {
            gShellRuntime.shell_pgid = getpgrp();
        }
        if (gShellRuntime.shell_pgid <= 0 && shell_pid > 0) {
            gShellRuntime.shell_pgid = shell_pid;
        }
    }
    return;
#endif

    if (gShellRuntime.tty_fd < 0) {
        gShellRuntime.job_control_enabled = false;
        return;
    }

    bool virtual_tty =
#if defined(PSCAL_TARGET_IOS)
        pscalRuntimeVirtualTTYEnabled();
#else
        false;
#endif

    if (!virtual_tty) {
        if (!pscalRuntimeFdHasRealTTY(gShellRuntime.tty_fd)) {
            gShellRuntime.job_control_enabled = false;
            gShellRuntime.tty_fd = -1;
            return;
        }
    }

    pid_t pgid = getpgrp();
    if (pgid <= 0) {
        gShellRuntime.job_control_enabled = false;
        return;
    }

    gShellRuntime.shell_pgid = pgid;
#if defined(PSCAL_TARGET_IOS)
    int shell_pid = vprocGetShellSelfPid();
    int sid = (shell_pid > 0) ? vprocGetSid(shell_pid) : -1;
    if (sid > 0) {
        vprocSetForegroundPgid(sid, (int)pgid);
    }
#endif

    if (virtual_tty) {
        /* No controlling TTY, but still allow the shell’s job table to track
         * background work. Foreground handing of tcsetpgrp stays skipped. */
        gShellRuntime.job_control_enabled = true;
        return;
    }

    while (true) {
        pid_t foreground = tcgetpgrp(gShellRuntime.tty_fd);
        if (foreground < 0) {
            if (errno == EINTR) {
                continue;
            }
            gShellRuntime.job_control_enabled = false;
            return;
        }
        if (foreground == pgid) {
            gShellRuntime.job_control_enabled = true;
            return;
        }
        if (tcsetpgrp(gShellRuntime.tty_fd, pgid) != 0) {
            if (errno == EINTR) {
                continue;
            }
            gShellRuntime.job_control_enabled = false;
            return;
        }
    }
}

static void shellJobControlSetForeground(pid_t pgid) {
    if (!gShellRuntime.job_control_enabled || gShellRuntime.tty_fd < 0 || pgid <= 0) {
        return;
    }
#if defined(PSCAL_TARGET_IOS)
    int shell_pid = vprocGetShellSelfPid();
    int sid = (shell_pid > 0) ? vprocGetSid(shell_pid) : -1;
    if (sid > 0) {
        vprocSetForegroundPgid(sid, (int)pgid);
    }
#endif
    if (
#if defined(PSCAL_TARGET_IOS)
        pscalRuntimeVirtualTTYEnabled()
#else
        false
#endif
    ) {
        /* Virtual TTY: skip tcsetpgrp but leave job tracking active. */
        return;
    }
    while (tcsetpgrp(gShellRuntime.tty_fd, pgid) != 0) {
        if (errno == EINTR) {
            continue;
        }
        break;
    }
}

static void shellJobControlRestoreForeground(void) {
    if (!gShellRuntime.job_control_enabled || gShellRuntime.tty_fd < 0) {
        return;
    }
#if defined(PSCAL_TARGET_IOS)
    int shell_pid = vprocGetShellSelfPid();
    int sid = (shell_pid > 0) ? vprocGetSid(shell_pid) : -1;
    if (sid > 0 && gShellRuntime.shell_pgid > 0) {
        vprocSetForegroundPgid(sid, (int)gShellRuntime.shell_pgid);
    }
#endif
    if (
#if defined(PSCAL_TARGET_IOS)
        pscalRuntimeVirtualTTYEnabled()
#else
        false
#endif
    ) {
        return;
    }
    pid_t target = gShellRuntime.shell_pgid > 0 ? gShellRuntime.shell_pgid : getpgrp();
    if (target <= 0) {
        return;
    }
    while (tcsetpgrp(gShellRuntime.tty_fd, target) != 0) {
        if (errno == EINTR) {
            continue;
        }
        break;
    }
}

typedef struct {
    char *subject;
    bool matched;
} ShellCaseContext;

typedef struct {
    ShellCaseContext *items;
    size_t count;
    size_t capacity;
} ShellCaseContextStack;

static ShellCaseContextStack gShellCaseStack = {NULL, 0, 0};

static bool shellCaseStackPush(const char *subject_text) {
    if (!subject_text) {
        subject_text = "";
    }
    char *copy = strdup(subject_text);
    if (!copy) {
        return false;
    }
    if (gShellCaseStack.count + 1 > gShellCaseStack.capacity) {
        size_t new_capacity = gShellCaseStack.capacity ? gShellCaseStack.capacity * 2 : 4;
        ShellCaseContext *items = realloc(gShellCaseStack.items, new_capacity * sizeof(ShellCaseContext));
        if (!items) {
            free(copy);
            return false;
        }
        gShellCaseStack.items = items;
        gShellCaseStack.capacity = new_capacity;
    }
    ShellCaseContext *ctx = &gShellCaseStack.items[gShellCaseStack.count++];
    ctx->subject = copy;
    ctx->matched = false;
    return true;
}

static ShellCaseContext *shellCaseStackTop(void) {
    if (gShellCaseStack.count == 0) {
        return NULL;
    }
    return &gShellCaseStack.items[gShellCaseStack.count - 1];
}

static void shellCaseStackPop(void) {
    if (gShellCaseStack.count == 0) {
        return;
    }
    ShellCaseContext *ctx = &gShellCaseStack.items[gShellCaseStack.count - 1];
    free(ctx->subject);
    ctx->subject = NULL;
    gShellCaseStack.count--;
    if (gShellCaseStack.count == 0 && gShellCaseStack.items) {
        free(gShellCaseStack.items);
        gShellCaseStack.items = NULL;
        gShellCaseStack.capacity = 0;
    }
}

typedef enum {
    SHELL_META_SUBSTITUTION_DOLLAR,
    SHELL_META_SUBSTITUTION_BACKTICK
} ShellMetaSubstitutionStyle;

typedef struct {
    ShellMetaSubstitutionStyle style;
    size_t span_length;
    char *command;
} ShellMetaSubstitution;

#if defined(__GNUC__) || defined(__clang__)
#define SHELL_STATIC_UNUSED static inline __attribute__((unused))
#else
#define SHELL_STATIC_UNUSED static inline
#endif

typedef struct {
    char *command;
    char *result;
    size_t result_length;
    size_t generation;
    unsigned long hits;
    unsigned long stores;
} ShellScalarCacheEntry;

typedef struct {
    ShellScalarCacheEntry *entry;
    char *command;
    char *capture;
    size_t capture_length;
    bool from_cache;
} ShellScalarCaptureFrame;

static ShellScalarCacheEntry *gShellScalarCache = NULL;
static size_t gShellScalarCacheCount = 0;
static size_t gShellScalarCacheCapacity = 0;
static size_t gShellScalarCacheGeneration = 0;

static ShellScalarCaptureFrame *gShellScalarCaptureStack = NULL;
static size_t gShellScalarCaptureDepth = 0;
static size_t gShellScalarCaptureCapacity = 0;

SHELL_STATIC_UNUSED void shellScalarCacheEntryDispose(ShellScalarCacheEntry *entry) {
    if (!entry) {
        return;
    }
    free(entry->command);
    entry->command = NULL;
    free(entry->result);
    entry->result = NULL;
    entry->result_length = 0;
    entry->generation = 0;
    entry->hits = 0;
    entry->stores = 0;
}

SHELL_STATIC_UNUSED bool shellScalarCacheEnsureCapacity(size_t needed) {
    if (gShellScalarCacheCapacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellScalarCacheCapacity ? (gShellScalarCacheCapacity * 2) : 4;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    ShellScalarCacheEntry *resized =
        (ShellScalarCacheEntry *)realloc(gShellScalarCache, new_capacity * sizeof(ShellScalarCacheEntry));
    if (!resized) {
        return false;
    }
    for (size_t i = gShellScalarCacheCapacity; i < new_capacity; ++i) {
        resized[i].command = NULL;
        resized[i].result = NULL;
        resized[i].result_length = 0;
        resized[i].generation = 0;
        resized[i].hits = 0;
        resized[i].stores = 0;
    }
    gShellScalarCache = resized;
    gShellScalarCacheCapacity = new_capacity;
    return true;
}

SHELL_STATIC_UNUSED bool shellScalarCacheEntryEquals(const ShellScalarCacheEntry *entry,
                                                     const char *command) {
    if (!entry || !command || !entry->command) {
        return false;
    }
    return strcmp(entry->command, command) == 0;
}

SHELL_STATIC_UNUSED ShellScalarCacheEntry *shellScalarCacheFind(const char *command) {
    if (!command || *command == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < gShellScalarCacheCount;) {
        ShellScalarCacheEntry *entry = &gShellScalarCache[i];
        if (entry->generation != gShellScalarCacheGeneration) {
            shellScalarCacheEntryDispose(entry);
            if (i + 1 < gShellScalarCacheCount) {
                memmove(&gShellScalarCache[i],
                        &gShellScalarCache[i + 1],
                        (gShellScalarCacheCount - i - 1) * sizeof(ShellScalarCacheEntry));
            }
            gShellScalarCacheCount--;
            continue;
        }
        if (shellScalarCacheEntryEquals(entry, command)) {
            return entry;
        }
        ++i;
    }
    return NULL;
}

SHELL_STATIC_UNUSED ShellScalarCacheEntry *shellScalarCacheStore(const char *command,
                                                                 const char *result,
                                                                 size_t result_length) {
    if (!command || *command == '\0') {
        return NULL;
    }
    size_t copy_length = 0;
    if (result) {
        copy_length = result_length ? result_length : strlen(result);
    }
    char *copy = (char *)malloc(copy_length + 1);
    if (!copy) {
        return NULL;
    }
    if (copy_length > 0 && result) {
        memcpy(copy, result, copy_length);
    }
    copy[copy_length] = '\0';

    ShellScalarCacheEntry *existing = shellScalarCacheFind(command);
    if (existing) {
        free(existing->result);
        existing->result = copy;
        existing->result_length = copy_length;
        existing->generation = gShellScalarCacheGeneration;
        existing->hits = 0;
        existing->stores++;
        gShellRuntime.scalar_cache_generation = gShellScalarCacheGeneration;
        return existing;
    }

    if (!shellScalarCacheEnsureCapacity(gShellScalarCacheCount + 1)) {
        free(copy);
        return NULL;
    }
    ShellScalarCacheEntry *entry = &gShellScalarCache[gShellScalarCacheCount];
    entry->command = strdup(command);
    if (!entry->command) {
        free(copy);
        return NULL;
    }
    entry->result = copy;
    entry->result_length = copy_length;
    entry->generation = gShellScalarCacheGeneration;
    entry->hits = 0;
    entry->stores = 1;
    gShellScalarCacheCount++;
    gShellRuntime.scalar_cache_generation = gShellScalarCacheGeneration;
    return entry;
}

SHELL_STATIC_UNUSED void shellScalarCacheRemoveAt(size_t index) {
    if (index >= gShellScalarCacheCount) {
        return;
    }
    shellScalarCacheEntryDispose(&gShellScalarCache[index]);
    if (index + 1 < gShellScalarCacheCount) {
        memmove(&gShellScalarCache[index],
                &gShellScalarCache[index + 1],
                (gShellScalarCacheCount - index - 1) * sizeof(ShellScalarCacheEntry));
    }
    gShellScalarCacheCount--;
}

SHELL_STATIC_UNUSED void shellScalarCacheClear(void) {
    for (size_t i = 0; i < gShellScalarCacheCount; ++i) {
        shellScalarCacheEntryDispose(&gShellScalarCache[i]);
    }
    free(gShellScalarCache);
    gShellScalarCache = NULL;
    gShellScalarCacheCount = 0;
    gShellScalarCacheCapacity = 0;
    gShellScalarCacheGeneration = 0;
    gShellRuntime.scalar_cache_generation = gShellScalarCacheGeneration;
}

SHELL_STATIC_UNUSED void shellScalarCacheInvalidate(void) {
    for (size_t i = 0; i < gShellScalarCacheCount; ++i) {
        shellScalarCacheEntryDispose(&gShellScalarCache[i]);
    }
    gShellScalarCacheCount = 0;
    if (gShellScalarCacheGeneration == SIZE_MAX) {
        gShellScalarCacheGeneration = 0;
    } else {
        gShellScalarCacheGeneration++;
    }
    gShellRuntime.scalar_cache_generation = gShellScalarCacheGeneration;
}

SHELL_STATIC_UNUSED bool shellCaptureStackEnsureCapacity(size_t needed) {
    if (gShellScalarCaptureCapacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellScalarCaptureCapacity ? (gShellScalarCaptureCapacity * 2) : 4;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    ShellScalarCaptureFrame *resized =
        (ShellScalarCaptureFrame *)realloc(gShellScalarCaptureStack,
                                           new_capacity * sizeof(ShellScalarCaptureFrame));
    if (!resized) {
        return false;
    }
    for (size_t i = gShellScalarCaptureCapacity; i < new_capacity; ++i) {
        resized[i].entry = NULL;
        resized[i].command = NULL;
        resized[i].capture = NULL;
        resized[i].capture_length = 0;
        resized[i].from_cache = false;
    }
    gShellScalarCaptureStack = resized;
    gShellScalarCaptureCapacity = new_capacity;
    return true;
}

SHELL_STATIC_UNUSED bool shellCaptureStackPush(ShellScalarCacheEntry *entry,
                                               const char *command,
                                               const char *capture,
                                               size_t capture_length,
                                               bool from_cache) {
    if (!shellCaptureStackEnsureCapacity(gShellScalarCaptureDepth + 1)) {
        return false;
    }
    ShellScalarCaptureFrame *frame = &gShellScalarCaptureStack[gShellScalarCaptureDepth];
    frame->entry = entry;
    frame->command = command ? strdup(command) : NULL;
    if (command && !frame->command) {
        return false;
    }
    frame->capture = NULL;
    frame->capture_length = 0;
    if (capture) {
        size_t length = capture_length;
        if (length == 0) {
            length = strlen(capture);
        }
        frame->capture = (char *)malloc(length + 1);
        if (!frame->capture) {
            free(frame->command);
            frame->command = NULL;
            return false;
        }
        memcpy(frame->capture, capture, length);
        frame->capture[length] = '\0';
        frame->capture_length = length;
    }
    frame->from_cache = from_cache;
    gShellScalarCaptureDepth++;
    if (gShellScalarCaptureDepth > gShellRuntime.scalar_capture_peak_depth) {
        gShellRuntime.scalar_capture_peak_depth = gShellScalarCaptureDepth;
    }
    return true;
}

SHELL_STATIC_UNUSED void shellCaptureStackPop(void) {
    if (gShellScalarCaptureDepth == 0) {
        return;
    }
    size_t index = gShellScalarCaptureDepth - 1;
    ShellScalarCaptureFrame *frame = &gShellScalarCaptureStack[index];
    free(frame->command);
    frame->command = NULL;
    free(frame->capture);
    frame->capture = NULL;
    frame->capture_length = 0;
    frame->entry = NULL;
    frame->from_cache = false;
    gShellScalarCaptureDepth--;
    if (gShellScalarCaptureDepth == 0) {
        free(gShellScalarCaptureStack);
        gShellScalarCaptureStack = NULL;
        gShellScalarCaptureCapacity = 0;
    }
}

SHELL_STATIC_UNUSED ShellScalarCaptureFrame *shellCaptureStackTop(void) {
    if (gShellScalarCaptureDepth == 0) {
        return NULL;
    }
    return &gShellScalarCaptureStack[gShellScalarCaptureDepth - 1];
}

SHELL_STATIC_UNUSED void shellCaptureStackClear(void) {
    while (gShellScalarCaptureDepth > 0) {
        shellCaptureStackPop();
    }
}

#undef SHELL_STATIC_UNUSED
