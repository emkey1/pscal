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

typedef struct AetherScopeFrame {
    int isFx;
    int isFunction;
    int functionIsPure;
    const char *functionName;
} AetherScopeFrame;

static const char *g_aether_source_path = NULL;

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
        "printf",
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

static void reportAetherError(const char *kind, int line, const char *detail) {
    fprintf(stderr,
            "%s:%d: Aether %s error: %s\n",
            g_aether_source_path ? g_aether_source_path : "<aether>",
            line,
            kind,
            detail ? detail : "unknown error");
    pascal_semantic_error_count++;
}

static void validateAetherSource(const char *source, const AetherFunctionTable *table) {
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
        collectFunctionPurity(source, &table);
        validateAetherSource(source, &table);
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
