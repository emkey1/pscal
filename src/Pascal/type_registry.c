#include "Pascal/type_registry.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/utils.h"

TypeEntry *findTypeEntry(const char *name) {
    if (!name) {
        return NULL;
    }

    for (TypeEntry *entry = type_table; entry; entry = entry->next) {
        if (entry->name && strcasecmp(entry->name, name) == 0) {
            return entry;
        }
    }

    return NULL;
}

void reserveTypePlaceholder(const char *name, VarType kind) {
    if (!name) {
        return;
    }

    TypeEntry *existing = findTypeEntry(name);
    if (existing) {
        if (!existing->typeAST) {
            AST *placeholder = newASTNode(AST_INTERFACE, NULL);
            setTypeAST(placeholder, kind);
            existing->typeAST = placeholder;
        } else if (existing->typeAST->var_type == TYPE_UNKNOWN && kind != TYPE_UNKNOWN) {
            setTypeAST(existing->typeAST, kind);
        }
        return;
    }

    TypeEntry *entry = malloc(sizeof(TypeEntry));
    if (!entry) {
        EXIT_FAILURE_HANDLER();
        return;
    }

    entry->name = strdup(name);
    if (!entry->name) {
        free(entry);
        EXIT_FAILURE_HANDLER();
        return;
    }

    AST *placeholder = newASTNode(AST_INTERFACE, NULL);
    if (!placeholder) {
        free(entry->name);
        free(entry);
        EXIT_FAILURE_HANDLER();
        return;
    }

    setTypeAST(placeholder, kind);
    entry->typeAST = placeholder;
    entry->next = type_table;
    type_table = entry;
}

void insertType(const char *name, AST *typeAST) {
    if (!name || !typeAST) {
        return;
    }

    TypeEntry *existing = findTypeEntry(name);
    AST *copy = copyAST(typeAST);
    if (!copy) {
        EXIT_FAILURE_HANDLER();
        return;
    }

    if (existing) {
        if (existing->typeAST) {
            freeAST(existing->typeAST);
        }
        existing->typeAST = copy;
        return;
    }

    TypeEntry *entry = malloc(sizeof(TypeEntry));
    if (!entry) {
        freeAST(copy);
        EXIT_FAILURE_HANDLER();
        return;
    }

    entry->name = strdup(name);
    if (!entry->name) {
        free(entry);
        freeAST(copy);
        EXIT_FAILURE_HANDLER();
        return;
    }

    entry->typeAST = copy;
    entry->next = type_table;
    type_table = entry;
}

AST *lookupType(const char *name) {
    TypeEntry *entry = type_table;
    while (entry) {
        if (entry->name && name && strcasecmp(entry->name, name) == 0) {
            return entry->typeAST;
        }
        entry = entry->next;
    }
    return NULL;
}
