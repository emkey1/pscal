#include "aether/semantic.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Pascal/globals.h"
#include "aether/parser.h"
#include "rea/semantic.h"

typedef struct AetherFunctionInfo {
    char *name;
    int isPure;
} AetherFunctionInfo;

typedef struct AetherFunctionTable {
    AetherFunctionInfo *items;
    size_t count;
    size_t cap;
} AetherFunctionTable;

typedef struct AetherOpaqueBinding {
    char *name;
    int isDoc;
} AetherOpaqueBinding;

typedef struct AetherOpaqueBindingTable {
    AetherOpaqueBinding *items;
    size_t count;
    size_t cap;
} AetherOpaqueBindingTable;

typedef struct AetherScalarBinding {
    char *name;
    const char *typeName;
} AetherScalarBinding;

typedef struct AetherScalarBindingTable {
    AetherScalarBinding *items;
    size_t count;
    size_t cap;
} AetherScalarBindingTable;

typedef struct AetherScopeFrame {
    int isFx;
    int isFunction;
    int functionIsPure;
    const char *functionName;
} AetherScopeFrame;

static const char *g_aether_source_path = NULL;

static void freeOpaqueBindingTable(AetherOpaqueBindingTable *table) {
    size_t i;

    if (!table) {
        return;
    }
    for (i = 0; i < table->count; i++) {
        free(table->items[i].name);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
}

static void freeScalarBindingTable(AetherScalarBindingTable *table) {
    size_t i;

    if (!table) {
        return;
    }
    for (i = 0; i < table->count; i++) {
        free(table->items[i].name);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
}

static int ensureOpaqueBindingTable(AetherOpaqueBindingTable *table, size_t extra) {
    AetherOpaqueBinding *resized;
    size_t need;
    size_t newCap;

    if (!table) {
        return 0;
    }
    need = table->count + extra;
    if (need <= table->cap) {
        return 1;
    }
    newCap = table->cap ? table->cap * 2 : 16;
    while (newCap < need) {
        newCap *= 2;
    }
    resized = (AetherOpaqueBinding *)realloc(table->items, newCap * sizeof(AetherOpaqueBinding));
    if (!resized) {
        return 0;
    }
    table->items = resized;
    table->cap = newCap;
    return 1;
}

static int ensureScalarBindingTable(AetherScalarBindingTable *table, size_t extra) {
    AetherScalarBinding *resized;
    size_t need;
    size_t newCap;

    if (!table) {
        return 0;
    }
    need = table->count + extra;
    if (need <= table->cap) {
        return 1;
    }
    newCap = table->cap ? table->cap * 2 : 16;
    while (newCap < need) {
        newCap *= 2;
    }
    resized = (AetherScalarBinding *)realloc(table->items, newCap * sizeof(AetherScalarBinding));
    if (!resized) {
        return 0;
    }
    table->items = resized;
    table->cap = newCap;
    return 1;
}

static void freeFunctionTable(AetherFunctionTable *table) {
    size_t i;

    if (!table) {
        return;
    }
    for (i = 0; i < table->count; i++) {
        free(table->items[i].name);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
}

static int ensureFunctionTable(AetherFunctionTable *table, size_t extra) {
    AetherFunctionInfo *resized;
    size_t need;
    size_t newCap;

    if (!table) {
        return 0;
    }
    need = table->count + extra;
    if (need <= table->cap) {
        return 1;
    }
    newCap = table->cap ? table->cap * 2 : 16;
    while (newCap < need) {
        newCap *= 2;
    }
    resized = (AetherFunctionInfo *)realloc(table->items, newCap * sizeof(AetherFunctionInfo));
    if (!resized) {
        return 0;
    }
    table->items = resized;
    table->cap = newCap;
    return 1;
}

static char *dupRange(const char *start, const char *end) {
    char *copy;
    size_t len;

    if (!start || !end || end < start) {
        return NULL;
    }
    len = (size_t)(end - start);
    copy = (char *)malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

static const char *skipInlineSpaces(const char *cursor, const char *lineEnd) {
    while (cursor < lineEnd && (*cursor == ' ' || *cursor == '\t')) {
        cursor++;
    }
    return cursor;
}

static const char *skipQuotedString(const char *cursor, int *line) {
    char quote = *cursor++;

    while (*cursor) {
        if (*cursor == '\n' && line) {
            (*line)++;
        }
        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor += 2;
            continue;
        }
        if (*cursor == quote) {
            cursor++;
            break;
        }
        cursor++;
    }
    return cursor;
}

static int startsWithWord(const char *body, const char *lineEnd, const char *word) {
    size_t len = word ? strlen(word) : 0;

    if (!body || !lineEnd || !word || len == 0) {
        return 0;
    }
    if ((size_t)(lineEnd - body) < len) {
        return 0;
    }
    if (strncmp(body, word, len) != 0) {
        return 0;
    }
    if ((size_t)(lineEnd - body) == len) {
        return 1;
    }
    return isspace((unsigned char)body[len]) || body[len] == '{' || body[len] == ';';
}

static int parseFunctionNameFromLine(const char *body, const char *lineEnd, char **outName) {
    const char *nameStart;
    const char *nameEnd;

    if (!outName) {
        return 0;
    }
    *outName = NULL;
    if (!startsWithWord(body, lineEnd, "fn")) {
        return 0;
    }
    nameStart = skipInlineSpaces(body + 2, lineEnd);
    nameEnd = nameStart;
    while (nameEnd < lineEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
        nameEnd++;
    }
    if (nameEnd == nameStart) {
        return 0;
    }
    *outName = dupRange(nameStart, nameEnd);
    return *outName != NULL;
}

static const AetherFunctionInfo *findFunctionInfo(const AetherFunctionTable *table,
                                                  const char *name,
                                                  size_t len) {
    size_t i;

    if (!table || !name) {
        return NULL;
    }
    for (i = 0; i < table->count; i++) {
        if (strlen(table->items[i].name) == len &&
            strncmp(table->items[i].name, name, len) == 0) {
            return &table->items[i];
        }
    }
    return NULL;
}

static int addFunctionInfo(AetherFunctionTable *table, const char *name, int isPure) {
    size_t i;
    char *copy;

    if (!table || !name) {
        return 0;
    }
    for (i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].name, name) == 0) {
            table->items[i].isPure = isPure;
            return 1;
        }
    }
    if (!ensureFunctionTable(table, 1)) {
        return 0;
    }
    copy = dupRange(name, name + strlen(name));
    if (!copy) {
        return 0;
    }
    table->items[table->count].name = copy;
    table->items[table->count].isPure = isPure;
    table->count++;
    return 1;
}

