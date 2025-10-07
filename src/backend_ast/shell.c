#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "shell/word_encoding.h"
#include "shell/quote_markers.h"
#include "shell/function.h"
#include "shell/builtins.h"
#include "shell/runner.h"
#include "vm/vm.h"
#include "Pascal/globals.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <regex.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
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

static bool shellIsValidEnvName(const char *name);
static void shellExportPrintEnvironment(void);
static bool shellParseReturnStatus(const char *text, int *out_status);
static bool shellArithmeticParseValueString(const char *text, long long *out_value);
static char *shellEvaluateArithmetic(const char *expr, bool *out_error);
static void shellMarkArithmeticError(void);

static bool gShellPositionalOwned = false;

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
static char *shellExpandParameter(const char *input, size_t *out_consumed);

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
                char *expanded = shellExpandParameter(pattern + i + 1, &consumed);
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
    .continue_requested_levels = 0
};

static unsigned long gShellStatusVersion = 0;

static bool gShellExitRequested = false;
static volatile sig_atomic_t gShellExitOnSignalFlag = 0;
static bool gShellArithmeticErrorPending = false;
static VM *gShellCurrentVm = NULL;
static volatile sig_atomic_t gShellPendingSignals[NSIG] = {0};
static unsigned int gShellRandomSeed = 0;
static bool gShellRandomSeedInitialized = false;

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
                                       ShellExecRedirBackup **backups,
                                       size_t *count,
                                       size_t *capacity);
static void shellRestoreExecRedirections(ShellExecRedirBackup *backups,
                                         size_t count);
static void shellFreeExecRedirBackups(ShellExecRedirBackup *backups,
                                      size_t count);

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

static char *shellRunCommandSubstitution(const char *command) {
    int pipes[2] = {-1, -1};
    int saved_stdout = -1;
    char *output = NULL;
    size_t length = 0;
    size_t capacity = 0;

    if (pipe(pipes) != 0) {
        return strdup("");
    }

    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0) {
        close(pipes[0]);
        close(pipes[1]);
        return strdup("");
    }

    if (dup2(pipes[1], STDOUT_FILENO) < 0) {
        int err = errno;
        close(pipes[0]);
        close(pipes[1]);
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
        fprintf(stderr, "exsh: command substitution: failed to redirect stdout: %s\n", strerror(err));
        return strdup("");
    }
    close(pipes[1]);

    ShellRunOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.no_cache = 1;
    opts.quiet = true;
    opts.exit_on_signal = shellRuntimeExitOnSignal();
    const char *frontend_path = shellRuntimeGetArg0();
    opts.frontend_path = frontend_path ? frontend_path : "exsh";

    const char *source = command ? command : "";
    bool exit_requested = false;
    int status = shellRunSource(source, "<command-substitution>", &opts, &exit_requested);
    fflush(stdout);

    if (dup2(saved_stdout, STDOUT_FILENO) < 0) {
        /* best effort; continue */
    }
    close(saved_stdout);

    if (exit_requested || status == EXIT_SUCCESS) {
        status = gShellRuntime.last_status;
    } else {
        status = gShellRuntime.last_status;
    }
    shellUpdateStatus(status);

    char buffer[256];
    while (true) {
        ssize_t n = read(pipes[0], buffer, sizeof(buffer));
        if (n > 0) {
            if (!shellBufferEnsure(&output, &length, &capacity, (size_t)n)) {
                free(output);
                output = NULL;
                break;
            }
            memcpy(output + length, buffer, (size_t)n);
            length += (size_t)n;
            output[length] = '\0';
        } else if (n == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            free(output);
            output = NULL;
            break;
        }
    }
    close(pipes[0]);

    if (!output) {
        return strdup("");
    }
    while (length > 0 && (output[length - 1] == '\n' || output[length - 1] == '\r')) {
        output[--length] = '\0';
    }
    return output;
}

