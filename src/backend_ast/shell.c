#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "shell/word_encoding.h"
#include "vm/vm.h"
#include "Pascal/globals.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <regex.h>
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

typedef struct {
    char **entries;
    size_t count;
    size_t capacity;
} ShellHistory;

static ShellHistory gShellHistory = {NULL, 0, 0};
static char *gShellArg0 = NULL;

static bool shellDecodeWordSpec(const char *encoded, const char **out_text, uint8_t *out_flags) {
    if (out_text) {
        *out_text = encoded ? encoded : "";
    }
    if (out_flags) {
        *out_flags = 0;
    }
    if (!encoded) {
        return false;
    }
    size_t len = strlen(encoded);
    if (len < 2 || encoded[0] != SHELL_WORD_ENCODE_PREFIX) {
        return false;
    }
    if (out_flags) {
        uint8_t stored = (uint8_t)encoded[1];
        *out_flags = stored > 0 ? (stored - 1) : 0;
    }
    if (out_text) {
        *out_text = encoded + 2;
    }
    return true;
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

static char *shellLookupParameterValue(const char *name, size_t len) {
    if (!name || len == 0) {
        return strdup("");
    }
    if (len == 1) {
        switch (name[0]) {
            case '?': {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d", gShellRuntime.last_status);
                return strdup(buffer);
            }
            case '$': {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d", (int)getpid());
                return strdup(buffer);
            }
            case '#': {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d", gParamCount);
                return strdup(buffer);
            }
            case '*':
            case '@':
                return shellJoinPositionalParameters();
            case '0': {
                if (gShellArg0) {
                    return strdup(gShellArg0);
                }
                return strdup("psh");
            }
            default:
                break;
        }
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
        if (index >= 1 && index <= gParamCount && gParamValues) {
            const char *value = gParamValues[index - 1] ? gParamValues[index - 1] : "";
            return strdup(value);
        }
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

static char *shellExpandParameter(const char *input, size_t *out_consumed) {
    if (out_consumed) {
        *out_consumed = 0;
    }
    if (!input || !*input) {
        return NULL;
    }
    if (*input == '{') {
        const char *cursor = input + 1;
        bool length_only = false;
        if (*cursor == '#') {
            length_only = true;
            cursor++;
        }
        const char *name_start = cursor;
        while (*cursor && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
            cursor++;
        }
        if (*cursor != '}' || cursor == name_start) {
            return NULL;
        }
        size_t name_len = (size_t)(cursor - name_start);
        if (out_consumed) {
            *out_consumed = (size_t)(cursor - input) + 1;
        }
        char *value = shellLookupParameterValue(name_start, name_len);
        if (!value) {
            return NULL;
        }
        if (length_only) {
            size_t val_len = strlen(value);
            free(value);
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%zu", val_len);
            return strdup(buffer);
        }
        return value;
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
        return strdup("psh");
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

static char *shellExpandWord(const char *text, uint8_t flags) {
    if (!text) {
        return strdup("");
    }
    if (flags & SHELL_WORD_FLAG_SINGLE_QUOTED) {
        return strdup(text);
    }
    size_t length = 0;
    size_t capacity = strlen(text) + 1;
    if (capacity < 32) {
        capacity = 32;
    }
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        return NULL;
    }
    buffer[0] = '\0';
    bool double_quoted = (flags & SHELL_WORD_FLAG_DOUBLE_QUOTED) != 0;
    for (size_t i = 0; text[i];) {
        char c = text[i];
        if (c == '\\') {
            if (text[i + 1]) {
                char next = text[i + 1];
                if (!double_quoted || next == '$' || next == '"' || next == '\\' || next == '`' || next == '\n') {
                    shellBufferAppendChar(&buffer, &length, &capacity, next);
                    i += 2;
                    continue;
                }
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }
        if (c == '$') {
            size_t consumed = 0;
            char *expanded = shellExpandParameter(text + i + 1, &consumed);
            if (expanded) {
                shellBufferAppendString(&buffer, &length, &capacity, expanded);
                free(expanded);
                i += consumed + 1;
                continue;
            }
        }
        shellBufferAppendChar(&buffer, &length, &capacity, c);
        i++;
    }
    return buffer;
}

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
    static const char *kBuiltins[] = {"cd", "pwd", "exit", "export", "unset", "setenv", "unsetenv", "alias", "history"};
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
    const char *text = NULL;
    uint8_t flags = 0;
    shellDecodeWordSpec(arg, &text, &flags);
    char *expanded = shellExpandWord(text, flags);
    if (!expanded) {
        return false;
    }
    if (shellWordShouldGlob(flags, expanded)) {
        glob_t glob_result;
        int glob_status = glob(expanded, 0, NULL, &glob_result);
        if (glob_status == 0) {
            size_t original_argc = cmd->argc;
            bool ok = true;
            for (size_t i = 0; i < glob_result.gl_pathc; ++i) {
                char *dup = strdup(glob_result.gl_pathv[i]);
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
                free(expanded);
                return false;
            }
            globfree(&glob_result);
            free(expanded);
            return true;
        }
        if (glob_status != GLOB_NOMATCH) {
            fprintf(stderr, "psh: glob failed for '%s'\n", expanded);
        }
    }
    if (!shellCommandAppendArgOwned(cmd, expanded)) {
        return false;
    }
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
    const char *target_spec = parts[2];
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
    const char *target_text = NULL;
    uint8_t target_flags = 0;
    shellDecodeWordSpec(target_spec, &target_text, &target_flags);
    redir->path = shellExpandWord(target_text, target_flags);
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
    if (setenv(args[0].s_val, value, 1) != 0) {
        runtimeError(vm, "setenv: unable to set '%s': %s", args[0].s_val, strerror(errno));
        shellUpdateStatus(1);
        return makeVoid();
    }
    shellUpdateStatus(0);
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

Value vmBuiltinShellUnsetenv(VM *vm, int arg_count, Value *args) {
    return vmBuiltinShellUnset(vm, arg_count, args);
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