static int addOpaqueBinding(AetherOpaqueBindingTable *table, const char *name, int isDoc) {
    size_t i;
    char *copy;

    if (!table || !name) {
        return 0;
    }
    for (i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].name, name) == 0) {
            table->items[i].isDoc = isDoc;
            return 1;
        }
    }
    if (!ensureOpaqueBindingTable(table, 1)) {
        return 0;
    }
    copy = dupRange(name, name + strlen(name));
    if (!copy) {
        return 0;
    }
    table->items[table->count].name = copy;
    table->items[table->count].isDoc = isDoc;
    table->count++;
    return 1;
}

static int addScalarBinding(AetherScalarBindingTable *table,
                            const char *name,
                            const char *typeName) {
    size_t i;
    char *copy;

    if (!table || !name || !typeName) {
        return 0;
    }
    for (i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].name, name) == 0) {
            table->items[i].typeName = typeName;
            return 1;
        }
    }
    if (!ensureScalarBindingTable(table, 1)) {
        return 0;
    }
    copy = dupRange(name, name + strlen(name));
    if (!copy) {
        return 0;
    }
    table->items[table->count].name = copy;
    table->items[table->count].typeName = typeName;
    table->count++;
    return 1;
}

static const AetherOpaqueBinding *findOpaqueBinding(const AetherOpaqueBindingTable *table,
                                                    const char *name,
                                                    size_t len) {
    size_t i;

    if (!table || !name) {
        return NULL;
    }
    for (i = 0; i < table->count; i++) {
        if (strlen(table->items[i].name) == len &&
            strncmp(table->items[i].name, name, len) == 0) {
            return &table->items[i];
        }
    }
    return NULL;
}

static const AetherScalarBinding *findScalarBinding(const AetherScalarBindingTable *table,
                                                    const char *name,
                                                    size_t len) {
    size_t i;

    if (!table || !name) {
        return NULL;
    }
    for (i = 0; i < table->count; i++) {
        if (strlen(table->items[i].name) == len &&
            strncmp(table->items[i].name, name, len) == 0) {
            return &table->items[i];
        }
    }
    return NULL;
}

static void collectOpaqueBindings(const char *source, AetherOpaqueBindingTable *table) {
    const char *cursor = source;

    while (cursor && *cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *body;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }
        body = skipInlineSpaces(lineStart, lineEnd);
        if (startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const")) {
            const char *scan = body + (startsWithWord(body, lineEnd, "const") ? 5 : 3);
            const char *nameStart;
            const char *nameEnd;
            const char *colon;
            const char *typeStart;
            const char *typeEnd;
            int isDoc = 0;

            scan = skipInlineSpaces(scan, lineEnd);
            nameStart = scan;
            while (scan < lineEnd && (isalnum((unsigned char)*scan) || *scan == '_')) {
                scan++;
            }
            nameEnd = scan;
            colon = skipInlineSpaces(scan, lineEnd);
            if (nameEnd > nameStart && colon < lineEnd && *colon == ':') {
                typeStart = skipInlineSpaces(colon + 1, lineEnd);
                typeEnd = typeStart;
                while (typeEnd < lineEnd && *typeEnd != '=' && *typeEnd != ';' &&
                       !isspace((unsigned char)*typeEnd)) {
                    typeEnd++;
                }
                if ((size_t)(typeEnd - typeStart) == 7 &&
                    strncmp(typeStart, "ToonDoc", 7) == 0) {
                    isDoc = 1;
                } else if ((size_t)(typeEnd - typeStart) == 8 &&
                           strncmp(typeStart, "ToonNode", 8) == 0) {
                    isDoc = 0;
                } else {
                    cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
                    continue;
                }
                {
                    char *name = dupRange(nameStart, nameEnd);
                    if (name) {
                        addOpaqueBinding(table, name, isDoc);
                        free(name);
                    }
                }
            }
        }
        cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
    }
}

static void collectScalarBindings(const char *source, AetherScalarBindingTable *table) {
    const char *cursor = source;

    while (cursor && *cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *body;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }
        body = skipInlineSpaces(lineStart, lineEnd);
        if (startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const")) {
            const char *scan = body + (startsWithWord(body, lineEnd, "const") ? 5 : 3);
            const char *nameStart;
            const char *nameEnd;
            const char *colon;
            const char *typeStart;
            const char *typeEnd;
            char *name;

            scan = skipInlineSpaces(scan, lineEnd);
            nameStart = scan;
            while (scan < lineEnd && (isalnum((unsigned char)*scan) || *scan == '_')) {
                scan++;
            }
            nameEnd = scan;
            colon = skipInlineSpaces(scan, lineEnd);
            if (nameEnd == nameStart || colon >= lineEnd || *colon != ':') {
                cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
                continue;
            }
            typeStart = skipInlineSpaces(colon + 1, lineEnd);
            typeEnd = typeStart;
            while (typeEnd < lineEnd && *typeEnd != '=' && *typeEnd != ';' &&
                   !isspace((unsigned char)*typeEnd)) {
                typeEnd++;
            }

            name = dupRange(nameStart, nameEnd);
            if (!name) {
                cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
                continue;
            }
            if ((size_t)(typeEnd - typeStart) == 4 && strncmp(typeStart, "Text", 4) == 0) {
                addScalarBinding(table, name, "Text");
            } else if ((size_t)(typeEnd - typeStart) == 4 &&
                       strncmp(typeStart, "TOON", 4) == 0) {
                addScalarBinding(table, name, "TOON");
            } else if ((size_t)(typeEnd - typeStart) == 3 && strncmp(typeStart, "Int", 3) == 0) {
                addScalarBinding(table, name, "Int");
            } else if ((size_t)(typeEnd - typeStart) == 4 &&
                       strncmp(typeStart, "Real", 4) == 0) {
                addScalarBinding(table, name, "Real");
            } else if ((size_t)(typeEnd - typeStart) == 4 &&
                       strncmp(typeStart, "Bool", 4) == 0) {
                addScalarBinding(table, name, "Bool");
            }
            free(name);
        }
        cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
    }
}

static void collectFunctionPurity(const char *source, AetherFunctionTable *table) {
    const char *cursor = source;
    int pendingPure = 0;

    while (cursor && *cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *body;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }
        body = skipInlineSpaces(lineStart, lineEnd);
        if (startsWithWord(body, lineEnd, "@pure")) {
            pendingPure = 1;
        } else if (startsWithWord(body, lineEnd, "@pre") ||
                   startsWithWord(body, lineEnd, "@post") ||
                   startsWithWord(body, lineEnd, "@cost")) {
        } else if (startsWithWord(body, lineEnd, "fn")) {
            char *name = NULL;
            if (parseFunctionNameFromLine(body, lineEnd, &name)) {
                addFunctionInfo(table, name, pendingPure);
                free(name);
            }
            pendingPure = 0;
        } else if (body < lineEnd && !(body[0] == '/' && body + 1 < lineEnd && body[1] == '/')) {
            pendingPure = 0;
        }
        cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
    }
}