static bool shellBufferEnsure(char **buffer, size_t *length, size_t *capacity, size_t extra) {
    if (!buffer || !length || !capacity) {
        return false;
    }
    size_t needed = *length + extra + 1; // +1 for null terminator
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

static ShellFunctionEntry *shellFindFunctionEntry(const char *name) {
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
    ShellFunctionEntry *existing = shellFindFunctionEntry(name);
    if (existing) {
        if (existing->compiled && existing->compiled != compiled) {
            shellDisposeCompiledFunction(existing->compiled);
        }
        free(existing->name);
        free(existing->parameter_metadata);
        existing->name = name_copy;
        existing->parameter_metadata = meta_copy;
        existing->compiled = compiled;
        return true;
    }
    ShellFunctionEntry *entries = realloc(gShellFunctions, sizeof(ShellFunctionEntry) * (gShellFunctionCount + 1));
    if (!entries) {
        free(name_copy);
        free(meta_copy);
        return false;
    }
    gShellFunctions = entries;
    ShellFunctionEntry *entry = &gShellFunctions[gShellFunctionCount++];
    entry->name = name_copy;
    entry->parameter_metadata = meta_copy;
    entry->compiled = compiled;
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

    char *replacement = strdup("test");
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
        const char *previous = getenv(backups[i].name);
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
                if (!shellSplitExpandedWord(copy, 0, NULL, 0, &sub_fields, &sub_count)) {
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
    const char *ifs = getenv("IFS");
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

static char *shellJoinPositionalParameters(void) {
    if (gParamCount <= 0 || !gParamValues) {
        return strdup("");
    }
    size_t total = 0;
    for (int i = 0; i < gParamCount; ++i) {
        if (gParamValues[i]) {
            total += strlen(gParamValues[i]);
        }
        if (i + 1 < gParamCount) {
            total += 1; // space separator
        }
    }
    char *result = (char *)malloc(total + 1);
    if (!result) {
        return NULL;
    }
    size_t pos = 0;
    for (int i = 0; i < gParamCount; ++i) {
        const char *value = gParamValues[i] ? gParamValues[i] : "";
        size_t len = strlen(value);
        memcpy(result + pos, value, len);
        pos += len;
        if (i + 1 < gParamCount) {
            result[pos++] = ' ';
        }
    }
    result[pos] = '\0';
    return result;
}

static char *shellLookupParameterValueInternal(const char *name, size_t len, bool *out_is_set) {
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
                return shellJoinPositionalParameters();
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
    const char *env = getenv(key);
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

static bool shellArrayRegistrySetElement(const char *name,
                                         const char *subscript,
                                         const char *value) {
    if (!name || !subscript) {
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
    const char *text = value ? value : "";
    if (setenv(name, text, 1) != 0) {
        return false;
    }
    if (is_array_literal) {
        const char *current = getenv(name);
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
    unsetenv(name);
    shellArrayRegistryRemove(name);
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
    const char *env = getenv(key);
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
                                            size_t subscript_len) {
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
        if (subscript[0] == '@') {
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

static char *shellExpandWord(const char *text,
                             uint8_t flags,
                             const char *meta,
                             size_t meta_len,
                             bool **out_quoted_map,
                             size_t *out_quoted_len);

static char *shellNormalizeDollarCommandInline(const char *command, size_t len) {
    if (!command) {
        return NULL;
    }
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    size_t j = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = command[i];
        if (c == '\\' && i + 1 < len && command[i + 1] == '\n') {
            i++;
            continue;
        }
        out[j++] = c;
    }
    out[j] = '\0';
    char *shrunk = (char *)realloc(out, j + 1);
    return shrunk ? shrunk : out;
}

static char *shellNormalizeBacktickCommandInline(const char *command, size_t len) {
    if (!command) {
        return NULL;
    }
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    size_t j = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = command[i];
        if (c == '\\' && i + 1 < len) {
            char next = command[i + 1];
            if (next == '\n') {
                i++;
                continue;
            }
            if (next == '\\' || next == '`' || next == '$') {
                out[j++] = next;
                i++;
                continue;
            }
        }
        out[j++] = c;
    }
    out[j] = '\0';
    char *shrunk = (char *)realloc(out, j + 1);
    return shrunk ? shrunk : out;
}

static bool shellParseInlineDollarCommand(const char *text,
                                         size_t start,
                                         size_t text_len,
                                         size_t *out_span,
                                         char **out_command) {
    if (!text || start + 1 >= text_len || text[start] != '$' || text[start + 1] != '(') {
        return false;
    }
    size_t i = start + 2;
    int depth = 1;
    bool in_single = false;
    bool in_double = false;
    while (i < text_len) {
        char c = text[i];
        if (c == '\\' && i + 1 < text_len) {
            if (text[i + 1] == '\n') {
                i += 2;
                continue;
            }
            if (!in_single) {
                i += 2;
                continue;
            }
        }
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
        if (in_single || in_double) {
            i++;
            continue;
        }
        if (c == '(') {
            depth++;
        } else if (c == ')') {
            depth--;
            if (depth == 0) {
                break;
            }
        }
        i++;
    }
    if (depth != 0 || i >= text_len || text[i] != ')') {
        return false;
    }
    size_t span = (i + 1) - start;
    if (out_span) {
        *out_span = span;
    }
    if (out_command) {
        size_t inner_start = start + 2;
        size_t inner_len = i - inner_start;
        *out_command = shellNormalizeDollarCommandInline(text + inner_start, inner_len);
        if (!*out_command) {
            return false;
        }
    }
    return true;
}

static bool shellParseInlineBacktickCommand(const char *text,
                                            size_t start,
                                            size_t text_len,
                                            size_t *out_span,
                                            char **out_command) {
    if (!text || start >= text_len || text[start] != '`') {
        return false;
    }
    size_t i = start + 1;
    while (i < text_len) {
        char c = text[i];
        if (c == '`') {
            break;
        }
        if (c == '\\' && i + 1 < text_len) {
            i += 2;
            continue;
        }
        i++;
    }
    if (i >= text_len || text[i] != '`') {
        return false;
    }
    size_t span = (i + 1) - start;
    if (out_span) {
        *out_span = span;
    }
    if (out_command) {
        size_t inner_len = (i - start) - 1;
        *out_command = shellNormalizeBacktickCommandInline(text + start + 1, inner_len);
        if (!*out_command) {
            return false;
        }
    }
    return true;
}

static char *shellExpandHereDocument(const char *body, bool quoted) {
    if (quoted) {
        return body ? strdup(body) : strdup("");
    }
    return shellExpandWord(body, SHELL_WORD_FLAG_HAS_ARITHMETIC, NULL, 0, NULL, NULL);
}

static char *shellExpandParameter(const char *input, size_t *out_consumed) {
    if (out_consumed) {
        *out_consumed = 0;
    }
    if (!input || !*input) {
        return NULL;
    }
    if (*input == '{') {
        const char *closing = strchr(input + 1, '}');
        if (!closing) {
            return NULL;
        }
        if (out_consumed) {
            *out_consumed = (size_t)(closing - input + 1);
        }
        const char *inner = input + 1;
        size_t inner_len = (size_t)(closing - inner);
        if (inner_len == 0) {
            return strdup("");
        }
        if (*inner == '#') {
            const char *name_start = inner + 1;
            if (name_start >= closing) {
                return NULL;
            }
            const char *cursor = name_start;
            while (cursor < closing &&
                   (isalnum((unsigned char)*cursor) || *cursor == '_')) {
                cursor++;
            }
            if (cursor == name_start) {
                return NULL;
            }
            size_t name_len = (size_t)(cursor - name_start);
            if (cursor == closing) {
                char *value = shellLookupParameterValue(name_start, name_len);
                if (!value) {
                    return NULL;
                }
                size_t val_len = strlen(value);
                free(value);
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%zu", val_len);
                return strdup(buffer);
            }
            if (*cursor != '[') {
                return NULL;
            }
            const char *subscript_start = cursor + 1;
            const char *subscript_end =
                memchr(subscript_start, ']', (size_t)(closing - subscript_start));
            if (!subscript_end || subscript_end > closing) {
                return NULL;
            }
            size_t subscript_len = (size_t)(subscript_end - subscript_start);
            const char *after_bracket = subscript_end + 1;
            while (after_bracket < closing &&
                   isspace((unsigned char)*after_bracket)) {
                after_bracket++;
            }
            if (after_bracket != closing) {
                return NULL;
            }
            while (subscript_len > 0 &&
                   isspace((unsigned char)subscript_start[0])) {
                subscript_start++;
                subscript_len--;
            }
            while (subscript_len > 0 &&
                   isspace((unsigned char)subscript_start[subscript_len - 1])) {
                subscript_len--;
            }
            if (subscript_len == 1 &&
                (subscript_start[0] == '@' || subscript_start[0] == '*')) {
                const ShellArrayVariable *array_var =
                    shellArrayRegistryLookup(name_start, name_len);
                size_t count = 0;
                if (array_var) {
                    count = array_var->count;
                } else {
                    char *raw = shellLookupRawEnvironmentValue(name_start, name_len);
                    if (!raw) {
                        return NULL;
                    }
                    char **items = NULL;
                    if (!shellParseArrayValues(raw, &items, &count)) {
                        free(raw);
                        return NULL;
                    }
                    free(raw);
                    shellFreeArrayValues(items, count);
                }
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%zu", count);
                return strdup(buffer);
            }
            char *element = shellExpandArraySubscriptValue(
                name_start, name_len, subscript_start, subscript_len);
            if (!element) {
                return NULL;
            }
            size_t elem_len = strlen(element);
            free(element);
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%zu", elem_len);
            return strdup(buffer);
        }

        if (*inner == '!') {
            const char *name_start = inner + 1;
            if (name_start >= closing) {
                return NULL;
            }
            const char *cursor = name_start;
            while (cursor < closing && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
                cursor++;
            }
            if (cursor == name_start) {
                return NULL;
            }
            size_t name_len = (size_t)(cursor - name_start);
            if (cursor == closing || *cursor != '[') {
                return NULL;
            }
            const char *subscript_start = cursor + 1;
            const char *subscript_end = memchr(subscript_start, ']', (size_t)(closing - subscript_start));
            if (!subscript_end || subscript_end > closing) {
                return NULL;
            }
            size_t subscript_len = (size_t)(subscript_end - subscript_start);
            const char *after_bracket = subscript_end + 1;
            while (after_bracket < closing && isspace((unsigned char)*after_bracket)) {
                after_bracket++;
            }
            if (after_bracket != closing) {
                return NULL;
            }
            if (!(subscript_len == 1 && (subscript_start[0] == '@' || subscript_start[0] == '*'))) {
                return NULL;
            }
            const ShellArrayVariable *array_var = shellArrayRegistryLookup(name_start, name_len);
            char **items = NULL;
            char **keys = NULL;
            size_t count = 0;
            ShellArrayKind kind = SHELL_ARRAY_KIND_INDEXED;
            bool using_registry = false;
            if (array_var) {
                items = array_var->values;
                keys = array_var->keys;
                count = array_var->count;
                kind = array_var->kind;
                using_registry = true;
            } else {
                char *raw = shellLookupRawEnvironmentValue(name_start, name_len);
                if (!raw) {
                    return NULL;
                }
                if (!shellParseArrayLiteral(raw, &items, &keys, &count, &kind)) {
                    free(raw);
                    return NULL;
                }
                free(raw);
            }
            char *joined = NULL;
            if (kind == SHELL_ARRAY_KIND_ASSOCIATIVE) {
                if (subscript_start[0] == '@') {
                    joined = shellJoinArrayValuesWithSeparator(keys, count, SHELL_ARRAY_ELEMENT_SEP);
                } else {
                    joined = shellJoinArrayValues(keys, count);
                }
            } else {
                char separator = (subscript_start[0] == '@') ? SHELL_ARRAY_ELEMENT_SEP : ' ';
                joined = shellJoinNumericIndices(count, separator);
            }
            if (!using_registry) {
                shellFreeArrayValues(items, count);
                if (keys) {
                    shellFreeArrayValues(keys, count);
                }
            }
            if (!joined) {
                joined = strdup("");
            }
            return joined;
        }

        const char *inner_end = inner + inner_len;
        const char *default_pos = NULL;
        bool default_requires_value = false;
        size_t bracket_depth = 0;
        for (const char *scan = inner; scan < inner_end; ++scan) {
            char ch = *scan;
            if (ch == '[') {
                bracket_depth++;
                continue;
            }
            if (ch == ']' && bracket_depth > 0) {
                bracket_depth--;
                continue;
            }
            if (bracket_depth > 0) {
                continue;
            }
            if (ch == ':' && (scan + 1) < inner_end && scan[1] == '-') {
                default_pos = scan;
                default_requires_value = true;
                break;
            }
            if (ch == '-' && (scan == inner || scan[-1] != ':')) {
                default_pos = scan;
                default_requires_value = false;
                break;
            }
        }
        if (default_pos) {
            size_t name_len = (size_t)(default_pos - inner);
            if (name_len == 0) {
                return NULL;
            }
            bool simple_name = false;
            if (name_len == 1) {
                unsigned char first = (unsigned char)inner[0];
                if (isalnum(first) || first == '_' || first == '*' || first == '@' ||
                    first == '#' || first == '?' || first == '$') {
                    simple_name = true;
                }
            } else {
                simple_name = true;
                for (size_t i = 0; i < name_len; ++i) {
                    unsigned char ch = (unsigned char)inner[i];
                    if (!isalnum(ch) && ch != '_') {
                        simple_name = false;
                        break;
                    }
                }
            }
            if (simple_name) {
                bool is_set = false;
                char *value = shellLookupParameterValueInternal(inner, name_len, &is_set);
                if (!value) {
                    return NULL;
                }
                bool use_default = !is_set || (default_requires_value && value[0] == '\0');
                if (!use_default) {
                    return value;
                }
                free(value);
                const char *default_start = default_pos + (default_requires_value ? 2 : 1);
                size_t default_len = (size_t)(inner_end - default_start);
                char *raw_default = (char *)malloc(default_len + 1);
                if (!raw_default) {
                    return NULL;
                }
                if (default_len > 0) {
                    memcpy(raw_default, default_start, default_len);
                }
                raw_default[default_len] = '\0';
                char *expanded_default = shellExpandWord(raw_default, 0, NULL, 0, NULL, NULL);
                free(raw_default);
                if (!expanded_default) {
                    return NULL;
                }
                return expanded_default;
            }
        }

        const char *colon = memchr(inner, ':', inner_len);
        if (colon && colon > inner) {
            const char *offset_start = colon + 1;
            if (offset_start >= inner_end ||
                !isdigit((unsigned char)*offset_start)) {
                return NULL;
            }
            size_t offset_value = 0;
            size_t offset_digits = 0;
            const char *cursor = offset_start;
            while (cursor < inner_end && isdigit((unsigned char)*cursor)) {
                if (offset_value > (SIZE_MAX - 9) / 10) {
                    offset_value = SIZE_MAX;
                } else {
                    offset_value = offset_value * 10 + (size_t)(*cursor - '0');
                }
                cursor++;
                offset_digits++;
            }
            if (offset_digits == 0) {
                return NULL;
            }
            const char *after_offset = cursor;
            bool have_length = false;
            size_t length_value = 0;
            if (after_offset < inner_end) {
                if (*after_offset != ':') {
                    return NULL;
                }
                const char *length_start = after_offset + 1;
                if (length_start >= inner_end ||
                    !isdigit((unsigned char)*length_start)) {
                    return NULL;
                }
                const char *length_cursor = length_start;
                size_t length_digits = 0;
                while (length_cursor < inner_end &&
                       isdigit((unsigned char)*length_cursor)) {
                    if (length_value > (SIZE_MAX - 9) / 10) {
                        length_value = SIZE_MAX;
                    } else {
                        length_value =
                            length_value * 10 + (size_t)(*length_cursor - '0');
                    }
                    length_cursor++;
                    length_digits++;
                }
                if (length_digits == 0 || length_cursor != inner_end) {
                    return NULL;
                }
                have_length = true;
            }
            size_t name_len = (size_t)(colon - inner);
            char *value = shellLookupParameterValue(inner, name_len);
            if (!value) {
                return NULL;
            }
            size_t value_len = strlen(value);
            size_t start_index = offset_value;
            if (start_index > value_len) {
                start_index = value_len;
            }
            size_t available = value_len - start_index;
            size_t copy_len = available;
            if (have_length && length_value < copy_len) {
                copy_len = length_value;
            }
            char *result = (char *)malloc(copy_len + 1);
            if (!result) {
                free(value);
                return NULL;
            }
            if (copy_len > 0) {
                memcpy(result, value + start_index, copy_len);
            }
            result[copy_len] = '\0';
            free(value);
            return result;
        }

        const char *cursor = inner;
        while (cursor < closing &&
               (isalnum((unsigned char)*cursor) || *cursor == '_')) {
            cursor++;
        }
        if (cursor == inner) {
            return NULL;
        }
        size_t name_len = (size_t)(cursor - inner);
        if (cursor < closing && *cursor == '[') {
            const char *subscript_start = cursor + 1;
            const char *subscript_end = memchr(subscript_start, ']', (size_t)(closing - subscript_start));
            if (!subscript_end || subscript_end > closing) {
                return NULL;
            }
            size_t subscript_len = (size_t)(subscript_end - subscript_start);
            const char *after_bracket = subscript_end + 1;
            while (after_bracket < closing && isspace((unsigned char)*after_bracket)) {
                after_bracket++;
            }
            if (after_bracket != closing) {
                return NULL;
            }
            return shellExpandArraySubscriptValue(inner, name_len, subscript_start, subscript_len);
        }
        if (cursor < closing && (*cursor == '%' || *cursor == '#')) {
            bool remove_suffix = (*cursor == '%');
            bool longest = false;
            char op = *cursor;
            cursor++;
            if (cursor < closing && *cursor == op) {
                longest = true;
                cursor++;
            }
            const char *pattern_start = cursor;
            size_t pattern_len = (size_t)(closing - pattern_start);
            char *expanded_pattern = shellExpandPatternText(pattern_start, pattern_len);
            if (!expanded_pattern) {
                return NULL;
            }
            char *value = shellLookupParameterValue(inner, name_len);
            if (!value) {
                free(expanded_pattern);
                return NULL;
            }
            char *result = remove_suffix ? shellRemovePatternSuffix(value, expanded_pattern, longest)
                                         : shellRemovePatternPrefix(value, expanded_pattern, longest);
            free(value);
            free(expanded_pattern);
            return result;
        }
        if (cursor != closing) {
            return NULL;
        }
        return shellLookupParameterValue(inner, name_len);
    }

    if (*input == '$') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d", (int)getpid());
        return strdup(buffer);
    }

    if (*input == '?') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d", gShellRuntime.last_status);
        return strdup(buffer);
    }

    if (*input == '#') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d", gParamCount);
        return strdup(buffer);
    }

    if (*input == '*' || *input == '@') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        return shellJoinPositionalParameters();
    }

    if (*input == '0') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        if (gShellArg0) {
            return strdup(gShellArg0);
        }
        return strdup("exsh");
    }

    if (isdigit((unsigned char)*input)) {
        const char *cursor = input;
        while (isdigit((unsigned char)*cursor)) {
            cursor++;
        }
        if (out_consumed) {
            *out_consumed = (size_t)(cursor - input);
        }
        return shellLookupParameterValue(input, (size_t)(cursor - input));
    }

    if (isalpha((unsigned char)*input) || *input == '_') {
        const char *cursor = input + 1;
        while (isalnum((unsigned char)*cursor) || *cursor == '_') {
            cursor++;
        }
        if (out_consumed) {
            *out_consumed = (size_t)(cursor - input);
        }
        return shellLookupParameterValue(input, (size_t)(cursor - input));
    }

    return NULL;
}

static void shellMarkArithmeticError(void) {
    shellUpdateStatus(1);
    gShellArithmeticErrorPending = true;
}

typedef struct {
    const char *input;
    size_t length;
    size_t pos;
} ShellArithmeticParser;

static void shellArithmeticSkipWhitespace(ShellArithmeticParser *parser) {
    if (!parser) {
        return;
    }
    while (parser->pos < parser->length &&
           isspace((unsigned char)parser->input[parser->pos])) {
        parser->pos++;
    }
}

static bool shellArithmeticParseValueString(const char *text, long long *out_value) {
    if (!out_value) {
        return false;
    }
    if (!text || *text == '\0') {
        *out_value = 0;
        return true;
    }
    errno = 0;
    char *endptr = NULL;
    long long value = strtoll(text, &endptr, 0);
    if (errno != 0) {
        return false;
    }
    if (endptr && *endptr != '\0') {
        while (endptr && *endptr && isspace((unsigned char)*endptr)) {
            endptr++;
        }
        if (endptr && *endptr != '\0') {
            return false;
        }
    }
    *out_value = value;
    return true;
}

static bool shellArithmeticParseExpression(ShellArithmeticParser *parser, long long *out_value);

static bool shellArithmeticParsePrimary(ShellArithmeticParser *parser, long long *out_value) {
    if (!parser || !out_value) {
        return false;
    }
    shellArithmeticSkipWhitespace(parser);
    if (parser->pos >= parser->length) {
        return false;
    }
    char c = parser->input[parser->pos];
    if (c == '(') {
        parser->pos++;
        if (!shellArithmeticParseExpression(parser, out_value)) {
            return false;
        }
        shellArithmeticSkipWhitespace(parser);
        if (parser->pos >= parser->length || parser->input[parser->pos] != ')') {
            return false;
        }
        parser->pos++;
        return true;
    }
    if (c == '$') {
        parser->pos++;
        size_t consumed = 0;
        char *value = shellExpandParameter(parser->input + parser->pos, &consumed);
        if (!value) {
            return false;
        }
        parser->pos += consumed;
        long long parsed = 0;
        bool ok = shellArithmeticParseValueString(value, &parsed);
        free(value);
        if (!ok) {
            return false;
        }
        *out_value = parsed;
        return true;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        size_t start = parser->pos;
        parser->pos++;
        while (parser->pos < parser->length) {
            char ch = parser->input[parser->pos];
            if (!isalnum((unsigned char)ch) && ch != '_') {
                break;
            }
            parser->pos++;
        }
        size_t len = parser->pos - start;
        char *value = shellLookupParameterValue(parser->input + start, len);
        if (!value) {
            return false;
        }
        long long parsed = 0;
        bool ok = shellArithmeticParseValueString(value, &parsed);
        free(value);
        if (!ok) {
            return false;
        }
        *out_value = parsed;
        return true;
    }
    if (isdigit((unsigned char)c)) {
        const char *start_ptr = parser->input + parser->pos;
        errno = 0;
        char *endptr = NULL;
        long long value = strtoll(start_ptr, &endptr, 0);
        if (errno != 0 || endptr == start_ptr) {
            return false;
        }
        size_t consumed = (size_t)(endptr - start_ptr);
        parser->pos += consumed;
        if (parser->pos < parser->length) {
            char next = parser->input[parser->pos];
            if (isalnum((unsigned char)next) || next == '_') {
                return false;
            }
        }
        *out_value = value;
        return true;
    }
    return false;
}

static bool shellArithmeticParseUnary(ShellArithmeticParser *parser, long long *out_value) {
    if (!parser || !out_value) {
        return false;
    }
    shellArithmeticSkipWhitespace(parser);
    if (parser->pos >= parser->length) {
        return false;
    }
    char c = parser->input[parser->pos];
    if (c == '+') {
        parser->pos++;
        return shellArithmeticParseUnary(parser, out_value);
    }
    if (c == '-') {
        parser->pos++;
        long long value = 0;
        if (!shellArithmeticParseUnary(parser, &value)) {
            return false;
        }
        *out_value = -value;
        return true;
    }
    return shellArithmeticParsePrimary(parser, out_value);
}

static bool shellArithmeticParseTerm(ShellArithmeticParser *parser, long long *out_value) {
    if (!parser || !out_value) {
        return false;
    }
    long long value = 0;
    if (!shellArithmeticParseUnary(parser, &value)) {
        return false;
    }
    while (true) {
        shellArithmeticSkipWhitespace(parser);
        if (parser->pos >= parser->length) {
            break;
        }
        char op = parser->input[parser->pos];
        if (op != '*' && op != '/' && op != '%') {
            break;
        }
        parser->pos++;
        long long rhs = 0;
        if (!shellArithmeticParseUnary(parser, &rhs)) {
            return false;
        }
        if (op == '*') {
            value *= rhs;
        } else if (op == '/') {
            if (rhs == 0) {
                return false;
            }
            value /= rhs;
        } else {
            if (rhs == 0) {
                return false;
            }
            value %= rhs;
        }
    }
    *out_value = value;
    return true;
}

static bool shellArithmeticParseExpression(ShellArithmeticParser *parser, long long *out_value) {
    if (!parser || !out_value) {
        return false;
    }
    long long value = 0;
    if (!shellArithmeticParseTerm(parser, &value)) {
        return false;
    }
    while (true) {
        shellArithmeticSkipWhitespace(parser);
        if (parser->pos >= parser->length) {
            break;
        }
        char op = parser->input[parser->pos];
        if (op != '+' && op != '-') {
            break;
        }
        parser->pos++;
        long long rhs = 0;
        if (!shellArithmeticParseTerm(parser, &rhs)) {
            return false;
        }
        if (op == '+') {
            value += rhs;
        } else {
            value -= rhs;
        }
    }
    *out_value = value;
    return true;
}

static char *shellEvaluateArithmetic(const char *expr, bool *out_error) {
    if (out_error) {
        *out_error = false;
    }
    if (!expr) {
        if (out_error) {
            *out_error = true;
        }
        return NULL;
    }
    ShellArithmeticParser parser;
    parser.input = expr;
    parser.length = strlen(expr);
    parser.pos = 0;
    long long value = 0;
    if (!shellArithmeticParseExpression(&parser, &value)) {
        if (out_error) {
            *out_error = true;
        }
        return NULL;
    }
    shellArithmeticSkipWhitespace(&parser);
    if (parser.pos < parser.length) {
        if (out_error) {
            *out_error = true;
        }
        return NULL;
    }
    char buffer[64];
    int written = snprintf(buffer, sizeof(buffer), "%lld", value);
    if (written < 0) {
        if (out_error) {
            *out_error = true;
        }
        return NULL;
    }
    char *result = strdup(buffer);
    if (!result) {
        if (out_error) {
            *out_error = true;
        }
        return NULL;
    }
    return result;
}

static char *shellExpandWord(const char *text,
                             uint8_t flags,
                             const char *meta,
                             size_t meta_len,
                             bool **out_quoted_map,
                             size_t *out_quoted_len) {
    if (out_quoted_map) {
        *out_quoted_map = NULL;
    }
    if (out_quoted_len) {
        *out_quoted_len = 0;
    }
    if (!text) {
        return strdup("");
    }

    bool track_quotes = out_quoted_map && out_quoted_len;
    bool *quoted_map = NULL;
    size_t quoted_len = 0;
    size_t quoted_cap = 0;

    ShellMetaSubstitution *subs = NULL;
    size_t sub_count = 0;
    if (!shellParseCommandMetadata(meta, meta_len, &subs, &sub_count)) {
        subs = NULL;
        sub_count = 0;
    }

    size_t text_len = strlen(text);
    size_t length = 0;
    size_t capacity = text_len + 1;
    if (capacity < 32) {
        capacity = 32;
    }
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        shellFreeMetaSubstitutions(subs, sub_count);
        return NULL;
    }
    buffer[0] = '\0';

    bool base_single = (flags & SHELL_WORD_FLAG_SINGLE_QUOTED) != 0;
    bool base_double = (flags & SHELL_WORD_FLAG_DOUBLE_QUOTED) != 0;
    bool in_single_segment = false;
    bool in_double_segment = false;
    bool saw_single_marker = false;
    bool saw_double_marker = false;
    bool has_arithmetic = (flags & SHELL_WORD_FLAG_HAS_ARITHMETIC) != 0;
    size_t sub_index = 0;

    for (size_t i = 0; i < text_len;) {
        char c = text[i];
        if (c == SHELL_QUOTE_MARK_SINGLE) {
            saw_single_marker = true;
            in_single_segment = !in_single_segment;
            i++;
            continue;
        }
        if (c == SHELL_QUOTE_MARK_DOUBLE) {
            saw_double_marker = true;
            in_double_segment = !in_double_segment;
            i++;
            continue;
        }

        bool effective_single = in_single_segment || (!saw_single_marker && base_single);
        bool effective_double = in_double_segment || (!saw_double_marker && base_double);
        bool quoted_flag = effective_single || effective_double;

        if (effective_single) {
            if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap, true, 1)) {
                goto expand_error;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }

        bool handled = false;
        if (sub_index < sub_count) {
            ShellMetaSubstitution *sub = &subs[sub_index];
            size_t span = sub->span_length;
            if (sub->style == SHELL_META_SUBSTITUTION_DOLLAR && c == '$' && i + 1 < text_len && text[i + 1] == '(') {
                if (span > 0 && i + span <= text_len) {
                    char *output = shellRunCommandSubstitution(sub->command);
                    if (output) {
                        size_t out_len = strlen(output);
                        if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                          quoted_flag, out_len)) {
                            free(output);
                            goto expand_error;
                        }
                        shellBufferAppendString(&buffer, &length, &capacity, output);
                        free(output);
                    }
                    i += span;
                    sub_index++;
                    handled = true;
                } else {
                    sub_index++;
                }
            } else if (sub->style == SHELL_META_SUBSTITUTION_BACKTICK && c == '`') {
                if (span > 0 && i + span <= text_len) {
                    char *output = shellRunCommandSubstitution(sub->command);
                    if (output) {
                        size_t out_len = strlen(output);
                        if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                          quoted_flag, out_len)) {
                            free(output);
                            goto expand_error;
                        }
                        shellBufferAppendString(&buffer, &length, &capacity, output);
                        free(output);
                    }
                    i += span;
                    sub_index++;
                    handled = true;
                } else {
                    sub_index++;
                }
            }
        }
        if (handled) {
            continue;
        }

        if (sub_count == 0 && c == '$' && i + 1 < text_len && text[i + 1] == '(' &&
            !(i + 2 < text_len && text[i + 2] == '(')) {
            size_t span = 0;
            char *command = NULL;
            if (shellParseInlineDollarCommand(text, i, text_len, &span, &command)) {
                char *output = shellRunCommandSubstitution(command);
                free(command);
                if (output) {
                    size_t out_len = strlen(output);
                    if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                      quoted_flag, out_len)) {
                        free(output);
                        goto expand_error;
                    }
                    shellBufferAppendString(&buffer, &length, &capacity, output);
                    free(output);
                }
                i += span;
                continue;
            }
        }
        if (sub_count == 0 && c == '`') {
            size_t span = 0;
            char *command = NULL;
            if (shellParseInlineBacktickCommand(text, i, text_len, &span, &command)) {
                char *output = shellRunCommandSubstitution(command);
                free(command);
                if (output) {
                    size_t out_len = strlen(output);
                    if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                      quoted_flag, out_len)) {
                        free(output);
                        goto expand_error;
                    }
                    shellBufferAppendString(&buffer, &length, &capacity, output);
                    free(output);
                }
                i += span;
                continue;
            }
        }

        if (c == '$' && has_arithmetic && i + 2 < text_len && text[i + 1] == '(' && text[i + 2] == '(') {
            size_t expr_start = i + 3;
            size_t j = expr_start;
            int depth = 1;
            while (j < text_len) {
                char inner = text[j];
                if (inner == '(') {
                    depth++;
                } else if (inner == ')') {
                    depth--;
                    if (depth == 0) {
                        break;
                    }
                }
                j++;
            }
            size_t span = 0;
            if (depth == 0 && j + 1 < text_len && text[j + 1] == ')') {
                span = (j + 2) - i;
                size_t expr_len = j - expr_start;
                char *expr = (char *)malloc(expr_len + 1);
                if (!expr) {
                    shellMarkArithmeticError();
                    if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                      quoted_flag, span)) {
                        goto expand_error;
                    }
                    shellBufferAppendSlice(&buffer, &length, &capacity, text + i, span);
                    i += span;
                    continue;
                }
                if (expr_len > 0) {
                    memcpy(expr, text + expr_start, expr_len);
                }
                expr[expr_len] = '\0';
                bool eval_error = false;
                char *result = shellEvaluateArithmetic(expr, &eval_error);
                free(expr);
                if (!eval_error && result) {
                    size_t out_len = strlen(result);
                    if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                      quoted_flag, out_len)) {
                        free(result);
                        goto expand_error;
                    }
                    shellBufferAppendString(&buffer, &length, &capacity, result);
                    free(result);
                } else {
                    shellMarkArithmeticError();
                    if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                      quoted_flag, span)) {
                        goto expand_error;
                    }
                    shellBufferAppendSlice(&buffer, &length, &capacity, text + i, span);
                }
                i += span;
                continue;
            } else {
                span = text_len - i;
                shellMarkArithmeticError();
                if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                  quoted_flag, span)) {
                    goto expand_error;
                }
                shellBufferAppendSlice(&buffer, &length, &capacity, text + i, span);
                i = text_len;
                continue;
            }
        }

        bool treat_as_double = effective_double;
        if (c == '\\') {
            if (i + 1 < text_len) {
                char next = text[i + 1];
                if (!treat_as_double || next == '$' || next == '"' || next == '\\' || next == '`' || next == '\n') {
                    if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                      quoted_flag, 1)) {
                        goto expand_error;
                    }
                    shellBufferAppendChar(&buffer, &length, &capacity, next);
                    i += 2;
                    continue;
                }
            }
            if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                              quoted_flag, 1)) {
                goto expand_error;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }

        if (c == '$') {
            size_t consumed = 0;
            char *expanded = shellExpandParameter(text + i + 1, &consumed);
            if (expanded) {
                size_t out_len = strlen(expanded);
                if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                  quoted_flag, out_len)) {
                    free(expanded);
                    goto expand_error;
                }
                shellBufferAppendString(&buffer, &length, &capacity, expanded);
                free(expanded);
                i += consumed + 1;
                continue;
            }
        }

        if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                          quoted_flag, 1)) {
            goto expand_error;
        }
        shellBufferAppendChar(&buffer, &length, &capacity, c);
        i++;
    }

    shellFreeMetaSubstitutions(subs, sub_count);
    if (track_quotes) {
        *out_quoted_map = quoted_map;
        *out_quoted_len = quoted_len;
    } else {
        free(quoted_map);
    }
    return buffer;

