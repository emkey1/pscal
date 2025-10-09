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
#include <time.h>

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

static ShellArrayVariable *gShellArrayVars = NULL;
static size_t gShellArrayVarCount = 0;
static size_t gShellArrayVarCapacity = 0;
static int gShellAssociativeArraySupport = -1;
static int gShellBindInteractiveStatus = -1;

typedef struct {
    char *name;
} ShellReadonlyEntry;

static ShellReadonlyEntry *gShellReadonlyVars = NULL;
static size_t gShellReadonlyVarCount = 0;
static size_t gShellReadonlyVarCapacity = 0;
static char *gShellReadonlyErrorName = NULL;

typedef struct ShellAlias {
    char *name;
    char *value;
} ShellAlias;

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

static const char *shellResolveBashPath(void) {
    static char resolved[PATH_MAX];
    static bool initialised = false;
    if (initialised) {
        return resolved[0] ? resolved : "/bin/bash";
    }
    const char *env_bash = getenv("BASH");
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

    int override_value = shellParseEnvBool(getenv("PSCALSHELL_ASSOC_ARRAYS"));
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

    int override_value = shellParseEnvBool(getenv("PSCALSHELL_BIND_INTERACTIVE"));
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
    {"expand_aliases", false},
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

static void shellRuntimeSetCurrentCommandLocation(int line, int column) {
    gShellCurrentCommandLine = line;
    gShellCurrentCommandColumn = column;
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
    SHELL_RUNTIME_REDIR_HEREDOC
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
    int pipeline_index;
    bool is_pipeline_head;
    bool is_pipeline_tail;
    int line;
    int column;
} ShellCommand;

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
} ShellPipelineContext;

typedef struct {
    int last_status;
    ShellPipelineContext pipeline;
    pid_t shell_pgid;
    int tty_fd;
    bool job_control_enabled;
    bool job_control_initialized;
    bool errexit_enabled;
    bool errexit_pending;
    bool trap_enabled;
    bool local_scope_active;
    bool break_requested;
    bool continue_requested;
    int break_requested_levels;
    int continue_requested_levels;
    char **dir_stack;
    size_t dir_stack_count;
    size_t dir_stack_capacity;
    bool dir_stack_initialised;
} ShellRuntimeState;

typedef enum {
    SHELL_LOOP_KIND_WHILE,
    SHELL_LOOP_KIND_UNTIL,
    SHELL_LOOP_KIND_FOR,
    SHELL_LOOP_KIND_CFOR
} ShellLoopKind;

typedef struct {
    char *name;
    char *previous_value;
    bool had_previous;
    bool previous_was_array;
} ShellAssignmentBackup;

typedef struct {
    int target_fd;
    int saved_fd;
    bool saved_valid;
    bool was_closed;
} ShellExecRedirBackup;

static void shellRestoreExecRedirections(ShellExecRedirBackup *backups, size_t count);
static void shellFreeExecRedirBackups(ShellExecRedirBackup *backups, size_t count);
static void shellFreeRedirections(ShellCommand *cmd);

typedef struct {
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
    bool redirs_active;
    ShellRedirection *applied_redirs;
    size_t applied_redir_count;
    ShellExecRedirBackup *redir_backups;
    size_t redir_backup_count;
} ShellLoopFrame;

static ShellLoopFrame *gShellLoopStack = NULL;
static size_t gShellLoopStackSize = 0;
static size_t gShellLoopStackCapacity = 0;

static ShellRuntimeState gShellRuntime = {
    .last_status = 0,
    .pipeline = {0},
    .shell_pgid = 0,
    .tty_fd = -1,
    .job_control_enabled = false,
    .job_control_initialized = false,
    .errexit_enabled = false,
    .errexit_pending = false,
    .trap_enabled = false,
    .local_scope_active = false,
    .break_requested = false,
    .continue_requested = false,
    .break_requested_levels = 0,
    .continue_requested_levels = 0,
    .dir_stack = NULL,
    .dir_stack_count = 0,
    .dir_stack_capacity = 0,
    .dir_stack_initialised = false
};

