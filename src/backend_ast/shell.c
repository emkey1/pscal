#include "backend_ast/builtin.h"
#include "shell_alias.h"
#include "shell_buffer.h"
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
#include <stddef.h>
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

static bool shellAssociativeArraysSupported(void) {
    if (gShellAssociativeArraySupport != -1) {
        return gShellAssociativeArraySupport == 1;
    }
    const char *bash_path = getenv("BASH");
    if (!bash_path || bash_path[0] == '\0') {
        bash_path = "/bin/bash";
    }
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
    const char *bash_path = getenv("BASH");
    if (!bash_path || bash_path[0] == '\0') {
        bash_path = "/bin/bash";
    }
    pid_t pid = fork();
    if (pid < 0) {
        gShellBindInteractiveStatus = 0;
        return false;
    }
    if (pid == 0) {
        execl(bash_path,
              bash_path,
              "--noprofile",
              "--norc",
              "-c",
              "bind 'set show-all-if-ambiguous on' >/dev/null 2>/dev/null",
              (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        gShellBindInteractiveStatus = 0;
        return false;
    }
    bool requires = !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
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

void shellRuntimeSetArg0(const char *name) {
    char *copy = NULL;
    if (name && *name) {
        copy = strdup(name);
        if (!copy) {
            return;
        }
    }
    free(gShellArg0);
    gShellArg0 = copy;
}

const char *shellRuntimeGetArg0(void) {
    return gShellArg0;
}

int shellRuntimeCurrentCommandLine(void) {
    return gShellCurrentCommandLine;
}

int shellRuntimeCurrentCommandColumn(void) {
    return gShellCurrentCommandColumn;
}

void shellRuntimeInitJobControl(void) {
    shellEnsureJobControl();
}

static bool shellIsRuntimeBuiltin(const char *name) {
    if (!name || !*name) {
        return false;
    }
    static const char *kBuiltins[] = {"cd",       "pwd",      "dirs",    "pushd",   "popd",    "exit",     "logout",   "exec",     "export",   "unset",    "setenv",
                                      "unsetenv", "set",      "declare", "typeset", "readonly", "umask",   "command", "trap",     "local",    "let",      "break",   "continue",
                                      "alias",   "bind",     "shopt",   "history", "jobs",     "disown",   "fg",       "finger",   "bg",       "wait",
                                      "builtin", "source",   "read",    "shift",   "return",   "help",     "type",    ":",       "unalias",
                                      "__shell_double_bracket"};

    size_t count = sizeof(kBuiltins) / sizeof(kBuiltins[0]);
    const char *canonical = shellBuiltinCanonicalName(name);
    for (size_t i = 0; i < count; ++i) {
        if (strcasecmp(name, kBuiltins[i]) == 0 ||
            (canonical && strcasecmp(canonical, kBuiltins[i]) == 0)) {
            return true;
        }
    }
    return false;
}

static bool shellInvokeFunction(VM *vm, ShellCommand *cmd) {
    if (!cmd || cmd->argc == 0) {
        return false;
    }
    const char *name = cmd->argv[0];
    ShellFunctionEntry *entry = shellFindFunctionEntry(name);
    if (!entry || !entry->compiled) {
        return false;
    }
    char **saved_params = gParamValues;
    int saved_count = gParamCount;
    bool saved_owned = gShellPositionalOwned;
    char **function_params = NULL;
    int function_count = (cmd->argc > 1) ? (int)cmd->argc - 1 : 0;
    if (function_count > 0) {
        function_params = (char **)calloc((size_t)function_count, sizeof(char *));
        if (!function_params) {
            runtimeError(vm, "%s: out of memory", name);
            shellUpdateStatus(1);
            return true;
        }
        bool ok = true;
        for (int i = 0; i < function_count; ++i) {
            const char *arg = cmd->argv[i + 1] ? cmd->argv[i + 1] : "";
            function_params[i] = strdup(arg);
            if (!function_params[i]) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            for (int i = 0; i < function_count; ++i) {
                free(function_params[i]);
            }
            free(function_params);
            runtimeError(vm, "%s: out of memory", name);
            shellUpdateStatus(1);
            return true;
        }
        gParamValues = function_params;
        gParamCount = function_count;
        gShellPositionalOwned = true;
    } else {
        gParamValues = NULL;
        gParamCount = 0;
        gShellPositionalOwned = false;
    }
    VM function_vm;
    initVM(&function_vm);
    InterpretResult result = interpretBytecode(&function_vm, &entry->compiled->chunk,
                                               globalSymbols, constGlobalSymbols, procedure_table, 0);
    if (result != INTERPRET_OK) {
        shellUpdateStatus(1);
    } else {
        shellUpdateStatus(shellRuntimeLastStatus());
    }
    freeVM(&function_vm);
    if (gShellPositionalOwned) {
        shellFreeOwnedPositionalParameters();
    } else {
        gParamValues = NULL;
        gParamCount = 0;
    }
    gParamValues = saved_params;
    gParamCount = saved_count;
    gShellPositionalOwned = saved_owned;
    return true;
}

static bool shellInvokeBuiltin(VM *vm, ShellCommand *cmd) {
    if (!cmd || cmd->argc == 0) {
        return false;
    }
    if (shellInvokeFunction(vm, cmd)) {
        return true;
    }
    const char *name = cmd->argv[0];
    if (!name || !shellIsRuntimeBuiltin(name)) {
        return false;
    }
    const char *canonical = shellBuiltinCanonicalName(name);
    VmBuiltinFn handler = getVmBuiltinHandler(canonical);
    if (!handler && canonical && canonical != name) {
        handler = getVmBuiltinHandler(name);
    }
    if (!handler) {
        const char *label = canonical ? canonical : name;
        if (vm) {
            runtimeError(vm, "shell builtin '%s': not available", label ? label : "<builtin>");
        } else {
            fprintf(stderr, "exsh: shell builtin '%s' is not available\n", label ? label : "<builtin>");
        }
        shellUpdateStatus(127);
        return true;
    }
    int arg_count = (cmd->argc > 0) ? (int)cmd->argc - 1 : 0;
    Value *args = NULL;
    int previous_line = gShellCurrentCommandLine;
    int previous_column = gShellCurrentCommandColumn;
    shellRuntimeSetCurrentCommandLocation(cmd->line, cmd->column);
    if (arg_count > 0) {
        args = calloc((size_t)arg_count, sizeof(Value));
        if (!args) {
            runtimeError(vm, "shell builtin '%s': out of memory", name);
            shellUpdateStatus(1);
            shellRuntimeSetCurrentCommandLocation(previous_line, previous_column);
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
    shellRuntimeSetCurrentCommandLocation(previous_line, previous_column);
    return true;
}

static int shellStatusFromWait(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    if (WIFSTOPPED(status)) {
        return 128 + WSTOPSIG(status);
    }
    return status;
}

static void shellFreeJob(ShellJob *job) {
    if (!job) {
        return;
    }
    free(job->command);
    job->command = NULL;
    free(job->pids);
    job->pids = NULL;
    job->pid_count = 0;
    job->pgid = -1;
    job->running = false;
    job->stopped = false;
    job->disowned = false;
    job->last_status = 0;
}

static void shellRemoveJobAt(size_t index) {
    if (index >= gShellJobCount) {
        return;
    }
    ShellJob *job = &gShellJobs[index];
    shellFreeJob(job);
    if (index + 1 < gShellJobCount) {
        gShellJobs[index] = gShellJobs[gShellJobCount - 1];
    }
    gShellJobCount--;
    if (gShellJobCount == 0 && gShellJobs) {
        free(gShellJobs);
        gShellJobs = NULL;
    }
}

static ShellJob *shellRegisterJob(pid_t pgid, const pid_t *pids, size_t pid_count, const ShellCommand *cmd) {
    if (pgid <= 0 || !pids || pid_count == 0 || !cmd) {
        return NULL;
    }

    pid_t *pid_copy = malloc(sizeof(pid_t) * pid_count);
    if (!pid_copy) {
        return NULL;
    }
    memcpy(pid_copy, pids, sizeof(pid_t) * pid_count);

    char *summary = NULL;
    if (cmd->argc > 0 && cmd->argv) {
        size_t len = 0;
        for (size_t i = 0; i < cmd->argc; ++i) {
            len += strlen(cmd->argv[i]) + 1;
        }
        summary = malloc(len + 1);
        if (summary) {
            summary[0] = '\0';
            for (size_t i = 0; i < cmd->argc; ++i) {
                strcat(summary, cmd->argv[i]);
                if (i + 1 < cmd->argc) {
                    strcat(summary, " ");
                }
            }
        }
    }

    ShellJob *new_jobs = realloc(gShellJobs, sizeof(ShellJob) * (gShellJobCount + 1));
    if (!new_jobs) {
        free(summary);
        free(pid_copy);
        return NULL;
    }

    gShellJobs = new_jobs;
    ShellJob *job = &gShellJobs[gShellJobCount++];
    job->pgid = pgid;
    job->pids = pid_copy;
    job->pid_count = pid_count;
    job->running = true;
    job->stopped = false;
    job->disowned = false;
    job->last_status = 0;
    job->command = summary;
    return job;
}

static int shellCollectJobs(void) {
    int reaped = 0;
    for (size_t i = 0; i < gShellJobCount;) {
        ShellJob *job = &gShellJobs[i];
        bool job_active = false;

        if (!job->pids || job->pid_count == 0) {
            shellRemoveJobAt(i);
            reaped++;
            continue;
        }

        for (size_t j = 0; j < job->pid_count; ++j) {
            pid_t pid = job->pids[j];
            if (pid <= 0) {
                continue;
            }
            int status = 0;
            pid_t res = waitpid(pid, &status, WNOHANG | WUNTRACED | WCONTINUED);
            if (res == 0) {
                job_active = true;
                continue;
            }
            if (res < 0) {
                if (errno == EINTR) {
                    job_active = true;
                    continue;
                }
                if (errno == ECHILD) {
                    job->pids[j] = -1;
                }
                continue;
            }
            if (WIFSTOPPED(status)) {
                job->stopped = true;
                job->running = false;
                job_active = true;
            } else if (WIFCONTINUED(status)) {
                job->stopped = false;
                job->running = true;
                job_active = true;
            } else {
                job->last_status = shellStatusFromWait(status);
                job->pids[j] = -1;
            }
        }

        if (!job->stopped) {
            for (size_t j = 0; j < job->pid_count; ++j) {
                if (job->pids[j] > 0) {
                    job_active = true;
                    job->running = true;
                    break;
                }
            }
        }

        bool all_done = true;
        for (size_t j = 0; j < job->pid_count; ++j) {
            if (job->pids[j] > 0) {
                all_done = false;
                break;
            }
        }

        if (all_done) {
            int status = job->last_status;
            bool disowned = job->disowned;
            if (!disowned) {
                shellUpdateStatus(status);
            }
            shellRemoveJobAt(i);
            reaped++;
            continue;
        }

        if (!job_active && !job->stopped) {
            job->running = true;
        }

        ++i;
    }
    return reaped;
}

static size_t shellJobVisibleCount(void) {
    size_t count = 0;
    for (size_t i = 0; i < gShellJobCount; ++i) {
        ShellJob *job = &gShellJobs[i];
        if (!job->disowned) {
            ++count;
        }
    }
    return count;
}

static bool shellJobFindVisibleIndex(size_t visible_number, size_t *out_index) {
    if (!out_index || visible_number == 0) {
        return false;
    }
    size_t visible = 0;
    for (size_t i = 0; i < gShellJobCount; ++i) {
        ShellJob *job = &gShellJobs[i];
        if (job->disowned) {
            continue;
        }
        ++visible;
        if (visible == visible_number) {
            *out_index = i;
            return true;
        }
    }
    return false;
}

static bool shellParseJobSpecifier(VM *vm, const char *name, Value spec, size_t *out_index) {
    if (spec.type == TYPE_STRING && spec.s_val) {
        const char *text = spec.s_val;
        if (text[0] == '%') {
            text++;
        }
        if (*text == '\0') {
            runtimeError(vm, "%s: invalid job spec", name);
            return false;
        }
        char *end = NULL;
        long index = strtol(text, &end, 10);
        if (*end != '\0' || index <= 0) {
            runtimeError(vm, "%s: invalid job '%s'", name, spec.s_val);
            return false;
        }
        if (!shellJobFindVisibleIndex((size_t)index, out_index)) {
            runtimeError(vm, "%s: invalid job '%s'", name, spec.s_val);
            return false;
        }
        return true;
    }

    if (IS_INTLIKE(spec)) {
        long index = (long)AS_INTEGER(spec);
        if (index <= 0) {
            runtimeError(vm, "%s: invalid job index", name);
            return false;
        }
        if (!shellJobFindVisibleIndex((size_t)index, out_index)) {
            runtimeError(vm, "%s: invalid job index", name);
            return false;
        }
        return true;
    }

    runtimeError(vm, "%s: job spec must be a string or integer", name);
    return false;
}

static bool shellResolveJobIndex(VM *vm, const char *name, int arg_count, Value *args, size_t *out_index) {
    size_t visible_count = shellJobVisibleCount();
    if (arg_count == 0) {
        if (visible_count == 0) {
            runtimeError(vm, "%s: no current job", name);
            return false;
        }
        for (size_t i = gShellJobCount; i > 0; --i) {
            ShellJob *job = &gShellJobs[i - 1];
            if (!job->disowned) {
                *out_index = i - 1;
                return true;
            }
        }
        runtimeError(vm, "%s: no current job", name);
        return false;
    }
    if (arg_count > 1) {
        runtimeError(vm, "%s: too many arguments", name);
        return false;
    }

    return shellParseJobSpecifier(vm, name, args[0], out_index);
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
            } else if (strcmp(key, "line") == 0) {
                cmd->line = atoi(value);
            } else if (strcmp(key, "col") == 0) {
                cmd->column = atoi(value);
            }
        }
        if (!next) {
            break;
        }
        cursor = next + 1;
    }
    free(copy);
}

static bool shellAddArg(ShellCommand *cmd, const char *arg, bool *saw_command_word) {
    if (!cmd || !arg) {
        return false;
    }
    const char *text = NULL;
    const char *meta = NULL;
    size_t meta_len = 0;
    uint8_t flags = 0;
    if (!shellDecodeWordSpec(arg, &text, &flags, &meta, &meta_len)) {
        return false;
    }
    bool *quoted_map = NULL;
    size_t quoted_len = 0;
    bool zero_array = false;
    char *expanded = shellExpandWord(text,
                                     flags,
                                     meta,
                                     meta_len,
                                     &quoted_map,
                                     &quoted_len,
                                     &zero_array);
    if (!expanded) {
        return false;
    }
    if (saw_command_word && !*saw_command_word) {
        if ((flags & SHELL_WORD_FLAG_ASSIGNMENT) && shellLooksLikeAssignment(expanded)) {
            bool is_array_literal = shellAssignmentIsArrayLiteral(text, flags);
            if (!shellCommandAppendAssignmentOwned(cmd, expanded, is_array_literal)) {
                free(quoted_map);
                return false;
            }
            free(quoted_map);
            return true;
        }
    } else if ((flags & SHELL_WORD_FLAG_ASSIGNMENT) && shellLooksLikeAssignment(expanded)) {
        if (!shellCommandAppendArgOwned(cmd, expanded)) {
            free(quoted_map);
            return false;
        }
        if (saw_command_word) {
            *saw_command_word = true;
        }
        free(quoted_map);
        return true;
    }
    char **fields = NULL;
    size_t field_count = 0;
    if (!shellSplitExpandedWord(expanded,
                                flags,
                                quoted_map,
                                quoted_len,
                                zero_array,
                                &fields,
                                &field_count)) {
        free(expanded);
        free(quoted_map);
        return false;
    }
    free(expanded);
    free(quoted_map);
    if (field_count == 0) {
        free(fields);
        return true;
    }

    for (size_t i = 0; i < field_count; ++i) {
        char *field = fields[i];
        if (!field) {
            continue;
        }
        bool appended = false;
        if (shellWordShouldGlob(flags, field)) {
            glob_t glob_result;
            int glob_status = glob(field, 0, NULL, &glob_result);
            if (glob_status == 0) {
                size_t original_argc = cmd->argc;
                bool ok = true;
                for (size_t g = 0; g < glob_result.gl_pathc; ++g) {
                    char *dup = strdup(glob_result.gl_pathv[g]);
                    if (!dup) {
                        ok = false;
                        break;
                    }
                    if (!shellCommandAppendArgOwned(cmd, dup)) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) {
                    while (cmd->argc > original_argc) {
                        free(cmd->argv[cmd->argc - 1]);
                        cmd->argc--;
                    }
                    if (cmd->argv) {
                        cmd->argv[cmd->argc] = NULL;
                    }
                    globfree(&glob_result);
                    for (size_t j = i; j < field_count; ++j) {
                        if (fields[j]) {
                            free(fields[j]);
                        }
                    }
                    free(fields);
                    free(field);
                    return false;
                }
                globfree(&glob_result);
                appended = true;
            } else if (glob_status != GLOB_NOMATCH) {
                fprintf(stderr, "exsh: glob failed for '%s'\n", field);
            }
        }
        if (!appended) {
            if (!shellCommandAppendArgOwned(cmd, field)) {
                for (size_t j = i + 1; j < field_count; ++j) {
                    if (fields[j]) {
                        free(fields[j]);
                    }
                }
                free(fields);
                return false;
            }
            appended = true;
        } else {
            free(field);
        }
        fields[i] = NULL;
    }
    free(fields);
    if (saw_command_word) {
        *saw_command_word = true;
    }
    return true;
}

static int decodeHexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    return -1;
}

static char *decodeHexString(const char *hex, size_t *out_len) {
    if (out_len) {
        *out_len = 0;
    }
    if (!hex || *hex == '\0') {
        if (out_len) {
            *out_len = 0;
        }
        return strdup("");
    }
    size_t len = strlen(hex);
    if ((len & 1) != 0) {
        return NULL;
    }
    size_t decoded_len = len / 2;
    char *decoded = (char *)malloc(decoded_len + 1);
    if (!decoded) {
        return NULL;
    }
    for (size_t i = 0; i < decoded_len; ++i) {
        int high = decodeHexDigit(hex[i * 2]);
        int low = decodeHexDigit(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            free(decoded);
            return NULL;
        }
        decoded[i] = (char)((high << 4) | low);
    }
    decoded[decoded_len] = '\0';
    if (out_len) {
        *out_len = decoded_len;
    }
    return decoded;
}

static bool shellAddRedirection(ShellCommand *cmd, const char *spec) {
    if (!cmd || !spec) {
        return false;
    }
    if (strncmp(spec, "redir:", 6) != 0) {
        return false;
    }
    const char *payload = spec + 6;
    char *copy = strdup(payload);
    if (!copy) {
        return false;
    }

    const char *fd_text = "";
    const char *type_text = "";
    const char *word_hex = "";
    const char *here_hex = "";
    bool here_quoted = false;

    char *cursor = copy;
    while (cursor && *cursor) {
        char *next = strchr(cursor, ';');
        if (next) {
            *next = '\0';
        }
        char *eq = strchr(cursor, '=');
        const char *key = cursor;
        const char *value = "";
        if (eq) {
            *eq = '\0';
            value = eq + 1;
        }
        if (strcmp(key, "fd") == 0) {
            fd_text = value;
        } else if (strcmp(key, "type") == 0) {
            type_text = value;
        } else if (strcmp(key, "word") == 0) {
            word_hex = value;
        } else if (strcmp(key, "here") == 0) {
            here_hex = value;
        } else if (strcmp(key, "hereq") == 0) {
            shellParseBool(value, &here_quoted);
        }
        if (!next) {
            break;
        }
        cursor = next + 1;
    }

    if (!type_text || *type_text == '\0') {
        free(copy);
        return false;
    }

    int fd = -1;
    if (fd_text && *fd_text) {
        fd = atoi(fd_text);
    } else if (strcmp(type_text, "<") == 0 || strcmp(type_text, "<<") == 0 || strcmp(type_text, "<<<") == 0 || strcmp(type_text, "<&") == 0 || strcmp(type_text, "<>") == 0) {
        fd = STDIN_FILENO;
    } else {
        fd = STDOUT_FILENO;
    }

    ShellRedirection redir;
    memset(&redir, 0, sizeof(redir));
    redir.fd = fd;

    char *word_encoded = decodeHexString(word_hex, NULL);
    const char *target_spec = (word_encoded && *word_encoded) ? word_encoded : "";
    const char *target_text = NULL;
    const char *target_meta = NULL;
    size_t target_meta_len = 0;
    uint8_t target_flags = 0;
    if (target_spec && *target_spec) {
        shellDecodeWordSpec(target_spec, &target_text, &target_flags, &target_meta, &target_meta_len);
    }

    char *expanded_target = NULL;
    if (strcmp(type_text, "<<") != 0) {
        if (!target_spec || *target_spec == '\0') {
            free(word_encoded);
            free(copy);
            return false;
        }
        expanded_target = shellExpandWord(target_text,
                                          target_flags,
                                          target_meta,
                                          target_meta_len,
                                          NULL,
                                          NULL,
                                          NULL);
        if (!expanded_target) {
            free(word_encoded);
            free(copy);
            return false;
        }
    }

    if (strcmp(type_text, "<") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_RDONLY;
        redir.mode = 0;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, ">") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_WRONLY | O_CREAT | O_TRUNC;
        redir.mode = 0666;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, ">>") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_WRONLY | O_CREAT | O_APPEND;
        redir.mode = 0666;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, "<>") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_RDWR | O_CREAT;
        redir.mode = 0666;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, ">|") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_WRONLY | O_CREAT | O_TRUNC;
        redir.mode = 0666;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, "<&") == 0 || strcmp(type_text, ">&") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_DUP;
        if (!expanded_target) {
            free(word_encoded);
            free(copy);
            return false;
        }
        if (strcmp(expanded_target, "-") == 0) {
            redir.close_target = true;
        } else {
            char *endptr = NULL;
            errno = 0;
            long value = strtol(expanded_target, &endptr, 10);
            if (errno != 0 || !endptr || *endptr != '\0') {
                free(expanded_target);
                free(copy);
                return false;
            }
            redir.dup_target_fd = (int)value;
        }
        free(expanded_target);
        expanded_target = NULL;
    } else if (strcmp(type_text, "<<") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_HEREDOC;
        size_t body_len = 0;
        char *decoded = decodeHexString(here_hex ? here_hex : "", &body_len);
        if (!decoded) {
            free(word_encoded);
            free(copy);
            return false;
        }
        char *expanded = shellExpandHereDocument(decoded, here_quoted);
        if (!expanded) {
            expanded = decoded;
        } else {
            free(decoded);
        }
        redir.here_doc = expanded;
        redir.here_doc_length = expanded ? strlen(expanded) : 0;
        redir.here_doc_quoted = here_quoted;
    } else if (strcmp(type_text, "<<<") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_HEREDOC;
        if (!expanded_target) {
            free(word_encoded);
            free(copy);
            return false;
        }
        size_t len = strlen(expanded_target);
        char *body = (char *)malloc(len + 2);
        if (!body) {
            free(expanded_target);
            free(word_encoded);
            free(copy);
            return false;
        }
        memcpy(body, expanded_target, len);
        body[len] = '\n';
        body[len + 1] = '\0';
        redir.here_doc = body;
        redir.here_doc_length = len + 1;
        redir.here_doc_quoted = false;
        free(expanded_target);
        expanded_target = NULL;
    } else {
        free(expanded_target);
        free(word_encoded);
        free(copy);
        return false;
    }

    ShellRedirection *new_redirs = realloc(cmd->redirs, sizeof(ShellRedirection) * (cmd->redir_count + 1));
    if (!new_redirs) {
        free(expanded_target);
        free(redir.path);
        free(redir.here_doc);
        free(word_encoded);
        free(copy);
        return false;
    }
    cmd->redirs = new_redirs;
    cmd->redirs[cmd->redir_count++] = redir;
    free(word_encoded);
    free(copy);
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
    SHELL_REDIR_OP_HEREDOC
} ShellRuntimeRedirOpType;