expand_error:
    shellFreeMetaSubstitutions(subs, sub_count);
    free(buffer);
    free(quoted_map);
    return NULL;
}

static void shellFreeRedirections(ShellCommand *cmd) {
    if (!cmd) {
        return;
    }
    for (size_t i = 0; i < cmd->redir_count; ++i) {
        free(cmd->redirs[i].path);
        free(cmd->redirs[i].here_doc);
    }
    free(cmd->redirs);
    cmd->redirs = NULL;
    cmd->redir_count = 0;
}

static void shellFreeCommand(ShellCommand *cmd) {
    if (!cmd) {
        return;
    }
    for (size_t i = 0; i < cmd->assignment_count; ++i) {
        free(cmd->assignments[i].text);
    }
    free(cmd->assignments);
    cmd->assignments = NULL;
    cmd->assignment_count = 0;
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

static bool shellStringEqualsIgnoreCase(const char *lhs, const char *rhs) {
    if (!lhs || !rhs) {
        return false;
    }
    return strcasecmp(lhs, rhs) == 0;
}

static bool shellTryParseIntegerLiteral(const char *text, long long *out_value) {
    if (!text || !*text) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long long value = strtoll(text, &end, 0);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    if (out_value) {
        *out_value = value;
    }
    return true;
}

static bool shellLooksLikeFloatLiteral(const char *text) {
    if (!text) {
        return false;
    }
    for (const char *cursor = text; *cursor; ++cursor) {
        if (*cursor == '.' || *cursor == 'e' || *cursor == 'E') {
            return true;
        }
    }
    if (shellStringEqualsIgnoreCase(text, "inf") ||
        shellStringEqualsIgnoreCase(text, "+inf") ||
        shellStringEqualsIgnoreCase(text, "-inf") ||
        shellStringEqualsIgnoreCase(text, "infinity") ||
        shellStringEqualsIgnoreCase(text, "+infinity") ||
        shellStringEqualsIgnoreCase(text, "-infinity") ||
        shellStringEqualsIgnoreCase(text, "nan") ||
        shellStringEqualsIgnoreCase(text, "+nan") ||
        shellStringEqualsIgnoreCase(text, "-nan")) {
        return true;
    }
    return false;
}

static bool shellTryParseFloatLiteral(const char *text, double *out_value) {
    if (!text || !*text) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    double value = strtod(text, &end);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    if (out_value) {
        *out_value = value;
    }
    return true;
}

static Value shellConvertBuiltinArgument(const char *text) {
    if (!text) {
        return makeString("");
    }

    enum {
        SHELL_ARG_MODE_AUTO,
        SHELL_ARG_MODE_STRING,
        SHELL_ARG_MODE_BOOL,
        SHELL_ARG_MODE_INT,
        SHELL_ARG_MODE_FLOAT,
        SHELL_ARG_MODE_NIL
    } mode = SHELL_ARG_MODE_AUTO;

    const char *payload = text;

    if (strncasecmp(payload, "str:", 4) == 0) {
        mode = SHELL_ARG_MODE_STRING;
        payload += 4;
    } else if (strncasecmp(payload, "string:", 7) == 0) {
        mode = SHELL_ARG_MODE_STRING;
        payload += 7;
    } else if (strncasecmp(payload, "raw:", 4) == 0) {
        mode = SHELL_ARG_MODE_STRING;
        payload += 4;
    } else if (strncasecmp(payload, "bool:", 5) == 0) {
        mode = SHELL_ARG_MODE_BOOL;
        payload += 5;
    } else if (strncasecmp(payload, "boolean:", 8) == 0) {
        mode = SHELL_ARG_MODE_BOOL;
        payload += 8;
    } else if (strncasecmp(payload, "int:", 4) == 0) {
        mode = SHELL_ARG_MODE_INT;
        payload += 4;
    } else if (strncasecmp(payload, "integer:", 8) == 0) {
        mode = SHELL_ARG_MODE_INT;
        payload += 8;
    } else if (strncasecmp(payload, "float:", 6) == 0) {
        mode = SHELL_ARG_MODE_FLOAT;
        payload += 6;
    } else if (strncasecmp(payload, "double:", 7) == 0) {
        mode = SHELL_ARG_MODE_FLOAT;
        payload += 7;
    } else if (strncasecmp(payload, "real:", 5) == 0) {
        mode = SHELL_ARG_MODE_FLOAT;
        payload += 5;
    } else if (strncasecmp(payload, "nil:", 4) == 0) {
        mode = SHELL_ARG_MODE_NIL;
        payload += 4;
    }

    if (mode == SHELL_ARG_MODE_STRING) {
        return makeString(payload ? payload : "");
    }

    if (mode == SHELL_ARG_MODE_NIL) {
        return makeNil();
    }

    if (mode == SHELL_ARG_MODE_BOOL || mode == SHELL_ARG_MODE_AUTO) {
        bool flag = false;
        if (shellParseBool(payload, &flag)) {
            return makeBoolean(flag ? 1 : 0);
        }
        if (mode == SHELL_ARG_MODE_BOOL) {
            return makeString(payload ? payload : "");
        }
    }

    if (mode == SHELL_ARG_MODE_INT || mode == SHELL_ARG_MODE_AUTO) {
        long long int_value = 0;
        if (shellTryParseIntegerLiteral(payload, &int_value)) {
            return makeInt(int_value);
        }
        if (mode == SHELL_ARG_MODE_INT) {
            return makeString(payload ? payload : "");
        }
    }

    if (mode == SHELL_ARG_MODE_FLOAT || mode == SHELL_ARG_MODE_AUTO) {
        if (mode == SHELL_ARG_MODE_FLOAT || shellLooksLikeFloatLiteral(payload)) {
            double dbl_value = 0.0;
            if (shellTryParseFloatLiteral(payload, &dbl_value)) {
                return makeDouble(dbl_value);
            }
            if (mode == SHELL_ARG_MODE_FLOAT) {
                return makeString(payload ? payload : "");
            }
        }
    }

    if (mode == SHELL_ARG_MODE_AUTO && payload && *payload) {
        if (shellStringEqualsIgnoreCase(payload, "nil") ||
            shellStringEqualsIgnoreCase(payload, "null")) {
            return makeNil();
        }
    }

    return makeString(payload ? payload : "");
}

static void shellUpdateStatus(int status) {
    gShellStatusVersion++;
    if (gShellArithmeticErrorPending) {
        status = 1;
        gShellArithmeticErrorPending = false;
    }
    gShellRuntime.last_status = status;
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", status);
    shellSetTrackedVariable("PSCALSHELL_LAST_STATUS", buffer, false);
    if (status != 0) {
        if (gShellRuntime.errexit_enabled) {
            gShellRuntime.errexit_pending = true;
            gShellExitRequested = true;
            if (gShellCurrentVm) {
                gShellCurrentVm->abort_requested = true;
                gShellCurrentVm->exit_requested = true;
                gShellCurrentVm->current_builtin_name = "errexit";
            }
        }
    } else {
        gShellRuntime.errexit_pending = false;
    }
}

/*
 * POSIX specifies that foreground commands should see the shell's inherited
 * signal dispositions, except that asynchronous lists without job control must
 * inherit SIG_IGN for SIGINT and SIGQUIT, and that traps fire only after the
 * foreground job or wait completes.  We record pending signals in an
 * async-signal-safe manner and reconcile them once we're back in the main
 * interpreter loop so the runtime can unwind cleanly before honouring traps.
 */
static void shellHandlePendingSignal(int signo) {
    if (signo != SIGINT && signo != SIGQUIT && signo != SIGTSTP) {
        return;
    }

    shellUpdateStatus(128 + signo);

    if (gShellCurrentVm) {
        gShellCurrentVm->exit_requested = true;
        gShellCurrentVm->current_builtin_name = "signal";
    }

    if (!gShellRuntime.job_control_enabled) {
        gShellExitRequested = true;
    }

    if (gShellLoopStackSize > 0) {
        gShellRuntime.break_requested = true;
        gShellRuntime.break_requested_levels = (int)gShellLoopStackSize;
        shellLoopRequestBreakLevels((int)gShellLoopStackSize);
    }

    bool propagate_default =
        (gShellExitOnSignalFlag && (signo == SIGINT || signo == SIGQUIT || signo == SIGTSTP) &&
         !gShellRuntime.trap_enabled);

    if (propagate_default) {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        sigemptyset(&action.sa_mask);
        action.sa_handler = SIG_DFL;
        sigaction(signo, &action, NULL);
        raise(signo);
        if (signo == SIGTSTP) {
            memset(&action, 0, sizeof(action));
            sigemptyset(&action.sa_mask);
            action.sa_handler = shellSignalHandler;
            action.sa_flags |= SA_RESTART;
            sigaction(signo, &action, NULL);
        }
    }
}

void shellRuntimeProcessPendingSignals(void) {
    for (int signo = 1; signo < NSIG; ++signo) {
        if (!gShellPendingSignals[signo]) {
            continue;
        }
        gShellPendingSignals[signo] = 0;
        shellHandlePendingSignal(signo);
    }
}

void shellRuntimeSetExitOnSignal(bool enabled) {
    gShellExitOnSignalFlag = enabled ? 1 : 0;
}

bool shellRuntimeExitOnSignal(void) {
    return gShellExitOnSignalFlag != 0;
}

void shellRuntimeInitSignals(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = shellSignalHandler;
    action.sa_flags |= SA_RESTART;

    sigaction(SIGINT, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);
    sigaction(SIGTSTP, &action, NULL);
}

static bool shellHistoryEnsureCapacity(size_t needed) {
    if (gShellHistory.capacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellHistory.capacity ? gShellHistory.capacity * 2 : 16;
    if (new_capacity < needed) {
        new_capacity = needed;
    }
    if (new_capacity > SIZE_MAX / sizeof(char *)) {
        return false;
    }
    char **entries = realloc(gShellHistory.entries, new_capacity * sizeof(char *));
    if (!entries) {
        return false;
    }
    gShellHistory.entries = entries;
    gShellHistory.capacity = new_capacity;
    return true;
}

void shellRuntimeRecordHistory(const char *line) {
    if (!line) {
        return;
    }
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        --len;
    }
    if (len == 0) {
        return;
    }
    bool has_content = false;
    for (size_t i = 0; i < len; ++i) {
        if (line[i] != ' ' && line[i] != '\t') {
            has_content = true;
            break;
        }
    }
    if (!has_content) {
        return;
    }
    if (!shellHistoryEnsureCapacity(gShellHistory.count + 1)) {
        return;
    }
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        return;
    }
    memcpy(copy, line, len);
    copy[len] = '\0';
    gShellHistory.entries[gShellHistory.count++] = copy;
}

