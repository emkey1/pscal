#include "backend_ast/builtin.h"
#include "shell_alias.h"
#include "core/utils.h"
#include "vm/vm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration from shell core. */
void shellReportRecoverableError(VM *vm, bool with_location, const char *fmt, ...);
void shellUpdateStatus(int status);

typedef struct {
    char *name;
    char *value;
} ShellAlias;

static ShellAlias *gShellAliases = NULL;
static size_t gShellAliasCount = 0;

static void shellAliasFreeEntry(ShellAlias *alias) {
    if (!alias) {
        return;
    }
    free(alias->name);
    free(alias->value);
    alias->name = NULL;
    alias->value = NULL;
}

static ShellAlias *shellAliasFind(const char *name) {
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

static void shellAliasRemoveAt(size_t index) {
    if (index >= gShellAliasCount) {
        return;
    }
    shellAliasFreeEntry(&gShellAliases[index]);
    if (index + 1 < gShellAliasCount) {
        gShellAliases[index] = gShellAliases[gShellAliasCount - 1];
    }
    gShellAliasCount--;
    if (gShellAliasCount == 0) {
        free(gShellAliases);
        gShellAliases = NULL;
    }
}

static bool shellAliasRemove(const char *name) {
    if (!name) {
        return false;
    }
    for (size_t i = 0; i < gShellAliasCount; ++i) {
        if (strcmp(gShellAliases[i].name, name) == 0) {
            shellAliasRemoveAt(i);
            return true;
        }
    }
    return false;
}

static void shellAliasClearAll(void) {
    if (!gShellAliases) {
        return;
    }
    for (size_t i = 0; i < gShellAliasCount; ++i) {
        shellAliasFreeEntry(&gShellAliases[i]);
    }
    free(gShellAliases);
    gShellAliases = NULL;
    gShellAliasCount = 0;
}

static bool shellAliasSet(const char *name, const char *value) {
    if (!name || !*name) {
        return false;
    }

    ShellAlias *existing = shellAliasFind(name);
    if (existing) {
        char *copy = strdup(value ? value : "");
        if (!copy) {
            return false;
        }
        free(existing->value);
        existing->value = copy;
        return true;
    }

    ShellAlias *resized = realloc(gShellAliases, sizeof(ShellAlias) * (gShellAliasCount + 1));
    if (!resized) {
        return false;
    }
    gShellAliases = resized;
    ShellAlias *alias = &gShellAliases[gShellAliasCount++];
    alias->name = strdup(name);
    alias->value = value ? strdup(value) : strdup("");
    if (!alias->name || !alias->value) {
        shellAliasFreeEntry(alias);
        gShellAliasCount--;
        return false;
    }
    return true;
}

bool shellAliasLookup(const char *name, const char **out_value) {
    ShellAlias *alias = shellAliasFind(name);
    if (!alias) {
        if (out_value) {
            *out_value = NULL;
        }
        return false;
    }
    if (out_value) {
        *out_value = alias->value ? alias->value : "";
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
        if (!shellAliasSet(name, value)) {
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

Value vmBuiltinShellUnalias(VM *vm, int arg_count, Value *args) {
    bool remove_all = false;
    int index = 0;

    while (index < arg_count) {
        Value value = args[index];
        if (value.type != TYPE_STRING || !value.s_val) {
            shellReportRecoverableError(vm, false, "unalias: usage: unalias [-a] name [name ...]");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *arg = value.s_val;
        if (arg[0] != '-' || arg[1] == '\0') {
            break;
        }
        if (strcmp(arg, "-a") == 0) {
            remove_all = true;
            ++index;
            continue;
        }
        shellReportRecoverableError(vm, true, "unalias: %s: invalid option", arg);
        shellReportRecoverableError(vm, false, "unalias: usage: unalias [-a] name [name ...]");
        shellUpdateStatus(1);
        return makeVoid();
    }

    if (remove_all) {
        if (index != arg_count) {
            shellReportRecoverableError(vm, false, "unalias: usage: unalias [-a] name [name ...]");
            shellUpdateStatus(1);
            return makeVoid();
        }
        shellAliasClearAll();
        shellUpdateStatus(0);
        return makeVoid();
    }

    if (index == arg_count) {
        shellReportRecoverableError(vm, false, "unalias: usage: unalias [-a] name [name ...]");
        shellUpdateStatus(1);
        return makeVoid();
    }

    bool all_removed = true;
    for (; index < arg_count; ++index) {
        Value value = args[index];
        if (value.type != TYPE_STRING || !value.s_val) {
            shellReportRecoverableError(vm, false, "unalias: usage: unalias [-a] name [name ...]");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *name = value.s_val;
        if (!shellAliasRemove(name)) {
            shellReportRecoverableError(vm, true, "unalias: %s: not found", name);
            all_removed = false;
        }
    }

    shellUpdateStatus(all_removed ? 0 : 1);
    return makeVoid();
}