typedef struct {
    ShellRuntimeRedirOpType type;
    int target_fd;
    int source_fd;
    int write_fd;
    const char *here_body;
    size_t here_length;
    bool close_target;
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
                if (op->type == SHELL_REDIR_OP_HEREDOC && op->write_fd >= 0) {
                    close(op->write_fd);
                    op->write_fd = -1;
                }
            }

            for (size_t i = 0; i < op_count; ++i) {
                ShellRuntimeRedirOp *op = &ops[i];
                switch (op->type) {
                    case SHELL_REDIR_OP_OPEN:
                    case SHELL_REDIR_OP_HEREDOC:
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
                if ((op->type == SHELL_REDIR_OP_OPEN || op->type == SHELL_REDIR_OP_HEREDOC) &&
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
            } else if (op->type == SHELL_REDIR_OP_HEREDOC) {
                if (op->source_fd >= 0) {
                    close(op->source_fd);
                    op->source_fd = -1;
                }
                if (op->write_fd >= 0) {
                    const char *body = op->here_body ? op->here_body : "";
                    size_t remaining = op->here_length;
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
        if (ops[j].type == SHELL_REDIR_OP_OPEN || ops[j].type == SHELL_REDIR_OP_HEREDOC) {
            if (ops[j].source_fd >= 0) {
                close(ops[j].source_fd);
                ops[j].source_fd = -1;
            }
        }
        if (ops[j].type == SHELL_REDIR_OP_HEREDOC && ops[j].write_fd >= 0) {
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
    do {
        waited = waitpid(pid, &status, options);
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
    int dup_fd = dup(target_fd);
    if (dup_fd >= 0) {
        backup.saved_fd = dup_fd;
        backup.saved_valid = true;
        fcntl(dup_fd, F_SETFD, FD_CLOEXEC);
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

    for (size_t i = 0; i < cmd->redir_count; ++i) {
        const ShellRedirection *redir = &cmd->redirs[i];
        int target_fd = redir->fd;
        if (!shellEnsureExecRedirBackup(target_fd, &backups, &backup_count, &backup_capacity)) {
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
                break;
            }
            case SHELL_RUNTIME_REDIR_DUP: {
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

Value vmBuiltinShellSubshell(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    shellResetPipeline();
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
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
                    }
                }
            }
            free(copy);
        }
    }
    if (kind != SHELL_LOOP_KIND_FOR && until_flag) {
        kind = SHELL_LOOP_KIND_UNTIL;
    }

    bool parent_skip = shellLoopSkipActive();
    ShellLoopFrame *frame = shellLoopPushFrame(kind);
    if (!frame) {
        runtimeError(vm, "shell loop: out of memory");
        shellRestoreCurrentVm(previous_vm);
        shellUpdateStatus(1);
        return result;
    }
    frame->skip_body = parent_skip;
    frame->break_pending = false;
    frame->continue_pending = false;

    ShellCommand redir_cmd;
    memset(&redir_cmd, 0, sizeof(redir_cmd));
    ShellExecRedirBackup *redir_backups = NULL;
    size_t redir_backup_count = 0;

    bool ok = true;

    int payload_total = (arg_count > 0) ? arg_count - 1 : 0;
    if ((size_t)payload_total < redir_count) {
        runtimeError(vm, "shell loop: redirection metadata mismatch");
        ok = false;
        redir_count = (payload_total >= 0) ? (size_t)payload_total : 0;
    }
    int payload_without_redirs = payload_total - (int)redir_count;
    if (payload_without_redirs < 0) {
        payload_without_redirs = 0;
    }
    int redir_start_index = arg_count - (int)redir_count;
    if (redir_start_index < 1) {
        redir_start_index = arg_count;
    }

    if (kind == SHELL_LOOP_KIND_FOR) {
        if (payload_without_redirs < 1 || args[1].type != TYPE_STRING || !args[1].s_val) {
            runtimeError(vm, "shell loop: expected iterator name");
            ok = false;
        } else {
            const char *spec = args[1].s_val;
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

        size_t value_start_index = 2;
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
                        int glob_status = glob(field_value, 0, NULL, &glob_result);
                        if (glob_status == 0) {
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
            const char *init_text = (args[1].type == TYPE_STRING && args[1].s_val) ? args[1].s_val : "";
            const char *cond_text = (args[2].type == TYPE_STRING && args[2].s_val) ? args[2].s_val : "";
            const char *update_text = (args[3].type == TYPE_STRING && args[3].s_val) ? args[3].s_val : "";
            frame->cfor_init = strdup(init_text);
            frame->cfor_condition = strdup(cond_text);
            frame->cfor_update = strdup(update_text);
            if (!frame->cfor_init || !frame->cfor_condition || !frame->cfor_update) {
                ok = false;
            } else if (!shellLoopExecuteCForInitializer(frame)) {
                ok = false;
            }
        }
    } else {
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
                frame->applied_redirs = redir_cmd.redirs;
                frame->applied_redir_count = redir_cmd.redir_count;
                redir_backups = NULL;
                redir_backup_count = 0;
                redir_cmd.redirs = NULL;
                redir_cmd.redir_count = 0;
            }
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

    shellResetPipeline();
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

Value vmBuiltinShellIf(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    shellResetPipeline();
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
    shellUpdateStatus(1);

cleanup:
    shellRestoreCurrentVm(previous_vm);
    return result;
}

Value vmBuiltinShellCaseClause(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
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
        shellUpdateStatus(1);
    }
    shellRestoreCurrentVm(previous_vm);
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
        *out_result = (fd >= 0 && isatty((int)fd));
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
    if (!shellDirectoryStackEnsureInitialised(vm, "cd")) {
        return makeVoid();
    }

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
            } else {
                old_cwd = NULL;
            }
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
    printf("%s\n", cwd);
    shellUpdateStatus(0);
    return makeVoid();
}

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
        gShellExitRequested = true;
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

    bool exit_requested = false;
    int status = shellRunSource(script, "<eval>", &opts, &exit_requested);
    free(script);

    if (exit_requested) {
        gShellExitRequested = true;
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
    gShellExitRequested = true;
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
    if (vm) {
        vm->exit_requested = true;
        vm->current_builtin_name = "return";
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

static const char *shellReadResolveIFS(void) {
    const char *ifs = getenv("IFS");
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
            fputs(prompt, stdout);
            fflush(stdout);
        }
        read_result = shellReadLineFromStream(stdin, &line, &line_length);
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

    if (ok && !interactive) {
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

    /* Match bash behavior: bind returns failure when line editing is unavailable. */
    int status = ok ? 0 : 1;
    if (ok && !interactive && shellBindRequiresInteractive()) {
        status = 1;
    }
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

Value vmBuiltinShellDeclare(VM *vm, int arg_count, Value *args) {
    bool ok = true;
    bool associative = false;
    bool global_scope = false;
    bool mark_readonly = false;
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
                    runtimeError(vm, "declare: -%c: invalid option", opt);
                    runtimeError(vm,
                                 "declare: usage: declare [-afFirtx] [-p] [name[=value] ...]");
                    ok = false;
                    break;
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
                if (!shellArrayRegistryInitializeAssociative(spec)) {
                    runtimeError(vm, "declare: unable to initialise '%s'", spec);
                    ok = false;
                } else {
                    if (setenv(spec, "", 1) != 0) {
                        runtimeError(vm, "declare: unable to set '%s'", spec);
                        ok = false;
                    }
                }
            } else {
                if (!shellSetTrackedVariable(spec, "", false)) {
                    const char *readonly = shellReadonlyGetErrorName();
                    if (errno == EPERM && readonly && *readonly) {
                        runtimeError(vm, "declare: %s: readonly variable", readonly);
                    } else if (errno != 0) {
                        runtimeError(vm, "declare: unable to set '%s': %s", spec, strerror(errno));
                    } else {
                        runtimeError(vm, "declare: unable to set '%s'", spec);
                    }
                    ok = false;
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
        if (associative) {
            if (!shellArrayRegistryInitializeAssociative(name)) {
                runtimeError(vm, "declare: unable to initialise '%s'", name);
                free(name);
                ok = false;
                break;
            }
            if (!shellSetTrackedVariable(name, value_text, true)) {
                const char *readonly = shellReadonlyGetErrorName();
                if (errno == EPERM && readonly && *readonly) {
                    runtimeError(vm, "declare: %s: readonly variable", readonly);
                } else if (errno != 0) {
                    runtimeError(vm, "declare: unable to set '%s': %s", name, strerror(errno));
                } else {
                    runtimeError(vm, "declare: unable to set '%s'", name);
                }
                free(name);
                ok = false;
                break;
            }
        } else {
            if (!shellSetTrackedVariable(name, value_text, false)) {
                const char *readonly = shellReadonlyGetErrorName();
                if (errno == EPERM && readonly && *readonly) {
                    runtimeError(vm, "declare: %s: readonly variable", readonly);
                } else if (errno != 0) {
                    runtimeError(vm, "declare: unable to set '%s': %s", name, strerror(errno));
                } else {
                    runtimeError(vm, "declare: unable to set '%s'", name);
                }
                free(name);
                ok = false;
                break;
            }
        }
        if (ok && mark_readonly) {
            if (!shellReadonlyAdd(name)) {
                runtimeError(vm, "declare: out of memory");
                free(name);
                ok = false;
                break;
            }
        }
        free(name);
    }

    (void)global_scope;

    shellUpdateStatus(ok ? 0 : 1);
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
            const char *existing = getenv(text);
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

    const char *env_value = path_env ? path_env : getenv("PATH");
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

    const char *alias_value = NULL;
    if (shellAliasLookup(name, &alias_value)) {
        const char *value = alias_value ? alias_value : "";
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

    if (shellIsRuntimeBuiltin(name)) {
        const char *canonical = shellBuiltinCanonicalName(name);
        if (!shellCommandAppendResult(&results, &count, SHELL_COMMAND_RESULT_BUILTIN, canonical)) {
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
            const char *value = getenv(text);
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
        runtimeError(vm, "command: execution without -v or -V is not implemented");
        shellUpdateStatus(1);
        return makeVoid();
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
        } else if (strcmp(token, "+e") == 0) {
            gShellRuntime.errexit_enabled = false;
            gShellRuntime.errexit_pending = false;
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
                }
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
    bool ok = true;
    if (arg_count == 0) {
        gShellRuntime.trap_enabled = false;
    } else {
        for (int i = 0; i < arg_count; ++i) {
            if (args[i].type != TYPE_STRING || !args[i].s_val) {
                runtimeError(vm, "trap: expected string arguments");
                ok = false;
                break;
            }
        }
        if (ok) {
            gShellRuntime.trap_enabled = true;
        }
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

Value vmBuiltinShellJobs(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    shellCollectJobs();
    size_t visible_index = 0;
    for (size_t i = 0; i < gShellJobCount; ++i) {
        ShellJob *job = &gShellJobs[i];
        if (job->disowned) {
            continue;
        }
        ++visible_index;
        const char *state = job->stopped ? "Stopped" : "Running";
        const char *command = job->command ? job->command : "";
        printf("[%zu] %s %s\n", visible_index, state, command);
    }
    fflush(stdout);
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

Value vmBuiltinShellFg(VM *vm, int arg_count, Value *args) {
    shellCollectJobs();
    size_t index = 0;
    if (!shellResolveJobIndex(vm, "fg", arg_count, args, &index)) {
        shellUpdateStatus(1);
        return makeVoid();
    }
    ShellJob *job = &gShellJobs[index];
    shellEnsureJobControl();
    bool job_control = gShellRuntime.job_control_enabled && job->pgid > 0;
    if (job_control) {
        shellJobControlSetForeground(job->pgid);
    }
    if (job->pgid > 0) {
        if (kill(-job->pgid, SIGCONT) != 0 && errno != ESRCH) {
            /* ignore */
        }
    } else {
        for (size_t i = 0; i < job->pid_count; ++i) {
            pid_t pid = job->pids[i];
            if (pid > 0) {
                kill(pid, SIGCONT);
            }
        }
    }
    job->stopped = false;
    job->running = true;
    int final_status = job->last_status;
    for (size_t i = 0; i < job->pid_count; ++i) {
        pid_t pid = job->pids[i];
        if (pid <= 0) {
            continue;
        }
        int status = 0;
        pid_t res;
        do {
            res = waitpid(pid, &status, WUNTRACED);
        } while (res < 0 && errno == EINTR);
        if (res < 0) {
            continue;
        }
        if (WIFSTOPPED(status)) {
            job->stopped = true;
            job->running = false;
            job->last_status = shellStatusFromWait(status);
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
    size_t index = 0;
    if (!shellResolveJobIndex(vm, "bg", arg_count, args, &index)) {
        shellUpdateStatus(1);
        return makeVoid();
    }
    ShellJob *job = &gShellJobs[index];
    if (job->pgid > 0) {
        if (kill(-job->pgid, SIGCONT) != 0 && errno != ESRCH) {
            /* ignore */
        }
    } else {
        for (size_t i = 0; i < job->pid_count; ++i) {
            pid_t pid = job->pids[i];
            if (pid > 0) {
                kill(pid, SIGCONT);
            }
        }
    }
    job->stopped = false;
    job->running = true;
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellWait(VM *vm, int arg_count, Value *args) {
    shellCollectJobs();
    size_t visible_count = shellJobVisibleCount();
    if (visible_count == 0 && arg_count == 0) {
        shellUpdateStatus(0);
        return makeVoid();
    }
    size_t index = 0;
    if (!shellResolveJobIndex(vm, "wait", arg_count, args, &index)) {
        shellUpdateStatus(1);
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
            res = waitpid(pid, &status, 0);
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

Value vmBuiltinShellBuiltin(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || args[0].type != TYPE_STRING || !args[0].s_val || args[0].s_val[0] == '\0') {
        runtimeError(vm, "builtin: expected VM builtin name");
        shellUpdateStatus(1);
        return makeVoid();
    }

    const char *name = args[0].s_val;
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

Value vmHostShellLastStatus(VM *vm) {
    (void)vm;
    return makeInt(gShellRuntime.last_status);
}

Value vmHostShellLoopIsReady(VM *vm) {
    (void)vm;
    shellRuntimeProcessPendingSignals();
    ShellLoopFrame *frame = shellLoopTop();
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

    if (frame->break_pending) {
        frame->break_pending = false;
        frame->continue_pending = false;
        frame->skip_body = false;
        frame->for_active = false;
        shellResetPipeline();
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
                shellResetPipeline();
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
            shellResetPipeline();
            return makeBoolean(false);
        }
        bool condition_ready = false;
        if (!shellLoopEvaluateCForCondition(frame, &condition_ready)) {
            shellUpdateStatus(1);
            frame->skip_body = false;
            frame->break_pending = true;
            shellResetPipeline();
            return makeBoolean(false);
        }
        should_continue = condition_ready;
    }

    frame->skip_body = false;
    shellResetPipeline();
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