size_t shellRuntimeHistoryCount(void) {
    return gShellHistory.count;
}

bool shellRuntimeHistoryGetEntry(size_t reverse_index, char **out_line) {
    if (!out_line) {
        return false;
    }
    *out_line = NULL;
    if (reverse_index >= gShellHistory.count) {
        return false;
    }
    const char *entry = gShellHistory.entries[gShellHistory.count - reverse_index - 1];
    if (!entry) {
        return false;
    }
    char *copy = strdup(entry);
    if (!copy) {
        return false;
    }
    *out_line = copy;
    return true;
}

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

void shellRuntimeInitJobControl(void) {
    shellEnsureJobControl();
}

typedef enum {
    SHELL_HISTORY_EXPAND_OK,
    SHELL_HISTORY_EXPAND_NOT_FOUND,
    SHELL_HISTORY_EXPAND_INVALID
} ShellHistoryExpandResult;

static bool shellIsHistoryTerminator(char c) {
    switch (c) {
        case '\0':
        case ' ':
        case '\t':
        case '\n':
        case '\r':
        case ';':
        case '&':
        case '|':
        case '(':
        case ')':
        case '<':
        case '>':
            return true;
        default:
            return false;
    }
}

static ShellHistoryExpandResult shellExpandHistoryDesignatorAt(const char *input,
                                                               size_t *out_consumed,
                                                               char **out_line) {
    if (out_line) {
        *out_line = NULL;
    }
    if (out_consumed) {
        *out_consumed = 0;
    }
    if (!input || input[0] != '!') {
        return SHELL_HISTORY_EXPAND_INVALID;
    }

    const char *cursor = input + 1;
    if (*cursor == '\0') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        return SHELL_HISTORY_EXPAND_INVALID;
    }

    const char *designator_start = NULL;
    size_t designator_len = 0;
    const char *entry = NULL;

    if (*cursor == '$' || *cursor == '*' || *cursor == '^') {
        designator_start = cursor;
        designator_len = 1;
        cursor++;
        entry = shellHistoryEntryByIndex(-1);
        if (!entry) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor - input);
            }
            return SHELL_HISTORY_EXPAND_NOT_FOUND;
        }
        if (out_consumed) {
            *out_consumed = (size_t)(cursor - input);
        }
        return shellApplyHistoryDesignator(entry, designator_start, designator_len, out_line)
                   ? SHELL_HISTORY_EXPAND_OK
                   : SHELL_HISTORY_EXPAND_INVALID;
    }

    long numeric_index = 0;
    bool has_index = false;
    const char *search_token_start = NULL;
    size_t search_token_len = 0;
    bool search_substring = false;
    bool search_regex = false;

    if (*cursor == '!') {
        numeric_index = -1;
        has_index = true;
        cursor++;
    } else if (*cursor == '-') {
        char *endptr = NULL;
        long value = strtol(cursor + 1, &endptr, 10);
        if (endptr == cursor + 1) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor + 1 - input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
        numeric_index = -value;
        cursor = endptr;
        has_index = true;
    } else if (isdigit((unsigned char)*cursor)) {
        char *endptr = NULL;
        long value = strtol(cursor, &endptr, 10);
        if (endptr == cursor) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor - input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
        numeric_index = value;
        cursor = endptr;
        has_index = true;
    } else if (*cursor == '?') {
        cursor++;
        const char *start = cursor;
        const char *closing = strchr(cursor, '?');
        if (!closing) {
            if (out_consumed) {
                *out_consumed = strlen(input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
        search_token_start = start;
        search_token_len = (size_t)(closing - start);
        if (search_token_len >= 2 && start[0] == '/' && start[search_token_len - 1] == '/') {
            search_regex = true;
            search_token_start = start + 1;
            search_token_len -= 2;
            if (search_token_len == 0) {
                if (out_consumed) {
                    *out_consumed = (size_t)(cursor - input);
                }
                return SHELL_HISTORY_EXPAND_INVALID;
            }
        }
        cursor = closing + 1;
        search_substring = true;
    } else {
        const char *start = cursor;
        while (*cursor && !shellIsHistoryTerminator(*cursor) &&
               *cursor != ':' && *cursor != '$' && *cursor != '^' && *cursor != '*') {
            cursor++;
        }
        if (cursor == start) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor - input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
        search_token_start = start;
        search_token_len = (size_t)(cursor - start);
    }

    if (*cursor == '$' || *cursor == '^' || *cursor == '*') {
        designator_start = cursor;
        designator_len = 1;
        cursor++;
    } else if (*cursor == ':') {
        cursor++;
        designator_start = cursor;
        while (*cursor && !shellIsHistoryTerminator(*cursor)) {
            cursor++;
        }
        designator_len = (size_t)(cursor - designator_start);
        if (designator_len == 0) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor - input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
    }

    if (has_index) {
        entry = shellHistoryEntryByIndex(numeric_index);
    } else if (search_substring) {
        if (search_regex) {
            bool invalid = false;
            entry = shellHistoryFindByRegex(search_token_start, search_token_len, &invalid);
            if (!entry && invalid) {
                if (out_consumed) {
                    *out_consumed = (size_t)(cursor - input);
                }
                return SHELL_HISTORY_EXPAND_INVALID;
            }
        } else {
            entry = shellHistoryFindBySubstring(search_token_start, search_token_len);
        }
    } else {
        entry = shellHistoryFindByPrefix(search_token_start, search_token_len);
    }

    if (!entry) {
        if (out_consumed) {
            *out_consumed = (size_t)(cursor - input);
        }
        return SHELL_HISTORY_EXPAND_NOT_FOUND;
    }

    if (out_consumed) {
        *out_consumed = (size_t)(cursor - input);
    }

    if (designator_start && designator_len > 0) {
        return shellApplyHistoryDesignator(entry, designator_start, designator_len, out_line)
                   ? SHELL_HISTORY_EXPAND_OK
                   : SHELL_HISTORY_EXPAND_INVALID;
    }

    *out_line = strdup(entry);
    if (!*out_line) {
        return SHELL_HISTORY_EXPAND_INVALID;
    }
    return SHELL_HISTORY_EXPAND_OK;
}

bool shellRuntimeExpandHistoryReference(const char *input,
                                        char **out_line,
                                        bool *out_did_expand,
                                        char **out_error_token) {
    if (out_line) {
        *out_line = NULL;
    }
    if (out_did_expand) {
        *out_did_expand = false;
    }
    if (out_error_token) {
        *out_error_token = NULL;
    }
    if (!input || !out_line) {
        return false;
    }

    size_t capacity = strlen(input) + 1;
    if (capacity < 32) {
        capacity = 32;
    }
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        return false;
    }
    buffer[0] = '\0';
    size_t length = 0;

    bool in_single = false;
    bool in_double = false;

    for (size_t i = 0; input[i];) {
        char c = input[i];
        if (c == '\\' && !in_single) {
            if (input[i + 1] == '!') {
                shellBufferAppendChar(&buffer, &length, &capacity, '!');
                i += 2;
                continue;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }
        if (c == '\'') {
            if (!in_double) {
                in_single = !in_single;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }
        if (c == '"') {
            if (!in_single) {
                in_double = !in_double;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }
        if (c == '!' && !in_single) {
            size_t consumed = 0;
            char *replacement = NULL;
            ShellHistoryExpandResult result =
                shellExpandHistoryDesignatorAt(input + i, &consumed, &replacement);
            if (result != SHELL_HISTORY_EXPAND_OK) {
                if (out_error_token) {
                    size_t error_len = consumed > 0 ? consumed : 1;
                    char *token = strndup(input + i, error_len);
                    *out_error_token = token;
                }
                free(replacement);
                free(buffer);
                return false;
            }
            if (replacement) {
                shellBufferAppendString(&buffer, &length, &capacity, replacement);
                free(replacement);
            }
            if (out_did_expand) {
                *out_did_expand = true;
            }
            i += consumed;
            continue;
        }
        shellBufferAppendChar(&buffer, &length, &capacity, c);
        i++;
    }

    *out_line = buffer;
    return true;
}

static bool shellIsRuntimeBuiltin(const char *name) {
    if (!name || !*name) {
        return false;
    }
    static const char *kBuiltins[] = {"cd",     "pwd",     "exit",    "exec",    "export",  "unset",    "setenv",
                                      "unsetenv", "set",    "declare", "trap",    "local",   "break",   "continue", "alias",
                                      "history", "jobs",   "fg",      "finger",  "bg",      "wait",    "builtin",
                                      "source", "read",   "shift",   "return",  "help",    ":"};

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
            shellUpdateStatus(job->last_status);
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

static bool shellResolveJobIndex(VM *vm, const char *name, int arg_count, Value *args, size_t *out_index) {
    if (gShellJobCount == 0) {
        runtimeError(vm, "%s: no current job", name);
        return false;
    }
    if (arg_count == 0) {
        *out_index = gShellJobCount - 1;
        return true;
    }
    if (arg_count > 1) {
        runtimeError(vm, "%s: too many arguments", name);
        return false;
    }

    Value spec = args[0];
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
        if (*end != '\0' || index <= 0 || (size_t)index > gShellJobCount) {
            runtimeError(vm, "%s: invalid job '%s'", name, spec.s_val);
            return false;
        }
        *out_index = (size_t)index - 1;
        return true;
    }

    if (IS_INTLIKE(spec)) {
        long index = (long)AS_INTEGER(spec);
        if (index <= 0 || (size_t)index > gShellJobCount) {
            runtimeError(vm, "%s: invalid job index", name);
            return false;
        }
        *out_index = (size_t)index - 1;
        return true;
    }

    runtimeError(vm, "%s: job spec must be a string or integer", name);
    return false;
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
    char *expanded = shellExpandWord(text, flags, meta, meta_len, &quoted_map, &quoted_len);
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
    if (!shellSplitExpandedWord(expanded, flags, quoted_map, quoted_len, &fields, &field_count)) {
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
        expanded_target = shellExpandWord(target_text, target_flags, target_meta, target_meta_len, NULL, NULL);
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
                char *expanded = shellExpandWord(text, word_flags, word_meta, word_meta_len, &quoted_map, &quoted_len);
                if (!expanded) {
                    ok = false;
                    break;
                }
                char **fields = NULL;
                size_t field_count = 0;
                if (!shellSplitExpandedWord(expanded, word_flags, quoted_map, quoted_len, &fields, &field_count)) {
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
    char *expanded_subject = shellExpandWord(subject_text, subject_flags, subject_meta, subject_meta_len, NULL, NULL);
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
        char *expanded_pattern = shellExpandWord(pattern_text, pattern_flags, pattern_meta, pattern_meta_len, NULL, NULL);
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
        shellSetTrackedVariable("PWD", cwd, false);
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
                for (size_t opt_index = 1; opt_index < option_length && ok; ++opt_index) {
                    char opt = token[opt_index];
                    switch (opt) {
                        case 'r':
                            raw_mode = true;
                            break;
                        case 'p':
                            pending_prompt = true;
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

    if (ok && variable_count == 0) {
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
    if (ok && (read_result == SHELL_READ_LINE_OK || read_result == SHELL_READ_LINE_EOF)) {
        char *cursor = line;
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

Value vmBuiltinShellDeclare(VM *vm, int arg_count, Value *args) {
    bool ok = true;
    bool associative = false;
    bool global_scope = false;
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
                associative = true;
            } else if (opt == 'A' && token[0] == '+') {
                associative = false;
            } else if (opt == 'g' && token[0] == '-') {
                global_scope = true;
            } else if (opt == 'g' && token[0] == '+') {
                global_scope = false;
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
                    setenv(spec, "", 1);
                }
            } else {
                if (!shellSetTrackedVariable(spec, "", false)) {
                    runtimeError(vm, "declare: unable to set '%s'", spec);
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
                runtimeError(vm, "declare: unable to set '%s'", name);
                free(name);
                ok = false;
                break;
            }
        } else {
            if (!shellSetTrackedVariable(name, value_text, false)) {
                runtimeError(vm, "declare: unable to set '%s'", name);
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

Value vmBuiltinShellUnset(VM *vm, int arg_count, Value *args) {
    for (int i = 0; i < arg_count; ++i) {
        if (args[i].type != TYPE_STRING || !args[i].s_val) {
            runtimeError(vm, "unset: expected variable name");
            shellUpdateStatus(1);
            return makeVoid();
        }
        shellUnsetTrackedVariable(args[i].s_val);
    }
    shellUpdateStatus(0);
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
        "cd",
        "Change the current working directory.",
        "cd [dir]",
        "With no arguments cd switches to $HOME. Successful runs update the PWD"
        " environment variable.",
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
        "declare [-a|-A] [name[=value] ...]",
        "Without arguments prints variables with attributes. The -a flag"
        " initialises indexed arrays and -A initialises associative arrays.",
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
        "local",
        "Activate the shell's local scope flag.",
        "local",
        "Sets the runtime flag that marks the current function scope as local-aware."
        " Accepts no arguments.",
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
    printf("Documentation: /usr/local/pscal/docs/exsh_overview.md.\n");
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

Value vmBuiltinShellJobs(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    shellCollectJobs();
    for (size_t i = 0; i < gShellJobCount; ++i) {
        ShellJob *job = &gShellJobs[i];
        const char *state = job->stopped ? "Stopped" : "Running";
        const char *command = job->command ? job->command : "";
        printf("[%zu] %s %s\n", i + 1, state, command);
    }
    fflush(stdout);
    shellUpdateStatus(0);
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
    if (gShellJobCount == 0) {
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