static int aetherIsEffectfulBuiltin(const char *name, size_t len) {
    static const char *kEffectfulNames[] = {
        "exit",
        "fetch",
        "flush",
        "fprintf",
        "halt",
        "openaichatcompletions",
        "print",
        "printf",
        "println",
        "read",
        "readln",
        "store",
        "swap",
        "thread_cancel",
        "thread_pause",
        "thread_pool_submit",
        "thread_resume",
        "thread_set_name",
        "thread_spawn_named",
        "write",
        "writeln"
    };
    size_t i;

    for (i = 0; i < sizeof(kEffectfulNames) / sizeof(kEffectfulNames[0]); i++) {
        if (strlen(kEffectfulNames[i]) == len && strncmp(kEffectfulNames[i], name, len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int expectedOpaqueArgKind(const char *name, size_t len, int *expectsDoc) {
    if (!name || !expectsDoc) {
        return 0;
    }
    if ((len == 9 && strncmp(name, "toon_root", len) == 0) ||
        (len == 10 && strncmp(name, "toon_close", len) == 0)) {
        *expectsDoc = 1;
        return 1;
    }
    if ((len == 8 && strncmp(name, "toon_key", len) == 0) ||
        (len == 12 && strncmp(name, "toon_has_key", len) == 0) ||
        (len == 7 && strncmp(name, "toon_at", len) == 0) ||
        (len == 11 && strncmp(name, "toon_has_at", len) == 0) ||
        (len == 8 && strncmp(name, "toon_len", len) == 0) ||
        (len == 9 && strncmp(name, "toon_type", len) == 0) ||
        (len == 9 && strncmp(name, "toon_free", len) == 0) ||
        (len == 12 && strncmp(name, "toon_is_text", len) == 0) ||
        (len == 11 && strncmp(name, "toon_is_int", len) == 0) ||
        (len == 12 && strncmp(name, "toon_is_real", len) == 0) ||
        (len == 12 && strncmp(name, "toon_is_bool", len) == 0) ||
        (len == 12 && strncmp(name, "toon_is_null", len) == 0) ||
        (len == 11 && strncmp(name, "toon_is_arr", len) == 0) ||
        (len == 11 && strncmp(name, "toon_is_obj", len) == 0) ||
        (len == 15 && strncmp(name, "toon_text_value", len) == 0) ||
        (len == 14 && strncmp(name, "toon_int_value", len) == 0) ||
        (len == 15 && strncmp(name, "toon_real_value", len) == 0) ||
        (len == 15 && strncmp(name, "toon_bool_value", len) == 0) ||
        (len == 15 && strncmp(name, "toon_null_value", len) == 0) ||
        (len == 13 && strncmp(name, "toon_get_text", len) == 0) ||
        (len == 16 && strncmp(name, "toon_get_text_or", len) == 0) ||
        (len == 12 && strncmp(name, "toon_get_int", len) == 0) ||
        (len == 15 && strncmp(name, "toon_get_int_or", len) == 0) ||
        (len == 13 && strncmp(name, "toon_get_real", len) == 0) ||
        (len == 16 && strncmp(name, "toon_get_real_or", len) == 0) ||
        (len == 13 && strncmp(name, "toon_get_bool", len) == 0) ||
        (len == 16 && strncmp(name, "toon_get_bool_or", len) == 0)) {
        *expectsDoc = 0;
        return 1;
    }
    return 0;
}

static const char *expectedSecondaryArgTypeName(const char *name, size_t len) {
    if ((len == 8 && strncmp(name, "toon_key", len) == 0) ||
        (len == 12 && strncmp(name, "toon_has_key", len) == 0) ||
        (len == 13 && strncmp(name, "toon_get_text", len) == 0) ||
        (len == 16 && strncmp(name, "toon_get_text_or", len) == 0) ||
        (len == 12 && strncmp(name, "toon_get_int", len) == 0) ||
        (len == 15 && strncmp(name, "toon_get_int_or", len) == 0) ||
        (len == 13 && strncmp(name, "toon_get_real", len) == 0) ||
        (len == 16 && strncmp(name, "toon_get_real_or", len) == 0) ||
        (len == 13 && strncmp(name, "toon_get_bool", len) == 0) ||
        (len == 16 && strncmp(name, "toon_get_bool_or", len) == 0)) {
        return "Text";
    }
    if ((len == 7 && strncmp(name, "toon_at", len) == 0) ||
        (len == 11 && strncmp(name, "toon_has_at", len) == 0)) {
        return "Int";
    }
    return NULL;
}

static const char *expectedPrimaryArgTypeName(const char *name, size_t len) {
    if (len == 10 && strncmp(name, "toon_parse", len) == 0) {
        return "TextOrTOON";
    }
    if (len == 15 && strncmp(name, "toon_parse_file", len) == 0) {
        return "Text";
    }
    return NULL;
}

static int expectedOpaqueReturnKind(const char *name, size_t len, int *returnsDoc) {
    if (!name || !returnsDoc) {
        return 0;
    }
    if (len == 10 && strncmp(name, "toon_parse", len) == 0) {
        *returnsDoc = 1;
        return 1;
    }
    if ((len == 9 && strncmp(name, "toon_root", len) == 0) ||
        (len == 8 && strncmp(name, "toon_key", len) == 0) ||
        (len == 7 && strncmp(name, "toon_at", len) == 0)) {
        *returnsDoc = 0;
        return 1;
    }
    return 0;
}

static const char *expectedScalarReturnTypeName(const char *name, size_t len) {
    if ((len == 9 && strncmp(name, "toon_type", len) == 0) ||
        (len == 13 && strncmp(name, "toon_get_text", len) == 0) ||
        (len == 16 && strncmp(name, "toon_get_text_or", len) == 0) ||
        (len == 15 && strncmp(name, "toon_text_value", len) == 0)) {
        return "Text";
    }
    if ((len == 8 && strncmp(name, "toon_len", len) == 0) ||
        (len == 12 && strncmp(name, "toon_get_int", len) == 0) ||
        (len == 15 && strncmp(name, "toon_get_int_or", len) == 0) ||
        (len == 14 && strncmp(name, "toon_int_value", len) == 0)) {
        return "Int";
    }
    if ((len == 13 && strncmp(name, "toon_get_real", len) == 0) ||
        (len == 16 && strncmp(name, "toon_get_real_or", len) == 0) ||
        (len == 15 && strncmp(name, "toon_real_value", len) == 0)) {
        return "Real";
    }
    if ((len == 13 && strncmp(name, "toon_get_bool", len) == 0) ||
        (len == 16 && strncmp(name, "toon_get_bool_or", len) == 0) ||
        (len == 15 && strncmp(name, "toon_bool_value", len) == 0) ||
        (len == 12 && strncmp(name, "toon_is_text", len) == 0) ||
        (len == 11 && strncmp(name, "toon_is_int", len) == 0) ||
        (len == 12 && strncmp(name, "toon_is_real", len) == 0) ||
        (len == 12 && strncmp(name, "toon_is_bool", len) == 0) ||
        (len == 12 && strncmp(name, "toon_is_null", len) == 0) ||
        (len == 11 && strncmp(name, "toon_is_arr", len) == 0) ||
        (len == 11 && strncmp(name, "toon_is_obj", len) == 0) ||
        (len == 12 && strncmp(name, "toon_has_key", len) == 0) ||
        (len == 11 && strncmp(name, "toon_has_at", len) == 0) ||
        (len == 15 && strncmp(name, "toon_null_value", len) == 0)) {
        return "Bool";
    }
    return NULL;
}

static const char *expectedTertiaryArgTypeName(const char *name, size_t len) {
    if (len == 16 && strncmp(name, "toon_get_text_or", len) == 0) {
        return "Text";
    }
    if (len == 15 && strncmp(name, "toon_get_int_or", len) == 0) {
        return "Int";
    }
    if (len == 16 && strncmp(name, "toon_get_real_or", len) == 0) {
        return "Real";
    }
    if (len == 16 && strncmp(name, "toon_get_bool_or", len) == 0) {
        return "Bool";
    }
    return NULL;
}

static void reportAetherError(const char *kind, int line, const char *detail) {
    fprintf(stderr,
            "%s:%d: Aether %s error: %s\n",
            g_aether_source_path ? g_aether_source_path : "<aether>",
            line,
            kind,
            detail ? detail : "unknown error");
    pascal_semantic_error_count++;
}

static int parseOpaqueTypeName(const char *start, const char *end, int *isDoc) {
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    if ((size_t)(end - start) == 7 && strncmp(start, "ToonDoc", 7) == 0) {
        if (isDoc) {
            *isDoc = 1;
        }
        return 1;
    }
    if ((size_t)(end - start) == 8 && strncmp(start, "ToonNode", 8) == 0) {
        if (isDoc) {
            *isDoc = 0;
        }
        return 1;
    }
    return 0;
}

static void validateOpaqueAssignmentLine(const char *body,
                                         const char *lineEnd,
                                         int line,
                                         const AetherOpaqueBindingTable *opaqueBindings) {
    const char *lhsStart;
    const char *lhsEnd;
    const char *rhsStart;
    const char *rhsEnd;
    const AetherOpaqueBinding *rhsBinding;
    int lhsIsDoc = 0;
    char detail[256];

    if (!body || !lineEnd || !opaqueBindings) {
        return;
    }

    if (startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const")) {
        const char *scan = body + (startsWithWord(body, lineEnd, "const") ? 5 : 3);
        const char *colon;
        const char *typeStart;
        const char *typeEnd;
        const char *equals;

        scan = skipInlineSpaces(scan, lineEnd);
        lhsStart = scan;
        while (scan < lineEnd && (isalnum((unsigned char)*scan) || *scan == '_')) {
            scan++;
        }
        lhsEnd = scan;
        colon = skipInlineSpaces(scan, lineEnd);
        if (lhsEnd == lhsStart || colon >= lineEnd || *colon != ':') {
            return;
        }
        typeStart = skipInlineSpaces(colon + 1, lineEnd);
        equals = typeStart;
        while (equals < lineEnd && *equals != '=') {
            equals++;
        }
        if (equals >= lineEnd || *equals != '=') {
            return;
        }
        typeEnd = equals;
        if (!parseOpaqueTypeName(typeStart, typeEnd, &lhsIsDoc)) {
            return;
        }
        rhsStart = skipInlineSpaces(equals + 1, lineEnd);
    } else {
        const AetherOpaqueBinding *lhsBinding;
        const char *scan = body;
        const char *equals;
        int rhsReturnsDoc = 0;

        lhsStart = scan;
        while (scan < lineEnd && (isalnum((unsigned char)*scan) || *scan == '_')) {
            scan++;
        }
        lhsEnd = scan;
        equals = skipInlineSpaces(scan, lineEnd);
        if (lhsEnd == lhsStart || equals >= lineEnd || *equals != '=') {
            return;
        }
        lhsBinding = findOpaqueBinding(opaqueBindings, lhsStart, (size_t)(lhsEnd - lhsStart));
        if (!lhsBinding) {
            return;
        }
        lhsIsDoc = lhsBinding->isDoc;
        rhsStart = skipInlineSpaces(equals + 1, lineEnd);
        rhsEnd = rhsStart;
        while (rhsEnd < lineEnd && (isalnum((unsigned char)*rhsEnd) || *rhsEnd == '_')) {
            rhsEnd++;
        }
        if (rhsEnd > rhsStart &&
            expectedOpaqueReturnKind(rhsStart, (size_t)(rhsEnd - rhsStart), &rhsReturnsDoc)) {
            if (rhsReturnsDoc != lhsIsDoc) {
                snprintf(detail,
                         sizeof(detail),
                         "binding for '%.*s' must use %s when initialized from '%.*s'.",
                         (int)(lhsEnd - lhsStart),
                         lhsStart,
                         rhsReturnsDoc ? "ToonDoc" : "ToonNode",
                         (int)(rhsEnd - rhsStart),
                         rhsStart);
                reportAetherError("type", line, detail);
            }
            return;
        }
    }

    rhsEnd = rhsStart;
    while (rhsEnd < lineEnd && (isalnum((unsigned char)*rhsEnd) || *rhsEnd == '_')) {
        rhsEnd++;
    }
    if (rhsEnd == rhsStart) {
        return;
    }
    rhsBinding = findOpaqueBinding(opaqueBindings, rhsStart, (size_t)(rhsEnd - rhsStart));
    if (!rhsBinding) {
        return;
    }
    if (rhsBinding->isDoc != lhsIsDoc) {
        snprintf(detail,
                 sizeof(detail),
                 "cannot assign %s handle '%.*s' to %s binding.",
                 rhsBinding->isDoc ? "ToonDoc" : "ToonNode",
                 (int)(rhsEnd - rhsStart),
                 rhsStart,
                 lhsIsDoc ? "ToonDoc" : "ToonNode");
        reportAetherError("type", line, detail);
    }
}

static void validateOpaqueReturnBindingLine(const char *body, const char *lineEnd, int line) {
    const char *scan;
    const char *nameEnd;
    const char *colon;
    const char *typeStart;
    const char *typeEnd;
    const char *equals;
    const char *rhsStart;
    const char *rhsEnd;
    int lhsIsDoc = 0;
    int lhsIsOpaque = 0;
    int rhsIsDoc = 0;
    char detail[256];

    if (!body || !lineEnd) {
        return;
    }
    if (!(startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const"))) {
        return;
    }

    scan = body + (startsWithWord(body, lineEnd, "const") ? 5 : 3);
    scan = skipInlineSpaces(scan, lineEnd);
    while (scan < lineEnd && (isalnum((unsigned char)*scan) || *scan == '_')) {
        scan++;
    }
    nameEnd = scan;
    colon = skipInlineSpaces(scan, lineEnd);
    if (colon >= lineEnd || *colon != ':') {
        return;
    }
    typeStart = skipInlineSpaces(colon + 1, lineEnd);
    equals = typeStart;
    while (equals < lineEnd && *equals != '=') {
        equals++;
    }
    if (equals >= lineEnd || *equals != '=') {
        return;
    }
    typeEnd = equals;
    lhsIsOpaque = parseOpaqueTypeName(typeStart, typeEnd, &lhsIsDoc);

    rhsStart = skipInlineSpaces(equals + 1, lineEnd);
    rhsEnd = rhsStart;
    while (rhsEnd < lineEnd && (isalnum((unsigned char)*rhsEnd) || *rhsEnd == '_')) {
        rhsEnd++;
    }
    if (rhsEnd == rhsStart) {
        return;
    }

    if (!expectedOpaqueReturnKind(rhsStart, (size_t)(rhsEnd - rhsStart), &rhsIsDoc)) {
        return;
    }

    if (!lhsIsOpaque) {
        snprintf(detail,
                 sizeof(detail),
                 "binding for '%.*s' must use %s when initialized from '%.*s'.",
                 (int)(nameEnd - skipInlineSpaces(body + (startsWithWord(body, lineEnd, "const") ? 5 : 3), lineEnd)),
                 skipInlineSpaces(body + (startsWithWord(body, lineEnd, "const") ? 5 : 3), lineEnd),
                 rhsIsDoc ? "ToonDoc" : "ToonNode",
                 (int)(rhsEnd - rhsStart),
                 rhsStart);
        reportAetherError("type", line, detail);
        return;
    }

    if (lhsIsDoc != rhsIsDoc) {
        snprintf(detail,
                 sizeof(detail),
                 "binding for '%.*s' must use %s when initialized from '%.*s'.",
                 (int)(nameEnd - skipInlineSpaces(body + (startsWithWord(body, lineEnd, "const") ? 5 : 3), lineEnd)),
                 skipInlineSpaces(body + (startsWithWord(body, lineEnd, "const") ? 5 : 3), lineEnd),
                 rhsIsDoc ? "ToonDoc" : "ToonNode",
                 (int)(rhsEnd - rhsStart),
                 rhsStart);
        reportAetherError("type", line, detail);
    }
}

static void validateScalarReturnBindingLine(const char *body,
                                            const char *lineEnd,
                                            int line,
                                            const AetherScalarBindingTable *scalarBindings) {
    const char *scan;
    const char *nameStart;
    const char *nameEnd;
    const char *colon;
    const char *typeStart;
    const char *typeEnd;
    const char *equals;
    const char *rhsStart;
    const char *rhsEnd;
    const char *expectedType;
    const AetherScalarBinding *lhsBinding = NULL;
    char detail[256];

    if (!body || !lineEnd) {
        return;
    }
    if (startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const")) {
        scan = body + (startsWithWord(body, lineEnd, "const") ? 5 : 3);
        scan = skipInlineSpaces(scan, lineEnd);
        nameStart = scan;
        while (scan < lineEnd && (isalnum((unsigned char)*scan) || *scan == '_')) {
            scan++;
        }
        nameEnd = scan;
        colon = skipInlineSpaces(scan, lineEnd);
        if (nameEnd == nameStart || colon >= lineEnd || *colon != ':') {
            return;
        }
        typeStart = skipInlineSpaces(colon + 1, lineEnd);
        equals = typeStart;
        while (equals < lineEnd && *equals != '=') {
            equals++;
        }
        if (equals >= lineEnd || *equals != '=') {
            return;
        }
        typeEnd = equals;
        while (typeEnd > typeStart && isspace((unsigned char)typeEnd[-1])) {
            typeEnd--;
        }
    } else {
        const char *equals;

        scan = body;
        nameStart = scan;
        while (scan < lineEnd && (isalnum((unsigned char)*scan) || *scan == '_')) {
            scan++;
        }
        nameEnd = scan;
        equals = skipInlineSpaces(scan, lineEnd);
        if (nameEnd == nameStart || equals >= lineEnd || *equals != '=') {
            return;
        }
        lhsBinding = findScalarBinding(scalarBindings, nameStart, (size_t)(nameEnd - nameStart));
        if (!lhsBinding) {
            return;
        }
        typeStart = lhsBinding->typeName;
        typeEnd = lhsBinding->typeName + strlen(lhsBinding->typeName);
        equals = skipInlineSpaces(scan, lineEnd);
        rhsStart = skipInlineSpaces(equals + 1, lineEnd);
        goto check_rhs;
    }

    rhsStart = skipInlineSpaces(equals + 1, lineEnd);
check_rhs:
    rhsEnd = rhsStart;
    while (rhsEnd < lineEnd && (isalnum((unsigned char)*rhsEnd) || *rhsEnd == '_')) {
        rhsEnd++;
    }
    if (rhsEnd == rhsStart) {
        return;
    }
    expectedType = expectedScalarReturnTypeName(rhsStart, (size_t)(rhsEnd - rhsStart));
    if (!expectedType) {
        return;
    }
    if ((size_t)(typeEnd - typeStart) != strlen(expectedType) ||
        strncmp(typeStart, expectedType, strlen(expectedType)) != 0) {
        snprintf(detail,
                 sizeof(detail),
                 "binding for '%.*s' must use %s when initialized from '%.*s'.",
                 (int)(nameEnd - nameStart),
                 nameStart,
                 expectedType,
                 (int)(rhsEnd - rhsStart),
                 rhsStart);
        reportAetherError("type", line, detail);
    }
}

static void validateScalarAssignmentLine(const char *body,
                                         const char *lineEnd,
                                         int line,
                                         const AetherScalarBindingTable *scalarBindings) {
    const char *scan;
    const char *lhsStart;
    const char *lhsEnd;
    const char *equals;
    const char *rhsStart;
    const char *rhsEnd;
    const AetherScalarBinding *lhsBinding;
    const AetherScalarBinding *rhsBinding;
    char detail[256];

    if (!body || !lineEnd || !scalarBindings) {
        return;
    }
    if (startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const")) {
        return;
    }

    scan = body;
    lhsStart = scan;
    while (scan < lineEnd && (isalnum((unsigned char)*scan) || *scan == '_')) {
        scan++;
    }
    lhsEnd = scan;
    equals = skipInlineSpaces(scan, lineEnd);
    if (lhsEnd == lhsStart || equals >= lineEnd || *equals != '=') {
        return;
    }

    lhsBinding = findScalarBinding(scalarBindings, lhsStart, (size_t)(lhsEnd - lhsStart));
    if (!lhsBinding) {
        return;
    }

    rhsStart = skipInlineSpaces(equals + 1, lineEnd);
    rhsEnd = rhsStart;
    while (rhsEnd < lineEnd && (isalnum((unsigned char)*rhsEnd) || *rhsEnd == '_')) {
        rhsEnd++;
    }
    if (rhsEnd == rhsStart) {
        return;
    }

    rhsBinding = findScalarBinding(scalarBindings, rhsStart, (size_t)(rhsEnd - rhsStart));
    if (!rhsBinding) {
        return;
    }
    if (strcmp(lhsBinding->typeName, rhsBinding->typeName) == 0) {
        return;
    }

    snprintf(detail,
             sizeof(detail),
             "cannot assign %s binding '%.*s' to %s binding '%.*s'.",
             rhsBinding->typeName,
             (int)(rhsEnd - rhsStart),
             rhsStart,
             lhsBinding->typeName,
             (int)(lhsEnd - lhsStart),
             lhsStart);
    reportAetherError("type", line, detail);
}

static int isArithmeticChar(char ch) {
    return ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '%';
}

static const char *previousSignificantChar(const char *lineStart, const char *cursor) {
    const char *scan = cursor;

    while (scan > lineStart) {
        scan--;
        if (!isspace((unsigned char)*scan)) {
            return scan;
        }
    }
    return NULL;
}

static const char *nextSignificantChar(const char *cursor, const char *lineEnd) {
    const char *scan = cursor;

    while (scan < lineEnd) {
        if (!isspace((unsigned char)*scan)) {
            return scan;
        }
        scan++;
    }
    return NULL;
}

static void validateOpaqueCallKind(const char *callName,
                                   size_t callNameLen,
                                   const char *openParen,
                                   const char *lineEnd,
                                   int line,
                                   const AetherOpaqueBindingTable *opaqueBindings) {
    const char *argStart;
    const char *argEnd;
    const AetherOpaqueBinding *binding;
    int expectsDoc = 0;
    char detail[256];

    if (!callName || !openParen || !lineEnd || !opaqueBindings) {
        return;
    }
    if (!expectedOpaqueArgKind(callName, callNameLen, &expectsDoc)) {
        return;
    }
    argStart = skipInlineSpaces(openParen + 1, lineEnd);
    argEnd = argStart;
    while (argEnd < lineEnd && (isalnum((unsigned char)*argEnd) || *argEnd == '_')) {
        argEnd++;
    }
    if (argEnd == argStart) {
        return;
    }
    binding = findOpaqueBinding(opaqueBindings, argStart, (size_t)(argEnd - argStart));
    if (!binding) {
        return;
    }
    if (binding->isDoc != expectsDoc) {
        snprintf(detail,
                 sizeof(detail),
                 "call to '%.*s' expects a %s handle, but '%.*s' is %s.",
                 (int)callNameLen,
                 callName,
                 expectsDoc ? "ToonDoc" : "ToonNode",
                 (int)(argEnd - argStart),
                 argStart,
                 binding->isDoc ? "ToonDoc" : "ToonNode");
        reportAetherError("type", line, detail);
    }
}

static void validateSecondaryHelperArgType(const char *callName,
                                           size_t callNameLen,
                                           const char *openParen,
                                           const char *lineEnd,
                                           int line,
                                           const AetherScalarBindingTable *scalarBindings) {
    const char *expectedType;
    const char *cursor;
    const char *secondStart;
    const char *secondEnd;
    const AetherScalarBinding *binding;
    int depth = 0;
    char detail[256];

    if (!callName || !openParen || !lineEnd || !scalarBindings) {
        return;
    }
    expectedType = expectedSecondaryArgTypeName(callName, callNameLen);
    if (!expectedType) {
        return;
    }

    cursor = openParen + 1;
    while (cursor < lineEnd && *cursor) {
        if (*cursor == '"' || *cursor == '\'') {
            int ignoredLine = line;
            cursor = skipQuotedString(cursor, &ignoredLine);
            continue;
        }
        if (*cursor == '(') {
            depth++;
        } else if (*cursor == ')') {
            if (depth == 0) {
                return;
            }
            depth--;
        } else if (*cursor == ',' && depth == 0) {
            cursor++;
            break;
        }
        cursor++;
    }
    if (cursor >= lineEnd) {
        return;
    }

    secondStart = skipInlineSpaces(cursor, lineEnd);
    secondEnd = secondStart;
    while (secondEnd < lineEnd && (isalnum((unsigned char)*secondEnd) || *secondEnd == '_')) {
        secondEnd++;
    }
    if (secondEnd == secondStart) {
        return;
    }

    binding = findScalarBinding(scalarBindings, secondStart, (size_t)(secondEnd - secondStart));
    if (!binding) {
        return;
    }
    if (strcmp(binding->typeName, expectedType) == 0) {
        return;
    }

    snprintf(detail,
             sizeof(detail),
             "call to '%.*s' expects a %s second argument, but '%.*s' is %s.",
             (int)callNameLen,
             callName,
             expectedType,
             (int)(secondEnd - secondStart),
             secondStart,
             binding->typeName);
    reportAetherError("type", line, detail);
}

static void validateTertiaryHelperArgType(const char *callName,
                                          size_t callNameLen,
                                          const char *openParen,
                                          const char *lineEnd,
                                          int line,
                                          const AetherScalarBindingTable *scalarBindings) {
    const char *expectedType;
    const char *cursor;
    const char *thirdStart = NULL;
    const char *thirdEnd = NULL;
    const AetherScalarBinding *binding;
    int depth = 0;
    int commaCount = 0;
    char detail[256];

    if (!callName || !openParen || !lineEnd || !scalarBindings) {
        return;
    }
    expectedType = expectedTertiaryArgTypeName(callName, callNameLen);
    if (!expectedType) {
        return;
    }

    cursor = openParen + 1;
    while (cursor < lineEnd && *cursor) {
        if (*cursor == '"' || *cursor == '\'') {
            int ignoredLine = line;
            cursor = skipQuotedString(cursor, &ignoredLine);
            continue;
        }
        if (*cursor == '(') {
            depth++;
        } else if (*cursor == ')') {
            if (depth == 0) {
                if (thirdStart) {
                    thirdEnd = cursor;
                }
                break;
            }
            depth--;
        } else if (*cursor == ',' && depth == 0) {
            commaCount++;
            if (commaCount == 2) {
                thirdStart = skipInlineSpaces(cursor + 1, lineEnd);
            } else if (commaCount > 2) {
                break;
            }
        }
        cursor++;
    }
    if (!thirdStart || !thirdEnd || thirdStart >= thirdEnd) {
        return;
    }

    while (thirdEnd > thirdStart && isspace((unsigned char)thirdEnd[-1])) {
        thirdEnd--;
    }
    while (thirdStart < thirdEnd && isspace((unsigned char)*thirdStart)) {
        thirdStart++;
    }
    if (thirdEnd == thirdStart) {
        return;
    }

    {
        const char *identEnd = thirdStart;
        while (identEnd < thirdEnd && (isalnum((unsigned char)*identEnd) || *identEnd == '_')) {
            identEnd++;
        }
        if (identEnd != thirdEnd) {
            return;
        }
        binding = findScalarBinding(scalarBindings, thirdStart, (size_t)(thirdEnd - thirdStart));
    }
    if (!binding) {
        return;
    }
    if (strcmp(binding->typeName, expectedType) == 0) {
        return;
    }

    snprintf(detail,
             sizeof(detail),
             "call to '%.*s' expects a %s third argument, but '%.*s' is %s.",
             (int)callNameLen,
             callName,
             expectedType,
             (int)(thirdEnd - thirdStart),
             thirdStart,
             binding->typeName);
    reportAetherError("type", line, detail);
}

static void validatePrimaryHelperArgType(const char *callName,
                                         size_t callNameLen,
                                         const char *openParen,
                                         const char *lineEnd,
                                         int line,
                                         const AetherScalarBindingTable *scalarBindings) {
    const char *expectedType;
    const char *argStart;
    const char *argEnd;
    const AetherScalarBinding *binding;
    const char *expectedLabel;
    int matches = 0;
    char detail[256];

    if (!callName || !openParen || !lineEnd || !scalarBindings) {
        return;
    }
    expectedType = expectedPrimaryArgTypeName(callName, callNameLen);
    if (!expectedType) {
        return;
    }

    argStart = skipInlineSpaces(openParen + 1, lineEnd);
    argEnd = argStart;
    while (argEnd < lineEnd && (isalnum((unsigned char)*argEnd) || *argEnd == '_')) {
        argEnd++;
    }
    if (argEnd == argStart) {
        return;
    }

    binding = findScalarBinding(scalarBindings, argStart, (size_t)(argEnd - argStart));
    if (!binding) {
        return;
    }

    if (strcmp(expectedType, "TextOrTOON") == 0) {
        matches = strcmp(binding->typeName, "Text") == 0 || strcmp(binding->typeName, "TOON") == 0;
        expectedLabel = "Text or TOON";
    } else {
        matches = strcmp(binding->typeName, expectedType) == 0;
        expectedLabel = expectedType;
    }
    if (matches) {
        return;
    }

    snprintf(detail,
             sizeof(detail),
             "call to '%.*s' expects a %s first argument, but '%.*s' is %s.",
             (int)callNameLen,
             callName,
             expectedLabel,
             (int)(argEnd - argStart),
             argStart,
             binding->typeName);
    reportAetherError("type", line, detail);
}

static void validateAetherSource(const char *source,
                                 const AetherFunctionTable *table,
                                 const AetherOpaqueBindingTable *opaqueBindings,
                                 const AetherScalarBindingTable *scalarBindings) {
    const char *cursor = source;
    int line = 1;
    int blockDepth = 0;
    int fxDepth = 0;
    const char *currentPureFunctionName = NULL;
    int pendingPureAnnotation = 0;
    AetherScopeFrame stack[1024];

    memset(stack, 0, sizeof(stack));

    while (*cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *body;
        const char *scan;
        const char *lastWord = NULL;
        size_t lastWordLen = 0;
        int pendingFx = 0;
        int pendingFunction = 0;
        int pendingFunctionIsPure = 0;
        const char *pendingFunctionName = NULL;
        char *fnName = NULL;
        const AetherFunctionInfo *fnInfo = NULL;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }
        body = skipInlineSpaces(lineStart, lineEnd);
        validateOpaqueAssignmentLine(body, lineEnd, line, opaqueBindings);
        validateOpaqueReturnBindingLine(body, lineEnd, line);
        validateScalarReturnBindingLine(body, lineEnd, line, scalarBindings);
        validateScalarAssignmentLine(body, lineEnd, line, scalarBindings);

        if (startsWithWord(body, lineEnd, "@pure")) {
            pendingPureAnnotation = 1;
        } else if (startsWithWord(body, lineEnd, "@pre") ||
                   startsWithWord(body, lineEnd, "@post") ||
                   startsWithWord(body, lineEnd, "@cost")) {
        } else if (startsWithWord(body, lineEnd, "fn") && parseFunctionNameFromLine(body, lineEnd, &fnName)) {
            fnInfo = findFunctionInfo(table, fnName, strlen(fnName));
            pendingFunction = 1;
            pendingFunctionIsPure = pendingPureAnnotation;
            pendingFunctionName = fnInfo ? fnInfo->name : fnName;
            pendingPureAnnotation = 0;
        } else if (body < lineEnd && !(body[0] == '/' && body + 1 < lineEnd && body[1] == '/')) {
            pendingPureAnnotation = 0;
        }

        scan = lineStart;
        while (scan < lineEnd) {
            if (scan[0] == '/' && scan + 1 < lineEnd && scan[1] == '/') {
                break;
            }
            if (*scan == '"' || *scan == '\'') {
                int ignoredLine = line;
                scan = skipQuotedString(scan, &ignoredLine);
                continue;
            }
            if (*scan == '{') {
                if (blockDepth < (int)(sizeof(stack) / sizeof(stack[0]))) {
                    stack[blockDepth].isFx = pendingFx;
                    stack[blockDepth].isFunction = pendingFunction;
                    stack[blockDepth].functionIsPure = pendingFunctionIsPure;
                    stack[blockDepth].functionName = pendingFunctionName;
                }
                if (pendingFx) {
                    fxDepth++;
                }
                if (pendingFunction && pendingFunctionIsPure) {
                    currentPureFunctionName = pendingFunctionName;
                }
                pendingFx = 0;
                pendingFunction = 0;
                pendingFunctionIsPure = 0;
                pendingFunctionName = NULL;
                blockDepth++;
                lastWord = NULL;
                lastWordLen = 0;
                scan++;
                continue;
            }
            if (*scan == '}') {
                if (blockDepth > 0) {
                    blockDepth--;
                    if (stack[blockDepth].isFx && fxDepth > 0) {
                        fxDepth--;
                    }
                    if (stack[blockDepth].isFunction && stack[blockDepth].functionIsPure) {
                        currentPureFunctionName = NULL;
                    }
                    memset(&stack[blockDepth], 0, sizeof(stack[blockDepth]));
                }
                lastWord = NULL;
                lastWordLen = 0;
                scan++;
                continue;
            }
            if (isalpha((unsigned char)*scan) || *scan == '_') {
                const char *start = scan;
                const char *afterName;
                size_t nameLen;

                scan++;
                while (scan < lineEnd && (isalnum((unsigned char)*scan) || *scan == '_')) {
                    scan++;
                }
                afterName = skipInlineSpaces(scan, lineEnd);
                nameLen = (size_t)(scan - start);

                if (nameLen == 2 && strncmp(start, "fx", 2) == 0 && *afterName == '{') {
                    pendingFx = 1;
                    lastWord = start;
                    lastWordLen = nameLen;
                    continue;
                }

                if (*afterName == '(' &&
                    !(lastWordLen == 2 && strncmp(lastWord, "fn", 2) == 0)) {
                    char detail[256];
                    const AetherFunctionInfo *calleeInfo = findFunctionInfo(table, start, nameLen);
                    int calleeKnown = calleeInfo != NULL;
                    int calleeIsPure = calleeInfo ? calleeInfo->isPure : 0;

                    if (aetherIsEffectfulBuiltin(start, nameLen) && fxDepth == 0) {
                        snprintf(detail,
                                 sizeof(detail),
                                 "call to '%.*s' requires an fx block.",
                                 (int)nameLen,
                                 start);
                        reportAetherError("effect", line, detail);
                    }
                    if (currentPureFunctionName && aetherIsEffectfulBuiltin(start, nameLen)) {
                        snprintf(detail,
                                 sizeof(detail),
                                 "pure function '%s' cannot call effectful builtin '%.*s'.",
                                 currentPureFunctionName,
                                 (int)nameLen,
                                 start);
                        reportAetherError("purity", line, detail);
                    } else if (currentPureFunctionName && calleeKnown && !calleeIsPure) {
                        snprintf(detail,
                                 sizeof(detail),
                                 "pure function '%s' cannot call non-pure function '%.*s'.",
                                 currentPureFunctionName,
                                 (int)nameLen,
                                 start);
                        reportAetherError("purity", line, detail);
                    }
                    validateOpaqueCallKind(start,
                                           nameLen,
                                           afterName,
                                           lineEnd,
                                           line,
                                           opaqueBindings);
                    validatePrimaryHelperArgType(start,
                                                 nameLen,
                                                 afterName,
                                                 lineEnd,
                                                 line,
                                                 scalarBindings);
                    validateSecondaryHelperArgType(start,
                                                   nameLen,
                                                   afterName,
                                                   lineEnd,
                                                   line,
                                                   scalarBindings);
                    validateTertiaryHelperArgType(start,
                                                  nameLen,
                                                  afterName,
                                                  lineEnd,
                                                  line,
                                                  scalarBindings);
                }

                if (findOpaqueBinding(opaqueBindings, start, nameLen) != NULL) {
                    const char *prevSig = previousSignificantChar(lineStart, start);
                    const char *nextSig = nextSignificantChar(scan, lineEnd);
                    char detail[256];

                    if ((prevSig && isArithmeticChar(*prevSig) &&
                         !(prevSig > lineStart && prevSig[-1] == '=')) ||
                        (nextSig && isArithmeticChar(*nextSig))) {
                        snprintf(detail,
                                 sizeof(detail),
                                 "opaque TOON handle '%.*s' cannot be used in arithmetic expressions.",
                                 (int)nameLen,
                                 start);
                        reportAetherError("type", line, detail);
                    }
                }

                lastWord = start;
                lastWordLen = nameLen;
                continue;
            }

            if (!strchr(":@", *scan)) {
                lastWord = NULL;
                lastWordLen = 0;
            }
            scan++;
        }

        free(fnName);
        if (*lineEnd == '\n') {
            line++;
            cursor = lineEnd + 1;
        } else {
            cursor = lineEnd;
        }
    }
}

void aetherPerformSemanticAnalysis(AST *root) {
    const char *source = aetherGetLastSource();

    if (source) {
        AetherFunctionTable table = {0};
        AetherOpaqueBindingTable opaqueBindings = {0};
        AetherScalarBindingTable scalarBindings = {0};
        collectFunctionPurity(source, &table);
        collectOpaqueBindings(source, &opaqueBindings);
        collectScalarBindings(source, &scalarBindings);
        validateAetherSource(source, &table, &opaqueBindings, &scalarBindings);
        freeScalarBindingTable(&scalarBindings);
        freeOpaqueBindingTable(&opaqueBindings);
        freeFunctionTable(&table);
    }
    reaPerformSemanticAnalysis(root);
}

void aetherSemanticSetSourcePath(const char *path) {
    g_aether_source_path = path;
    reaSemanticSetSourcePath(path);
}

int aetherGetLoadedModuleCount(void) {
    return reaGetLoadedModuleCount();
}

AST *aetherGetModuleAST(int index) {
    return reaGetModuleAST(index);
}

const char *aetherGetModulePath(int index) {
    return reaGetModulePath(index);
}

const char *aetherGetModuleName(int index) {
    return reaGetModuleName(index);
}

char *aetherResolveImportPath(const char *path) {
    return reaResolveImportPath(path);
}

void aetherSemanticResetState(void) {
    g_aether_source_path = NULL;
    reaSemanticResetState();
}
