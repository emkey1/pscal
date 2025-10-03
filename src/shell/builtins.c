#include "shell/builtins.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char *name;
    const char *canonical;
    int id;
} ShellBuiltinEntry;

static const ShellBuiltinEntry kShellBuiltins[] = {
    {"cd", "cd", 1},
    {"pwd", "pwd", 2},
    {"echo", "echo", 3},
    {"exit", "exit", 4},
    {"true", "true", 5},
    {"false", "false", 6},
    {"set", "set", 7},
    {"unset", "unset", 8},
    {"export", "export", 9},
    {"read", "read", 10},
    {"test", "test", 11},
    {"shift", "shift", 12},
    {"alias", "alias", 13},
    {"history", "history", 14},
    {"setenv", "setenv", 15},
    {"unsetenv", "unsetenv", 16},
    {"jobs", "jobs", 17},
    {"fg", "fg", 18},
    {"bg", "bg", 19},
    {"wait", "wait", 20},
    {"builtin", "builtin", 21},
    {"__shell_exec", "__shell_exec", 1001},
    {"__shell_pipeline", "__shell_pipeline", 1002},
    {"__shell_and", "__shell_and", 1003},
    {"__shell_or", "__shell_or", 1004},
    {"__shell_subshell", "__shell_subshell", 1005},
    {"__shell_loop", "__shell_loop", 1006},
    {"__shell_if", "__shell_if", 1007},
    {"__shell_case", "__shell_case", 1008},
    {"__shell_case_clause", "__shell_case_clause", 1009},
    {"__shell_case_end", "__shell_case_end", 1010},
    {"__shell_define_function", "__shell_define_function", 1011}
};

static char *shellLowercase(const char *name) {
    if (!name) {
        return NULL;
    }
    size_t len = strlen(name);
    char *lower = (char *)malloc(len + 1);
    if (!lower) {
        return NULL;
    }
    for (size_t i = 0; i < len; ++i) {
        lower[i] = (char)tolower((unsigned char)name[i]);
    }
    lower[len] = '\0';
    return lower;
}

void shellRegisterBuiltins(HashTable *table) {
    if (!table) {
        return;
    }
    size_t builtin_count = sizeof(kShellBuiltins) / sizeof(kShellBuiltins[0]);
    for (size_t i = 0; i < builtin_count; ++i) {
        const ShellBuiltinEntry *entry = &kShellBuiltins[i];
        Symbol *symbol = (Symbol *)calloc(1, sizeof(Symbol));
        if (!symbol) {
            continue;
        }
        char *lower = shellLowercase(entry->canonical);
        symbol->name = lower ? lower : strdup(entry->canonical);
        symbol->type = TYPE_VOID;
        symbol->is_alias = false;
        symbol->is_const = true;
        symbol->is_defined = true;
        symbol->bytecode_address = entry->id;
        symbol->value = NULL;
        symbol->type_def = NULL;
        hashTableInsert(table, symbol);
    }
}

int shellGetBuiltinId(const char *name) {
    if (!name) {
        return -1;
    }
    size_t builtin_count = sizeof(kShellBuiltins) / sizeof(kShellBuiltins[0]);
    for (size_t i = 0; i < builtin_count; ++i) {
        if (strcasecmp(kShellBuiltins[i].name, name) == 0 ||
            strcasecmp(kShellBuiltins[i].canonical, name) == 0) {
            return kShellBuiltins[i].id;
        }
    }
    return -1;
}

const char *shellBuiltinCanonicalName(const char *name) {
    if (!name) {
        return "";
    }
    size_t builtin_count = sizeof(kShellBuiltins) / sizeof(kShellBuiltins[0]);
    for (size_t i = 0; i < builtin_count; ++i) {
        if (strcasecmp(kShellBuiltins[i].name, name) == 0 ||
            strcasecmp(kShellBuiltins[i].canonical, name) == 0) {
            return kShellBuiltins[i].canonical;
        }
    }
    return name;
}

bool shellIsBuiltinName(const char *name) {
    return shellGetBuiltinId(name) >= 0;
}

void shellDumpBuiltins(FILE *out) {
    if (!out) {
        out = stdout;
    }
    size_t builtin_count = sizeof(kShellBuiltins) / sizeof(kShellBuiltins[0]);
    fprintf(out, "Shell builtins (%zu):\n", builtin_count);
    for (size_t i = 0; i < builtin_count; ++i) {
        fprintf(out, "  %s\n", kShellBuiltins[i].name);
    }
}