static unsigned long gShellStatusVersion = 0;

static bool gShellExitRequested = false;
static volatile sig_atomic_t gShellExitOnSignalFlag = 0;
static bool gShellArithmeticErrorPending = false;
static VM *gShellCurrentVm = NULL;
static volatile sig_atomic_t gShellPendingSignals[NSIG] = {0};
static unsigned int gShellRandomSeed = 0;
static bool gShellRandomSeedInitialized = false;
static bool gShellInteractiveMode = false;

void shellRuntimeSetInteractive(bool interactive) {
    gShellInteractiveMode = interactive;
}

bool shellRuntimeIsInteractive(void) {
    return gShellInteractiveMode;
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
    if (frame->redirs_active) {
        shellRestoreExecRedirections(frame->redir_backups, frame->redir_backup_count);
    }
    if (frame->redir_backups) {
        shellFreeExecRedirBackups(frame->redir_backups, frame->redir_backup_count);
        frame->redir_backups = NULL;
        frame->redir_backup_count = 0;
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

typedef enum {
    SHELL_READ_LINE_OK,
    SHELL_READ_LINE_EOF,
    SHELL_READ_LINE_ERROR
} ShellReadLineResult;

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
        return true;
    }
    const char *start = expr;
    const char *end = expr + strlen(expr);
    return shellLoopEvaluateConditionRange(start, end, out_ready);
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
    frame.redirs_active = false;
    frame.applied_redirs = NULL;
    frame.applied_redir_count = 0;
    frame.redir_backups = NULL;
    frame.redir_backup_count = 0;
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
        gShellExitRequested = true;
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

static VM *shellSwapCurrentVm(VM *vm) {
    VM *previous = gShellCurrentVm;
    gShellCurrentVm = vm;
    return previous;
}

static void shellRestoreCurrentVm(VM *vm) {
    gShellCurrentVm = vm;
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

    if (!isatty(gShellRuntime.tty_fd)) {
        gShellRuntime.tty_fd = -1;
        return;
    }

    struct sigaction ignore_action;
    memset(&ignore_action, 0, sizeof(ignore_action));
    sigemptyset(&ignore_action.sa_mask);
    ignore_action.sa_handler = SIG_IGN;
    (void)sigaction(SIGTTIN, &ignore_action, NULL);
    (void)sigaction(SIGTTOU, &ignore_action, NULL);

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
}

static void shellEnsureJobControl(void) {
    shellInitJobControlState();

    if (gShellRuntime.tty_fd < 0) {
        gShellRuntime.job_control_enabled = false;
        return;
    }

    if (!isatty(gShellRuntime.tty_fd)) {
        gShellRuntime.job_control_enabled = false;
        gShellRuntime.tty_fd = -1;
        return;
    }

    pid_t pgid = getpgrp();
    if (pgid <= 0) {
        gShellRuntime.job_control_enabled = false;
        return;
    }

    gShellRuntime.shell_pgid = pgid;

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

typedef struct {
    pid_t pgid;
    pid_t *pids;
    size_t pid_count;
    bool running;
    bool stopped;
    bool disowned;
    int last_status;
    char *command;
} ShellJob;

static ShellJob *gShellJobs = NULL;
static size_t gShellJobCount = 0;

typedef struct {
    char **entries;
    size_t count;
    size_t capacity;
} ShellHistory;

static ShellHistory gShellHistory = {NULL, 0, 0};
static char *gShellArg0 = NULL;

typedef struct {
    char *name;
    char *parameter_metadata;
    ShellCompiledFunction *compiled;
} ShellFunctionEntry;

static ShellFunctionEntry *gShellFunctions = NULL;
static size_t gShellFunctionCount = 0;

typedef enum {
    SHELL_META_SUBSTITUTION_DOLLAR,
    SHELL_META_SUBSTITUTION_BACKTICK
} ShellMetaSubstitutionStyle;

typedef struct {
    ShellMetaSubstitutionStyle style;
    size_t span_length;
    char *command;
} ShellMetaSubstitution;
