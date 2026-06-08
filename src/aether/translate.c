#include "aether/translate.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct Buffer {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

static int bufferEnsure(Buffer *buf, size_t extra) {
    if (!buf) {
        return 0;
    }
    size_t need = buf->len + extra + 1;
    if (need <= buf->cap) {
        return 1;
    }
    size_t newCap = buf->cap ? buf->cap : 256;
    while (newCap < need) {
        newCap *= 2;
    }
    char *resized = (char *)realloc(buf->data, newCap);
    if (!resized) {
        return 0;
    }
    buf->data = resized;
    buf->cap = newCap;
    return 1;
}

static int bufferAppendN(Buffer *buf, const char *text, size_t len) {
    if (!buf || (!text && len > 0)) {
        return 0;
    }
    if (!bufferEnsure(buf, len)) {
        return 0;
    }
    if (len > 0) {
        memcpy(buf->data + buf->len, text, len);
        buf->len += len;
    }
    buf->data[buf->len] = '\0';
    return 1;
}

static int bufferAppend(Buffer *buf, const char *text) {
    return bufferAppendN(buf, text, text ? strlen(text) : 0);
}

static char *dupRange(const char *start, const char *end) {
    if (!start || !end || end < start) {
        return NULL;
    }
    size_t len = (size_t)(end - start);
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

static char *dupCString(const char *text) {
    return text ? dupRange(text, text + strlen(text)) : NULL;
}

static char *readTextFile(const char *path) {
    FILE *fp;
    long size;
    char *buffer;

    if (!path) {
        return NULL;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(fp);
        return NULL;
    }
    if (size > 0 && fread(buffer, 1, (size_t)size, fp) != (size_t)size) {
        free(buffer);
        fclose(fp);
        return NULL;
    }
    buffer[size] = '\0';
    fclose(fp);
    return buffer;
}

static char *trimmedCopy(const char *start, const char *end) {
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    return dupRange(start, end);
}

static const char *mapTypeName(const char *typeName) {
    if (!typeName || !*typeName) {
        return typeName;
    }
    if (strcmp(typeName, "Int") == 0) {
        return "int";
    }
    if (strcmp(typeName, "Real") == 0) {
        return "float";
    }
    if (strcmp(typeName, "Text") == 0) {
        return "str";
    }
    if (strcmp(typeName, "Bool") == 0) {
        return "bool";
    }
    if (strcmp(typeName, "Void") == 0) {
        return "void";
    }
    if (strcmp(typeName, "TOON") == 0) {
        return "str";
    }
    if (strcmp(typeName, "ToonDoc") == 0) {
        return "int";
    }
    if (strcmp(typeName, "ToonNode") == 0) {
        return "int";
    }
    return typeName;
}

static int appendMappedType(Buffer *buf, const char *start, const char *end) {
    char *trimmed = trimmedCopy(start, end);
    int ok;
    if (!trimmed) {
        return 0;
    }
    ok = bufferAppend(buf, mapTypeName(trimmed));
    free(trimmed);
    return ok;
}

static const char *skipSpaces(const char *p) {
    while (p && (*p == ' ' || *p == '\t')) {
        p++;
    }
    return p;
}

static const char *skipSpacesInRange(const char *p, const char *end) {
    while (p && p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }
    return p;
}

static char *resolveRelativePath(const char *sourcePath, const char *importPath) {
    char combined[PATH_MAX];
    char resolved[PATH_MAX];
    const char *slash;
    size_t dirLen;

    if (!importPath || !*importPath) {
        return NULL;
    }
    if (importPath[0] == '/') {
        if (realpath(importPath, resolved)) {
            return dupCString(resolved);
        }
        return dupCString(importPath);
    }
    if (!sourcePath || !*sourcePath) {
        return dupCString(importPath);
    }
    slash = strrchr(sourcePath, '/');
    if (!slash) {
        return dupCString(importPath);
    }
    dirLen = (size_t)(slash - sourcePath);
    if (dirLen + 1 + strlen(importPath) + 1 > sizeof(combined)) {
        return NULL;
    }
    memcpy(combined, sourcePath, dirLen);
    combined[dirLen] = '/';
    strcpy(combined + dirLen + 1, importPath);
    if (realpath(combined, resolved)) {
        return dupCString(resolved);
    }
    return dupCString(combined);
}

static int hasTypedDeclSeparator(const char *body, const char *lineEnd, int isConst) {
    const char *cursor;
    const char *nameEnd;
    const char *marker;

    if (!body || !lineEnd) {
        return 0;
    }
    cursor = body + (isConst ? 5 : 3);
    cursor = skipSpacesInRange(cursor, lineEnd);
    while (cursor < lineEnd && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    nameEnd = cursor;
    marker = skipSpacesInRange(nameEnd, lineEnd);
    return marker < lineEnd && *marker == ':';
}

static int appendIdentifier(Buffer *buf, const char *start, const char *end) {
    char *trimmed = trimmedCopy(start, end);
    int ok;
    if (!trimmed) {
        return 0;
    }
    ok = bufferAppend(buf, trimmed);
    free(trimmed);
    return ok;
}

static const char *findCharInRange(const char *start, const char *end, char target) {
    const char *cursor = start;
    while (cursor < end) {
        if (*cursor == target) {
            return cursor;
        }
        cursor++;
    }
    return NULL;
}

static const char *findLastCharInRange(const char *start, const char *end, char target) {
    const char *cursor = end;
    while (cursor > start) {
        cursor--;
        if (*cursor == target) {
            return cursor;
        }
    }
    return NULL;
}

static const char *findSubstringInRange(const char *start, const char *end, const char *needle) {
    size_t needleLen = needle ? strlen(needle) : 0;
    const char *cursor = start;
    if (needleLen == 0 || !needle) {
        return NULL;
    }
    while (cursor + needleLen <= end) {
        if (strncmp(cursor, needle, needleLen) == 0) {
            return cursor;
        }
        cursor++;
    }
    return NULL;
}

static int startsWithWord(const char *body, const char *lineEnd, const char *word);

static int isIdentifierChar(unsigned char ch) {
    return isalnum(ch) || ch == '_';
}

static int bufferAppendEscapedStringLiteral(Buffer *buf, const char *text, size_t len) {
    size_t i;

    if (!buf || (!text && len > 0)) {
        return 0;
    }
    if (!bufferAppend(buf, "\"")) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];

        if (ch == '\\') {
            if (!bufferAppend(buf, "\\\\")) {
                return 0;
            }
        } else if (ch == '"') {
            if (!bufferAppend(buf, "\\\"")) {
                return 0;
            }
        } else if (ch == '\n') {
            if (!bufferAppend(buf, "\\n")) {
                return 0;
            }
        } else if (ch == '\r') {
            if (!bufferAppend(buf, "\\r")) {
                return 0;
            }
        } else if (ch == '\t') {
            if (!bufferAppend(buf, "\\t")) {
                return 0;
            }
        } else if (ch < 0x20) {
            char escaped[7];

            snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
            if (!bufferAppend(buf, escaped)) {
                return 0;
            }
        } else {
            if (!bufferAppendN(buf, (const char *)&text[i], 1)) {
                return 0;
            }
        }
    }
    return bufferAppend(buf, "\"");
}

static const char *findToonMarker(const char *lineStart, const char *lineEnd) {
    const char *cursor = lineStart;
    int inString = 0;
    char quote = '\0';

    while (cursor + 5 <= lineEnd) {
        if (!inString && cursor[0] == '/' && cursor + 1 < lineEnd && cursor[1] == '/') {
            break;
        }
        if (inString) {
            if (*cursor == '\\' && cursor + 1 < lineEnd) {
                cursor += 2;
                continue;
            }
            if (*cursor == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (*cursor == '"' || *cursor == '\'') {
            inString = 1;
            quote = *cursor;
            cursor++;
            continue;
        }
        if (strncmp(cursor, "toon:", 5) == 0 &&
            (cursor == lineStart || !isIdentifierChar((unsigned char)cursor[-1])) &&
            (cursor + 5 == lineEnd || isspace((unsigned char)cursor[5]) ||
             cursor[5] == '/' || cursor[5] == ';')) {
            return cursor;
        }
        cursor++;
    }
    return NULL;
}

static size_t leadingIndentWidth(const char *lineStart, const char *lineEnd) {
    const char *cursor = lineStart;

    while (cursor < lineEnd && (*cursor == ' ' || *cursor == '\t')) {
        cursor++;
    }
    return (size_t)(cursor - lineStart);
}

static char *preprocessToonBlocks(const char *source, const char *path) {
    const char *cursor = source;
    Buffer out = {0};

    if (!source) {
        return NULL;
    }

    while (*cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *toonMarker;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }

        toonMarker = findToonMarker(lineStart, lineEnd);
        if (!toonMarker) {
            if (!bufferAppendN(&out, lineStart, (size_t)(lineEnd - lineStart))) {
                free(out.data);
                return NULL;
            }
            if (*lineEnd == '\n' && !bufferAppendN(&out, "\n", 1)) {
                free(out.data);
                return NULL;
            }
            cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
            continue;
        }

        {
            const char *afterMarker = toonMarker + 5;
            const char *scan = afterMarker;
            const char *blockStart;
            const char *blockCursor;
            const char *blockEnd;
            const char *lastContentEnd = NULL;
            size_t markerIndent = leadingIndentWidth(lineStart, lineEnd);
            size_t baseIndent = 0;
            int sawContent = 0;
            Buffer toonText = {0};

            while (scan < lineEnd) {
                if (*scan == '/' && scan + 1 < lineEnd && scan[1] == '/') {
                    break;
                }
                if (!isspace((unsigned char)*scan)) {
                    fprintf(stderr,
                            "%s: Aether TOON rewrite error: only whitespace or comments may follow 'toon:'.\n",
                            path ? path : "<aether>");
                    free(out.data);
                    return NULL;
                }
                scan++;
            }

            blockStart = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
            blockCursor = blockStart;

            while (*blockCursor) {
                const char *blockLineStart = blockCursor;
                const char *blockLineEnd = blockCursor;
                size_t blockIndent;
                const char *trimmed;

                while (*blockLineEnd && *blockLineEnd != '\n') {
                    blockLineEnd++;
                }
                blockIndent = leadingIndentWidth(blockLineStart, blockLineEnd);
                trimmed = blockLineStart + blockIndent;

                if (trimmed >= blockLineEnd) {
                    blockCursor = *blockLineEnd == '\n' ? blockLineEnd + 1 : blockLineEnd;
                    continue;
                }
                if (blockIndent <= markerIndent) {
                    break;
                }
                if (!sawContent || blockIndent < baseIndent) {
                    baseIndent = blockIndent;
                }
                sawContent = 1;
                lastContentEnd = *blockLineEnd == '\n' ? blockLineEnd + 1 : blockLineEnd;
                blockCursor = *blockLineEnd == '\n' ? blockLineEnd + 1 : blockLineEnd;
            }

            if (!sawContent) {
                fprintf(stderr,
                        "%s: Aether TOON rewrite error: 'toon:' must be followed by an indented TOON block.\n",
                        path ? path : "<aether>");
                free(out.data);
                return NULL;
            }

            blockEnd = lastContentEnd ? lastContentEnd : blockCursor;
            blockCursor = blockStart;
            while (blockCursor < blockEnd) {
                const char *blockLineStart = blockCursor;
                const char *blockLineEnd = blockCursor;
                const char *nextBlockCursor;
                size_t blockIndent;
                const char *contentStart;

                while (*blockLineEnd && *blockLineEnd != '\n') {
                    blockLineEnd++;
                }
                nextBlockCursor = *blockLineEnd == '\n' ? blockLineEnd + 1 : blockLineEnd;
                blockIndent = leadingIndentWidth(blockLineStart, blockLineEnd);
                contentStart = blockLineStart;
                if (blockIndent >= baseIndent) {
                    contentStart = blockLineStart + baseIndent;
                }
                if (!bufferAppendN(&toonText, contentStart, (size_t)(blockLineEnd - contentStart))) {
                    free(toonText.data);
                    free(out.data);
                    return NULL;
                }
                if (nextBlockCursor < blockEnd && *blockLineEnd == '\n' &&
                    !bufferAppendN(&toonText, "\n", 1)) {
                    free(toonText.data);
                    free(out.data);
                    return NULL;
                }
                blockCursor = nextBlockCursor;
            }

            if (!bufferAppendN(&out, lineStart, (size_t)(toonMarker - lineStart)) ||
                !bufferAppendEscapedStringLiteral(&out, toonText.data, toonText.len) ||
                !bufferAppend(&out, ";")) {
                free(toonText.data);
                free(out.data);
                return NULL;
            }
            free(toonText.data);

            if (*lineEnd == '\n' && !bufferAppendN(&out, "\n", 1)) {
                free(out.data);
                return NULL;
            }

            blockCursor = blockStart;
            while (blockCursor < blockEnd) {
                const char *blockLineEnd = blockCursor;

                while (*blockLineEnd && *blockLineEnd != '\n') {
                    blockLineEnd++;
                }
                if (*blockLineEnd == '\n' && !bufferAppendN(&out, "\n", 1)) {
                    free(out.data);
                    return NULL;
                }
                blockCursor = *blockLineEnd == '\n' ? blockLineEnd + 1 : blockLineEnd;
            }

            cursor = blockEnd;
        }
    }

    return out.data;
}

typedef struct PendingContracts {
    char *preExpr;
    char *postExpr;
} PendingContracts;

typedef struct FunctionContracts {
    int active;
    int bodyDepth;
    int isVoid;
    char *name;
    char *postExpr;
} FunctionContracts;

typedef struct ParBlockState {
    int active;
    int bodyDepth;
    int nextHandle;
    char *indent;
    Buffer joinLines;
} ParBlockState;

typedef struct TypeBlockState {
    int active;
    int bodyDepth;
} TypeBlockState;

typedef struct JsonAliasState {
    int needed;
    int alreadyImported;
} JsonAliasState;

typedef struct AetherBinding {
    char *name;
    char *typeName;
} AetherBinding;

typedef struct AetherBindingTable {
    AetherBinding *items;
    size_t count;
    size_t cap;
} AetherBindingTable;

typedef struct AetherFunctionSig {
    char *name;
    char *returnType;
} AetherFunctionSig;

typedef struct AetherFunctionTable {
    AetherFunctionSig *items;
    size_t count;
    size_t cap;
} AetherFunctionTable;

typedef struct ToonLiteralBinding {
    char *name;
    char *literal;
} ToonLiteralBinding;

typedef struct ToonLiteralTable {
    ToonLiteralBinding *items;
    size_t count;
    size_t cap;
} ToonLiteralTable;

static void freePendingContracts(PendingContracts *pending) {
    if (!pending) {
        return;
    }
    free(pending->preExpr);
    free(pending->postExpr);
    pending->preExpr = NULL;
    pending->postExpr = NULL;
}

static void clearFunctionContracts(FunctionContracts *state) {
    if (!state) {
        return;
    }
    free(state->name);
    free(state->postExpr);
    state->active = 0;
    state->bodyDepth = 0;
    state->isVoid = 0;
    state->name = NULL;
    state->postExpr = NULL;
}

static void clearParBlockState(ParBlockState *state) {
    if (!state) {
        return;
    }
    free(state->indent);
    free(state->joinLines.data);
    state->active = 0;
    state->bodyDepth = 0;
    state->nextHandle = 0;
    state->indent = NULL;
    state->joinLines.data = NULL;
    state->joinLines.len = 0;
    state->joinLines.cap = 0;
}

static void clearTypeBlockState(TypeBlockState *state) {
    if (!state) {
        return;
    }
    state->active = 0;
    state->bodyDepth = 0;
}

static void freeAetherBindingTable(AetherBindingTable *table) {
    size_t i;

    if (!table) {
        return;
    }
    for (i = 0; i < table->count; i++) {
        free(table->items[i].name);
        free(table->items[i].typeName);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
}

static void freeAetherFunctionTable(AetherFunctionTable *table) {
    size_t i;

    if (!table) {
        return;
    }
    for (i = 0; i < table->count; i++) {
        free(table->items[i].name);
        free(table->items[i].returnType);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
}

static int ensureAetherBindingTable(AetherBindingTable *table, size_t extra) {
    AetherBinding *resized;
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
    resized = (AetherBinding *)realloc(table->items, newCap * sizeof(AetherBinding));
    if (!resized) {
        return 0;
    }
    table->items = resized;
    table->cap = newCap;
    return 1;
}

static int ensureAetherFunctionTable(AetherFunctionTable *table, size_t extra) {
    AetherFunctionSig *resized;
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
    resized = (AetherFunctionSig *)realloc(table->items, newCap * sizeof(AetherFunctionSig));
    if (!resized) {
        return 0;
    }
    table->items = resized;
    table->cap = newCap;
    return 1;
}

static int setAetherBindingType(AetherBindingTable *table, const char *name, const char *typeName) {
    size_t i;
    char *nameCopy;
    char *typeCopy;

    if (!table || !name || !typeName) {
        return 0;
    }
    for (i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].name, name) == 0) {
            typeCopy = dupRange(typeName, typeName + strlen(typeName));
            if (!typeCopy) {
                return 0;
            }
            free(table->items[i].typeName);
            table->items[i].typeName = typeCopy;
            return 1;
        }
    }
    if (!ensureAetherBindingTable(table, 1)) {
        return 0;
    }
    nameCopy = dupRange(name, name + strlen(name));
    typeCopy = dupRange(typeName, typeName + strlen(typeName));
    if (!nameCopy || !typeCopy) {
        free(nameCopy);
        free(typeCopy);
        return 0;
    }
    table->items[table->count].name = nameCopy;
    table->items[table->count].typeName = typeCopy;
    table->count++;
    return 1;
}

static int setAetherFunctionReturnType(AetherFunctionTable *table,
                                       const char *name,
                                       const char *returnType) {
    size_t i;
    char *nameCopy;
    char *typeCopy;

    if (!table || !name || !returnType) {
        return 0;
    }
    for (i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].name, name) == 0) {
            typeCopy = dupRange(returnType, returnType + strlen(returnType));
            if (!typeCopy) {
                return 0;
            }
            free(table->items[i].returnType);
            table->items[i].returnType = typeCopy;
            return 1;
        }
    }
    if (!ensureAetherFunctionTable(table, 1)) {
        return 0;
    }
    nameCopy = dupRange(name, name + strlen(name));
    typeCopy = dupRange(returnType, returnType + strlen(returnType));
    if (!nameCopy || !typeCopy) {
        free(nameCopy);
        free(typeCopy);
        return 0;
    }
    table->items[table->count].name = nameCopy;
    table->items[table->count].returnType = typeCopy;
    table->count++;
    return 1;
}

static const char *findAetherBindingType(const AetherBindingTable *table, const char *name, size_t len) {
    size_t i;

    if (!table || !name) {
        return NULL;
    }
    for (i = 0; i < table->count; i++) {
        if (strlen(table->items[i].name) == len &&
            strncmp(table->items[i].name, name, len) == 0) {
            return table->items[i].typeName;
        }
    }
    return NULL;
}

static const char *findAetherFunctionReturnType(const AetherFunctionTable *table,
                                                const char *name,
                                                size_t len) {
    size_t i;

    if (!table || !name) {
        return NULL;
    }
    for (i = 0; i < table->count; i++) {
        if (strlen(table->items[i].name) == len &&
            strncmp(table->items[i].name, name, len) == 0) {
            return table->items[i].returnType;
        }
    }
    return NULL;
}

static void freeToonLiteralTable(ToonLiteralTable *table) {
    size_t i;

    if (!table) {
        return;
    }
    for (i = 0; i < table->count; i++) {
        free(table->items[i].name);
        free(table->items[i].literal);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
}

static int ensureToonLiteralTable(ToonLiteralTable *table, size_t extra) {
    ToonLiteralBinding *resized;
    size_t need;
    size_t newCap;

    if (!table) {
        return 0;
    }
    need = table->count + extra;
    if (need <= table->cap) {
        return 1;
    }
    newCap = table->cap ? table->cap * 2 : 8;
    while (newCap < need) {
        newCap *= 2;
    }
    resized = (ToonLiteralBinding *)realloc(table->items, newCap * sizeof(ToonLiteralBinding));
    if (!resized) {
        return 0;
    }
    table->items = resized;
    table->cap = newCap;
    return 1;
}

static int setToonLiteralBinding(ToonLiteralTable *table, const char *name, const char *literal) {
    size_t i;
    char *nameCopy;
    char *literalCopy;

    if (!table || !name || !literal) {
        return 0;
    }
    for (i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].name, name) == 0) {
            literalCopy = dupRange(literal, literal + strlen(literal));
            if (!literalCopy) {
                return 0;
            }
            free(table->items[i].literal);
            table->items[i].literal = literalCopy;
            return 1;
        }
    }
    if (!ensureToonLiteralTable(table, 1)) {
        return 0;
    }
    nameCopy = dupRange(name, name + strlen(name));
    literalCopy = dupRange(literal, literal + strlen(literal));
    if (!nameCopy || !literalCopy) {
        free(nameCopy);
        free(literalCopy);
        return 0;
    }
    table->items[table->count].name = nameCopy;
    table->items[table->count].literal = literalCopy;
    table->count++;
    return 1;
}

static const char *findToonLiteralBinding(const ToonLiteralTable *table, const char *name, size_t len) {
    size_t i;

    if (!table || !name) {
        return NULL;
    }
    for (i = 0; i < table->count; i++) {
        if (strlen(table->items[i].name) == len &&
            strncmp(table->items[i].name, name, len) == 0) {
            return table->items[i].literal;
        }
    }
    return NULL;
}

static void clearToonLiteralBinding(ToonLiteralTable *table, const char *name, size_t len) {
    size_t i;

    if (!table || !name) {
        return;
    }
    for (i = 0; i < table->count; i++) {
        if (strlen(table->items[i].name) == len &&
            strncmp(table->items[i].name, name, len) == 0) {
            free(table->items[i].name);
            free(table->items[i].literal);
            if (i + 1 < table->count) {
                memmove(&table->items[i],
                        &table->items[i + 1],
                        (table->count - i - 1) * sizeof(ToonLiteralBinding));
            }
            table->count--;
            return;
        }
    }
}

static int isSupportedToonBindingType(const char *typeStart, const char *typeEnd) {
    size_t len;

    if (!typeStart || !typeEnd || typeEnd < typeStart) {
        return 0;
    }
    while (typeStart < typeEnd && isspace((unsigned char)*typeStart)) {
        typeStart++;
    }
    while (typeEnd > typeStart && isspace((unsigned char)typeEnd[-1])) {
        typeEnd--;
    }
    len = (size_t)(typeEnd - typeStart);
    return (len == 4 && strncmp(typeStart, "TOON", len) == 0) ||
           (len == 4 && strncmp(typeStart, "Text", len) == 0);
}

static int isAetherBoolLiteral(const char *start, const char *end) {
    size_t len;

    if (!start || !end || end <= start) {
        return 0;
    }
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    len = (size_t)(end - start);
    return (len == 4 && strncmp(start, "true", 4) == 0) ||
           (len == 5 && strncmp(start, "false", 5) == 0);
}

static int inferNumericLiteralType(const char *start, const char *end, const char **outTypeName) {
    const char *cursor;
    int sawDigit = 0;
    int sawDot = 0;
    int sawExp = 0;

    if (!start || !end || !outTypeName) {
        return 0;
    }
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    if (start >= end) {
        return 0;
    }
    cursor = start;
    if (*cursor == '+' || *cursor == '-') {
        cursor++;
    }
    while (cursor < end) {
        if (isdigit((unsigned char)*cursor)) {
            sawDigit = 1;
            cursor++;
            continue;
        }
        if (*cursor == '.' && !sawDot && !sawExp) {
            sawDot = 1;
            cursor++;
            continue;
        }
        if ((*cursor == 'e' || *cursor == 'E') && !sawExp && sawDigit) {
            sawExp = 1;
            sawDot = 1;
            cursor++;
            if (cursor < end && (*cursor == '+' || *cursor == '-')) {
                cursor++;
            }
            continue;
        }
        return 0;
    }
    if (!sawDigit) {
        return 0;
    }
    *outTypeName = sawDot ? "Real" : "Int";
    return 1;
}

static const char *inferHelperReturnTypeName(const char *nameStart, size_t nameLen) {
    if (!nameStart || nameLen == 0) {
        return NULL;
    }
    if ((nameLen == 10 && strncmp(nameStart, "toon_parse", 10) == 0) ||
        (nameLen == 15 && strncmp(nameStart, "toon_parse_file", 15) == 0)) {
        return "ToonDoc";
    }
    if ((nameLen == 9 && strncmp(nameStart, "toon_root", 9) == 0) ||
        (nameLen == 8 && strncmp(nameStart, "toon_key", 8) == 0) ||
        (nameLen == 7 && strncmp(nameStart, "toon_at", 7) == 0)) {
        return "ToonNode";
    }
    if ((nameLen == 9 && strncmp(nameStart, "toon_type", 9) == 0) ||
        (nameLen == 13 && strncmp(nameStart, "toon_get_text", 13) == 0) ||
        (nameLen == 16 && strncmp(nameStart, "toon_get_text_or", 16) == 0) ||
        (nameLen == 15 && strncmp(nameStart, "toon_text_value", 15) == 0) ||
        (nameLen == 7 && strncmp(nameStart, "ai_chat", 7) == 0)) {
        return "Text";
    }
    if ((nameLen == 8 && strncmp(nameStart, "toon_len", 8) == 0) ||
        (nameLen == 12 && strncmp(nameStart, "toon_get_int", 12) == 0) ||
        (nameLen == 15 && strncmp(nameStart, "toon_get_int_or", 15) == 0) ||
        (nameLen == 14 && strncmp(nameStart, "toon_int_value", 14) == 0)) {
        return "Int";
    }
    if ((nameLen == 13 && strncmp(nameStart, "toon_get_real", 13) == 0) ||
        (nameLen == 16 && strncmp(nameStart, "toon_get_real_or", 16) == 0) ||
        (nameLen == 15 && strncmp(nameStart, "toon_real_value", 15) == 0)) {
        return "Real";
    }
    if ((nameLen == 13 && strncmp(nameStart, "toon_get_bool", 13) == 0) ||
        (nameLen == 16 && strncmp(nameStart, "toon_get_bool_or", 16) == 0) ||
        (nameLen == 15 && strncmp(nameStart, "toon_bool_value", 15) == 0) ||
        (nameLen == 15 && strncmp(nameStart, "toon_null_value", 15) == 0) ||
        (nameLen == 12 && strncmp(nameStart, "toon_is_text", 12) == 0) ||
        (nameLen == 11 && strncmp(nameStart, "toon_is_int", 11) == 0) ||
        (nameLen == 12 && strncmp(nameStart, "toon_is_real", 12) == 0) ||
        (nameLen == 12 && strncmp(nameStart, "toon_is_bool", 12) == 0) ||
        (nameLen == 12 && strncmp(nameStart, "toon_is_null", 12) == 0) ||
        (nameLen == 11 && strncmp(nameStart, "toon_is_arr", 11) == 0) ||
        (nameLen == 11 && strncmp(nameStart, "toon_is_obj", 11) == 0) ||
        (nameLen == 12 && strncmp(nameStart, "toon_has_key", 12) == 0) ||
        (nameLen == 11 && strncmp(nameStart, "toon_has_at", 11) == 0) ||
        (nameLen == 8 && strncmp(nameStart, "has_toon", 8) == 0) ||
        (nameLen == 6 && strncmp(nameStart, "has_ai", 6) == 0) ||
        (nameLen == 11 && strncmp(nameStart, "has_builtin", 11) == 0)) {
        return "Bool";
    }
    if ((nameLen == 8 && strncmp(nameStart, "has_toon", 8) == 0) ||
        (nameLen == 6 && strncmp(nameStart, "has_ai", 6) == 0) ||
        (nameLen == 11 && strncmp(nameStart, "has_builtin", 11) == 0)) {
        return "Bool";
    }
    return NULL;
}

static const char *inferObjectInitTypeName(const char *start, const char *end) {
    const char *nameStart;
    const char *nameEnd;
    const char *cursor;

    if (!start || !end || end <= start) {
        return NULL;
    }
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    if (start >= end || !(isalpha((unsigned char)*start) || *start == '_')) {
        return NULL;
    }
    nameStart = start;
    nameEnd = start + 1;
    while (nameEnd < end && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
        nameEnd++;
    }
    cursor = skipSpacesInRange(nameEnd, end);
    if (cursor < end && *cursor == '{' && end[-1] == '}') {
        return trimmedCopy(nameStart, nameEnd);
    }
    return NULL;
}

static char *inferAetherBindingTypeName(const char *exprStart,
                                        const char *exprEnd,
                                        const AetherBindingTable *bindings,
                                        const AetherFunctionTable *functions) {
    const char *trimmedStart = exprStart;
    const char *trimmedEnd = exprEnd;
    const char *nameEnd;
    const char *helperType;
    char *objectInitType;

    if (!exprStart || !exprEnd || exprEnd < exprStart) {
        return NULL;
    }
    while (trimmedStart < trimmedEnd && isspace((unsigned char)*trimmedStart)) {
        trimmedStart++;
    }
    while (trimmedEnd > trimmedStart && isspace((unsigned char)trimmedEnd[-1])) {
        trimmedEnd--;
    }
    if (trimmedEnd > trimmedStart && trimmedEnd[-1] == ';') {
        trimmedEnd--;
    }
    while (trimmedEnd > trimmedStart && isspace((unsigned char)trimmedEnd[-1])) {
        trimmedEnd--;
    }
    if (trimmedStart >= trimmedEnd) {
        return NULL;
    }
    if (*trimmedStart == '"' && trimmedEnd[-1] == '"') {
        return dupCString("Text");
    }
    if (isAetherBoolLiteral(trimmedStart, trimmedEnd)) {
        return dupCString("Bool");
    }
    if (inferNumericLiteralType(trimmedStart, trimmedEnd, &helperType)) {
        return dupCString(helperType);
    }
    nameEnd = trimmedStart;
    if (isalpha((unsigned char)*nameEnd) || *nameEnd == '_') {
        while (nameEnd < trimmedEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
            nameEnd++;
        }
        if (nameEnd == trimmedEnd && bindings) {
            const char *bindingType = findAetherBindingType(bindings,
                                                            trimmedStart,
                                                            (size_t)(trimmedEnd - trimmedStart));
            if (bindingType) {
                return dupCString(bindingType);
            }
        }
        if (nameEnd < trimmedEnd) {
            const char *callNameEnd = nameEnd;

            while (callNameEnd < trimmedEnd) {
                const char *dot = skipSpacesInRange(callNameEnd, trimmedEnd);

                if (dot >= trimmedEnd || *dot != '.') {
                    break;
                }
                callNameEnd = skipSpacesInRange(dot + 1, trimmedEnd);
                if (callNameEnd >= trimmedEnd ||
                    !(isalpha((unsigned char)*callNameEnd) || *callNameEnd == '_')) {
                    callNameEnd = nameEnd;
                    break;
                }
                while (callNameEnd < trimmedEnd &&
                       (isalnum((unsigned char)*callNameEnd) || *callNameEnd == '_')) {
                    callNameEnd++;
                }
            }
            if (functions && *skipSpacesInRange(callNameEnd, trimmedEnd) == '(') {
                const char *functionType = findAetherFunctionReturnType(functions,
                                                                        trimmedStart,
                                                                        (size_t)(callNameEnd - trimmedStart));
                if (functionType) {
                    return dupCString(functionType);
                }
            }
        }
        if (nameEnd < trimmedEnd && *skipSpacesInRange(nameEnd, trimmedEnd) == '(') {
            helperType = inferHelperReturnTypeName(trimmedStart, (size_t)(nameEnd - trimmedStart));
            if (helperType) {
                return dupCString(helperType);
            }
        }
    }
    objectInitType = (char *)inferObjectInitTypeName(trimmedStart, trimmedEnd);
    if (objectInitType) {
        return objectInitType;
    }
    return NULL;
}

static void maybeRecordAetherBindingType(AetherBindingTable *table,
                                         const char *body,
                                         const char *lineEnd,
                                         const AetherFunctionTable *functions) {
    const char *cursor;
    const char *nameStart;
    const char *nameEnd;
    const char *colon;
    const char *equals;
    char *name = NULL;
    char *typeName = NULL;

    if (!table || !body || !lineEnd) {
        return;
    }
    if (!(startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const"))) {
        return;
    }
    cursor = body + (startsWithWord(body, lineEnd, "const") ? 5 : 3);
    cursor = skipSpacesInRange(cursor, lineEnd);
    nameStart = cursor;
    while (cursor < lineEnd && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    nameEnd = cursor;
    if (nameEnd == nameStart) {
        return;
    }
    name = trimmedCopy(nameStart, nameEnd);
    if (!name) {
        return;
    }

    colon = skipSpacesInRange(cursor, lineEnd);
    if (colon < lineEnd && *colon == ':') {
        const char *typeStart = skipSpacesInRange(colon + 1, lineEnd);
        const char *typeEnd = typeStart;

        while (typeEnd < lineEnd && *typeEnd != '=' && *typeEnd != ';') {
            typeEnd++;
        }
        while (typeEnd > typeStart && isspace((unsigned char)typeEnd[-1])) {
            typeEnd--;
        }
        typeName = trimmedCopy(typeStart, typeEnd);
    } else {
        equals = colon;
        while (equals < lineEnd && *equals != '=') {
            equals++;
        }
        if (equals < lineEnd && *equals == '=') {
            typeName = inferAetherBindingTypeName(skipSpacesInRange(equals + 1, lineEnd),
                                                  lineEnd,
                                                  table,
                                                  functions);
        }
    }

    if (typeName) {
        setAetherBindingType(table, name, typeName);
    }
    free(name);
    free(typeName);
}

static char *extractBindingName(const char *body, const char *lineEnd) {
    const char *cursor;
    const char *nameStart;
    const char *nameEnd;

    if (!body || !lineEnd) {
        return NULL;
    }
    if (!(startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const"))) {
        return NULL;
    }
    cursor = body + (startsWithWord(body, lineEnd, "const") ? 5 : 3);
    cursor = skipSpacesInRange(cursor, lineEnd);
    nameStart = cursor;
    while (cursor < lineEnd && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    nameEnd = cursor;
    if (nameEnd == nameStart) {
        return NULL;
    }
    return trimmedCopy(nameStart, nameEnd);
}

static char *extractModuleName(const char *source) {
    const char *cursor = source;

    if (!source) {
        return NULL;
    }
    while (*cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *body;
        const char *nameStart;
        const char *nameEnd;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }
        body = skipSpacesInRange(lineStart, lineEnd);
        if (startsWithWord(body, lineEnd, "mod")) {
            nameStart = skipSpacesInRange(body + 3, lineEnd);
            nameEnd = nameStart;
            while (nameEnd < lineEnd &&
                   (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
                nameEnd++;
            }
            if (nameEnd > nameStart) {
                return trimmedCopy(nameStart, nameEnd);
            }
        }
        cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
    }
    return NULL;
}

static void maybeRecordAetherFunctionReturnType(AetherFunctionTable *table,
                                                const char *body,
                                                const char *lineEnd,
                                                const char *moduleName) {
    const char *cursor;
    const char *nameStart;
    const char *nameEnd;
    const char *paramsOpen;
    const char *paramsClose;
    const char *arrow;
    const char *typeStart;
    const char *typeEnd;
    char qualifiedName[512];
    char *fnName = NULL;
    char *returnType = NULL;

    if (!table || !body || !lineEnd) {
        return;
    }
    if (startsWithWord(body, lineEnd, "export")) {
        body = skipSpacesInRange(body + 6, lineEnd);
    }
    if (!startsWithWord(body, lineEnd, "fn")) {
        return;
    }
    cursor = skipSpacesInRange(body + 2, lineEnd);
    nameStart = cursor;
    while (cursor < lineEnd && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    nameEnd = cursor;
    paramsOpen = findCharInRange(nameEnd, lineEnd, '(');
    paramsClose = paramsOpen ? findLastCharInRange(paramsOpen, lineEnd, ')') : NULL;
    arrow = paramsClose ? findSubstringInRange(paramsClose, lineEnd, "->") : NULL;
    if (!paramsOpen || !paramsClose || !arrow || nameEnd == nameStart) {
        return;
    }
    typeStart = skipSpacesInRange(arrow + 2, lineEnd);
    typeEnd = typeStart;
    while (typeEnd < lineEnd && *typeEnd != '{') {
        typeEnd++;
    }
    while (typeEnd > typeStart && isspace((unsigned char)typeEnd[-1])) {
        typeEnd--;
    }
    fnName = trimmedCopy(nameStart, nameEnd);
    returnType = trimmedCopy(typeStart, typeEnd);
    if (!fnName || !returnType) {
        free(fnName);
        free(returnType);
        return;
    }
    setAetherFunctionReturnType(table, fnName, returnType);
    if (moduleName && *moduleName &&
        snprintf(qualifiedName, sizeof(qualifiedName), "%s.%s", moduleName, fnName) < (int)sizeof(qualifiedName)) {
        setAetherFunctionReturnType(table, qualifiedName, returnType);
    }
    free(fnName);
    free(returnType);
}

static char *extractUsePathLiteral(const char *body, const char *lineEnd) {
    const char *cursor;
    const char *pathStart;
    const char *pathEnd;

    if (!body || !lineEnd || !startsWithWord(body, lineEnd, "use")) {
        return NULL;
    }
    cursor = skipSpacesInRange(body + 3, lineEnd);
    if (cursor >= lineEnd || *cursor != '"') {
        return NULL;
    }
    pathStart = cursor + 1;
    pathEnd = pathStart;
    while (pathEnd < lineEnd) {
        if (*pathEnd == '\\' && pathEnd + 1 < lineEnd) {
            pathEnd += 2;
            continue;
        }
        if (*pathEnd == '"') {
            break;
        }
        pathEnd++;
    }
    if (pathEnd >= lineEnd || *pathEnd != '"') {
        return NULL;
    }
    return dupRange(pathStart, pathEnd);
}

static int copyNamedBindingType(AetherBindingTable *dst,
                                const AetherBindingTable *src,
                                const char *name) {
    const char *typeName;

    if (!dst || !src || !name) {
        return 0;
    }
    typeName = findAetherBindingType(src, name, strlen(name));
    if (!typeName) {
        return 1;
    }
    return setAetherBindingType(dst, name, typeName);
}

static int collectImportedAetherBindings(AetherBindingTable *out,
                                         AetherFunctionTable *functions,
                                         const char *source,
                                         const char *modulePath) {
    AetherBindingTable local = {0};
    char *moduleName = NULL;
    const char *cursor = source;

    (void)modulePath;
    if (!out || !source) {
        return 0;
    }
    moduleName = extractModuleName(source);

    while (*cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *body;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }
        body = skipSpacesInRange(lineStart, lineEnd);

        if (startsWithWord(body, lineEnd, "export")) {
            const char *rest = skipSpacesInRange(body + 6, lineEnd);

            maybeRecordAetherFunctionReturnType(functions, rest, lineEnd, moduleName);
            maybeRecordAetherBindingType(&local, rest, lineEnd, functions);
            if (startsWithWord(rest, lineEnd, "let") || startsWithWord(rest, lineEnd, "const")) {
                char *name = extractBindingName(rest, lineEnd);
                if (name) {
                    if (!copyNamedBindingType(out, &local, name)) {
                        free(name);
                        free(moduleName);
                        freeAetherBindingTable(&local);
                        return 0;
                    }
                    free(name);
                }
            }
        } else {
            maybeRecordAetherFunctionReturnType(functions, body, lineEnd, moduleName);
            maybeRecordAetherBindingType(&local, body, lineEnd, functions);
        }

        cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
    }

    free(moduleName);
    freeAetherBindingTable(&local);
    return 1;
}

static int maybeLoadImportedBindings(AetherBindingTable *table,
                                     AetherFunctionTable *functions,
                                     const char *body,
                                     const char *lineEnd,
                                     const char *sourcePath) {
    char *importPath = NULL;
    char *resolvedPath = NULL;
    char *moduleSource = NULL;
    int ok = 1;

    if (!table || !body || !lineEnd || !startsWithWord(body, lineEnd, "use")) {
        return 1;
    }
    importPath = extractUsePathLiteral(body, lineEnd);
    if (!importPath) {
        return 1;
    }
    resolvedPath = resolveRelativePath(sourcePath, importPath);
    if (!resolvedPath) {
        free(importPath);
        return 1;
    }
    moduleSource = readTextFile(resolvedPath);
    if (moduleSource) {
        ok = collectImportedAetherBindings(table, functions, moduleSource, resolvedPath);
    }
    free(moduleSource);
    free(resolvedPath);
    free(importPath);
    return ok;
}

static void maybeRecordToonLiteralBinding(ToonLiteralTable *table, const char *body, const char *lineEnd) {
    const char *cursor;
    const char *nameStart;
    const char *nameEnd;
    const char *equals;
    const char *valueStart;
    const char *valueEnd;
    const char *aliasLiteral = NULL;
    char *name = NULL;
    char *literal = NULL;

    if (!table || !body || !lineEnd) {
        return;
    }
    if (!(startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const"))) {
        return;
    }
    cursor = body + (startsWithWord(body, lineEnd, "const") ? 5 : 3);
    cursor = skipSpacesInRange(cursor, lineEnd);
    nameStart = cursor;
    while (cursor < lineEnd && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    nameEnd = cursor;
    cursor = skipSpacesInRange(cursor, lineEnd);
    if (cursor >= lineEnd || *cursor != ':') {
        return;
    }
    equals = cursor + 1;
    while (equals < lineEnd && *equals != '=') {
        equals++;
    }
    if (equals >= lineEnd || *equals != '=') {
        return;
    }
    if (!isSupportedToonBindingType(cursor + 1, equals)) {
        return;
    }
    valueStart = skipSpacesInRange(equals + 1, lineEnd);
    valueEnd = lineEnd;
    while (valueEnd > valueStart && isspace((unsigned char)valueEnd[-1])) {
        valueEnd--;
    }
    if (valueEnd > valueStart && valueEnd[-1] == ';') {
        valueEnd--;
    }
    while (valueEnd > valueStart && isspace((unsigned char)valueEnd[-1])) {
        valueEnd--;
    }
    name = trimmedCopy(nameStart, nameEnd);
    if (!name) {
        return;
    }
    if (valueEnd > valueStart && *valueStart == '"' && valueEnd[-1] == '"') {
        literal = dupRange(valueStart, valueEnd);
        if (!literal || !setToonLiteralBinding(table, name, literal)) {
            free(name);
            free(literal);
            return;
        }
        free(name);
        free(literal);
        return;
    }
    if (valueEnd > valueStart &&
        (isalpha((unsigned char)*valueStart) || *valueStart == '_')) {
        const char *aliasEnd = valueStart;

        while (aliasEnd < valueEnd && (isalnum((unsigned char)*aliasEnd) || *aliasEnd == '_')) {
            aliasEnd++;
        }
        if (aliasEnd == valueEnd) {
            aliasLiteral = findToonLiteralBinding(table,
                                                 valueStart,
                                                 (size_t)(valueEnd - valueStart));
        }
    }
    if (aliasLiteral && setToonLiteralBinding(table, name, aliasLiteral)) {
        free(name);
        return;
    }
    clearToonLiteralBinding(table, name, strlen(name));
    free(name);
}

static int appendJsonAliasReplacement(Buffer *out,
                                      const char *nameStart,
                                      size_t nameLen,
                                      JsonAliasState *jsonState) {
    if (!out || !nameStart || nameLen == 0) {
        return 0;
    }

    if (nameLen == 15 && strncmp(nameStart, "toon_parse_file", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonReadFile");
    }
    if (nameLen == 10 && strncmp(nameStart, "toon_parse", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonRead");
    }
    if (nameLen == 9 && strncmp(nameStart, "toon_root", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonGetRoot");
    }
    if (nameLen == 10 && strncmp(nameStart, "toon_close", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonDocFree");
    }
    if (nameLen == 8 && strncmp(nameStart, "toon_key", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonGetKey");
    }
    if (nameLen == 12 && strncmp(nameStart, "toon_has_key", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonHasKey");
    }
    if (nameLen == 7 && strncmp(nameStart, "toon_at", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonGetIndex");
    }
    if (nameLen == 11 && strncmp(nameStart, "toon_has_at", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonHasIndex");
    }
    if (nameLen == 8 && strncmp(nameStart, "toon_len", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonGetLength");
    }
    if (nameLen == 9 && strncmp(nameStart, "toon_free", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonFreeValue");
    }
    if (nameLen == 15 && strncmp(nameStart, "toon_text_value", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonGetString");
    }
    if (nameLen == 14 && strncmp(nameStart, "toon_int_value", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonGetInt");
    }
    if (nameLen == 15 && strncmp(nameStart, "toon_real_value", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonGetNumber");
    }
    if (nameLen == 15 && strncmp(nameStart, "toon_bool_value", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonGetBool");
    }
    if (nameLen == 15 && strncmp(nameStart, "toon_null_value", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonIsNull");
    }
    return 0;
}

static int appendAetherBuiltinAlias(Buffer *out, const char *nameStart, size_t nameLen) {
    if (!out || !nameStart || nameLen == 0) {
        return 0;
    }
    if (nameLen == 10 && strncmp(nameStart, "task_spawn", nameLen) == 0) {
        return bufferAppend(out, "thread_spawn_named");
    }
    if (nameLen == 10 && strncmp(nameStart, "task_queue", nameLen) == 0) {
        return bufferAppend(out, "thread_pool_submit");
    }
    if (nameLen == 13 && strncmp(nameStart, "task_set_name", nameLen) == 0) {
        return bufferAppend(out, "thread_set_name");
    }
    if (nameLen == 10 && strncmp(nameStart, "task_pause", nameLen) == 0) {
        return bufferAppend(out, "thread_pause");
    }
    if (nameLen == 11 && strncmp(nameStart, "task_resume", nameLen) == 0) {
        return bufferAppend(out, "thread_resume");
    }
    if (nameLen == 11 && strncmp(nameStart, "task_cancel", nameLen) == 0) {
        return bufferAppend(out, "thread_cancel");
    }
    if (nameLen == 11 && strncmp(nameStart, "task_lookup", nameLen) == 0) {
        return bufferAppend(out, "thread_lookup");
    }
    if (nameLen == 9 && strncmp(nameStart, "task_wait", nameLen) == 0) {
        return bufferAppend(out, "WaitForThread");
    }
    if (nameLen == 11 && strncmp(nameStart, "task_status", nameLen) == 0) {
        return bufferAppend(out, "thread_get_status");
    }
    if (nameLen == 11 && strncmp(nameStart, "task_result", nameLen) == 0) {
        return bufferAppend(out, "thread_get_result");
    }
    if (nameLen == 10 && strncmp(nameStart, "task_stats", nameLen) == 0) {
        return bufferAppend(out, "thread_stats");
    }
    if (nameLen == 15 && strncmp(nameStart, "task_stats_json", nameLen) == 0) {
        return bufferAppend(out, "ThreadStatsJson");
    }
    if (nameLen == 7 && strncmp(nameStart, "ai_chat", nameLen) == 0) {
        return bufferAppend(out, "openaichatcompletions");
    }
    if (nameLen == 7 && strncmp(nameStart, "println", nameLen) == 0) {
        return bufferAppend(out, "writeln");
    }
    if (nameLen == 5 && strncmp(nameStart, "print", nameLen) == 0) {
        return bufferAppend(out, "write");
    }
    return 0;
}

static int appendAetherCapabilityAlias(Buffer *out,
                                       const char *nameStart,
                                       size_t nameLen,
                                       const char *openParen,
                                       const char **outCursor) {
    const char *closeParen;

    if (!out || !nameStart || !openParen || *openParen != '(' || !outCursor) {
        return 0;
    }
    closeParen = skipSpaces(openParen + 1);
    if (nameLen == 8 && strncmp(nameStart, "has_toon", nameLen) == 0) {
        if (*closeParen != ')') {
            return 0;
        }
        if (!bufferAppend(out, "hasextbuiltin(\"yyjson\", \"YyjsonRead\")")) {
            return 0;
        }
        *outCursor = closeParen + 1;
        return 1;
    }
    if (nameLen == 6 && strncmp(nameStart, "has_ai", nameLen) == 0) {
        if (*closeParen != ')') {
            return 0;
        }
        if (!bufferAppend(out, "hasextbuiltin(\"openai\", \"OpenAIChatCompletions\")")) {
            return 0;
        }
        *outCursor = closeParen + 1;
        return 1;
    }
    if (nameLen == 11 && strncmp(nameStart, "has_builtin", nameLen) == 0) {
        if (!bufferAppend(out, "hasextbuiltin")) {
            return 0;
        }
        *outCursor = openParen;
        return 1;
    }
    return 0;
}

static const char *toonScalarGetterForName(const char *nameStart, size_t nameLen) {
    if (nameLen == 13 && strncmp(nameStart, "toon_get_text", nameLen) == 0) {
        return "YyjsonGetString";
    }
    if (nameLen == 16 && strncmp(nameStart, "toon_get_text_or", nameLen) == 0) {
        return "YyjsonGetString";
    }
    if (nameLen == 12 && strncmp(nameStart, "toon_get_int", nameLen) == 0) {
        return "YyjsonGetInt";
    }
    if (nameLen == 15 && strncmp(nameStart, "toon_get_int_or", nameLen) == 0) {
        return "YyjsonGetInt";
    }
    if (nameLen == 13 && strncmp(nameStart, "toon_get_real", nameLen) == 0) {
        return "YyjsonGetNumber";
    }
    if (nameLen == 16 && strncmp(nameStart, "toon_get_real_or", nameLen) == 0) {
        return "YyjsonGetNumber";
    }
    if (nameLen == 13 && strncmp(nameStart, "toon_get_bool", nameLen) == 0) {
        return "YyjsonGetBool";
    }
    if (nameLen == 16 && strncmp(nameStart, "toon_get_bool_or", nameLen) == 0) {
        return "YyjsonGetBool";
    }
    return NULL;
}

static int isToonScalarDefaultHelper(const char *nameStart, size_t nameLen) {
    return (nameLen == 16 && strncmp(nameStart, "toon_get_text_or", nameLen) == 0) ||
           (nameLen == 15 && strncmp(nameStart, "toon_get_int_or", nameLen) == 0) ||
           (nameLen == 16 && strncmp(nameStart, "toon_get_real_or", nameLen) == 0) ||
           (nameLen == 16 && strncmp(nameStart, "toon_get_bool_or", nameLen) == 0);
}

static const char *toonTypePredicateExpected(const char *nameStart, size_t nameLen) {
    if (nameLen == 12 && strncmp(nameStart, "toon_is_text", nameLen) == 0) {
        return "string";
    }
    if (nameLen == 11 && strncmp(nameStart, "toon_is_int", nameLen) == 0) {
        return "int";
    }
    if (nameLen == 12 && strncmp(nameStart, "toon_is_real", nameLen) == 0) {
        return "real";
    }
    if (nameLen == 12 && strncmp(nameStart, "toon_is_bool", nameLen) == 0) {
        return "bool";
    }
    if (nameLen == 12 && strncmp(nameStart, "toon_is_null", nameLen) == 0) {
        return "null";
    }
    if (nameLen == 11 && strncmp(nameStart, "toon_is_arr", nameLen) == 0) {
        return "array";
    }
    if (nameLen == 11 && strncmp(nameStart, "toon_is_obj", nameLen) == 0) {
        return "object";
    }
    return NULL;
}

static int appendTrimmedRange(Buffer *out, const char *start, const char *end) {
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    return bufferAppendN(out, start, (size_t)(end - start));
}

static int appendToonScalarAlias(Buffer *out,
                                 const char *nameStart,
                                 size_t nameLen,
                                 const char *openParen,
                                 const char **outCursor,
                                 JsonAliasState *jsonState) {
    const char *getter = toonScalarGetterForName(nameStart, nameLen);
    const char *cursor;
    const char *arg1Start;
    const char *arg1End = NULL;
    const char *arg2Start = NULL;
    const char *arg2End = NULL;
    const char *arg3Start = NULL;
    const char *arg3End = NULL;
    int depth = 0;
    int inString = 0;
    char quote = '\0';
    int defaultHelper = isToonScalarDefaultHelper(nameStart, nameLen);

    if (!getter || !openParen || *openParen != '(' || !outCursor) {
        return 0;
    }
    cursor = openParen + 1;
    arg1Start = cursor;
    while (*cursor) {
        if (inString) {
            if (*cursor == '\\' && cursor[1] != '\0') {
                cursor += 2;
                continue;
            }
            if (*cursor == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (*cursor == '"' || *cursor == '\'') {
            inString = 1;
            quote = *cursor;
            cursor++;
            continue;
        }
        if (*cursor == '(' || *cursor == '[' || *cursor == '{') {
            depth++;
        } else if (*cursor == ')' || *cursor == ']' || *cursor == '}') {
            if (depth == 0) {
                if (!arg1End || !arg2Start) {
                    return 0;
                }
                if (arg3Start) {
                    arg3End = cursor;
                } else {
                    arg2End = cursor;
                }
                break;
            }
            depth--;
        } else if (*cursor == ',' && depth == 0 && !arg1End) {
            arg1End = cursor;
            arg2Start = cursor + 1;
        } else if (*cursor == ',' && depth == 0 && !arg2End) {
            arg2End = cursor;
            arg3Start = cursor + 1;
        }
        cursor++;
    }
    if (!arg1End || !arg2Start || !arg2End) {
        return 0;
    }
    if (defaultHelper && (!arg3Start || !arg3End)) {
        return 0;
    }
    if (!defaultHelper && arg3Start) {
        return 0;
    }
    jsonState->needed = 1;
    if (defaultHelper) {
        if (!bufferAppend(out, "(YyjsonHasKey(") ||
            !appendTrimmedRange(out, arg1Start, arg1End) ||
            !bufferAppend(out, ", ") ||
            !appendTrimmedRange(out, arg2Start, arg2End) ||
            !bufferAppend(out, ") ? ") ||
            !bufferAppend(out, getter) ||
            !bufferAppend(out, "(YyjsonGetKey(") ||
            !appendTrimmedRange(out, arg1Start, arg1End) ||
            !bufferAppend(out, ", ") ||
            !appendTrimmedRange(out, arg2Start, arg2End) ||
            !bufferAppend(out, ")) : ") ||
            !appendTrimmedRange(out, arg3Start, arg3End) ||
            !bufferAppend(out, ")")) {
            return 0;
        }
    } else {
        if (!bufferAppend(out, getter) ||
            !bufferAppend(out, "(YyjsonGetKey(") ||
            !appendTrimmedRange(out, arg1Start, arg1End) ||
            !bufferAppend(out, ", ") ||
            !appendTrimmedRange(out, arg2Start, arg2End) ||
            !bufferAppend(out, "))")) {
            return 0;
        }
    }
    *outCursor = cursor + 1;
    return 1;
}

static int appendToonInspectAlias(Buffer *out,
                                  const char *nameStart,
                                  size_t nameLen,
                                  const char *openParen,
                                  const char **outCursor,
                                  JsonAliasState *jsonState) {
    const char *expectedType = toonTypePredicateExpected(nameStart, nameLen);
    const char *cursor;
    const char *argStart;
    const char *argEnd;
    int depth = 0;
    int inString = 0;
    char quote = '\0';

    if (!out || !nameStart || !openParen || *openParen != '(' || !outCursor) {
        return 0;
    }

    if (nameLen == 9 && strncmp(nameStart, "toon_type", nameLen) == 0) {
        cursor = openParen + 1;
        argStart = cursor;
        while (*cursor) {
            if (inString) {
                if (*cursor == '\\' && cursor[1] != '\0') {
                    cursor += 2;
                    continue;
                }
                if (*cursor == quote) {
                    inString = 0;
                    quote = '\0';
                }
                cursor++;
                continue;
            }
            if (*cursor == '"' || *cursor == '\'') {
                inString = 1;
                quote = *cursor;
                cursor++;
                continue;
            }
            if (*cursor == '(' || *cursor == '[' || *cursor == '{') {
                depth++;
            } else if (*cursor == ')' || *cursor == ']' || *cursor == '}') {
                if (depth == 0) {
                    argEnd = cursor;
                    jsonState->needed = 1;
                    if (!bufferAppend(out, "YyjsonGetType(") ||
                        !appendTrimmedRange(out, argStart, argEnd) ||
                        !bufferAppend(out, ")")) {
                        return 0;
                    }
                    *outCursor = cursor + 1;
                    return 1;
                }
                depth--;
            }
            cursor++;
        }
        return 0;
    }

    if (!expectedType) {
        return 0;
    }

    cursor = openParen + 1;
    argStart = cursor;
    while (*cursor) {
        if (inString) {
            if (*cursor == '\\' && cursor[1] != '\0') {
                cursor += 2;
                continue;
            }
            if (*cursor == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (*cursor == '"' || *cursor == '\'') {
            inString = 1;
            quote = *cursor;
            cursor++;
            continue;
        }
        if (*cursor == '(' || *cursor == '[' || *cursor == '{') {
            depth++;
        } else if (*cursor == ')' || *cursor == ']' || *cursor == '}') {
            if (depth == 0) {
                argEnd = cursor;
                jsonState->needed = 1;
                if (!bufferAppend(out, "(YyjsonGetType(") ||
                    !appendTrimmedRange(out, argStart, argEnd) ||
                    !bufferAppend(out, ") == \"") ||
                    !bufferAppend(out, expectedType) ||
                    !bufferAppend(out, "\")")) {
                    return 0;
                }
                *outCursor = cursor + 1;
                return 1;
            }
            depth--;
        }
        cursor++;
    }
    return 0;
}

static char *applyJsonAliasesToLine(const char *line,
                                    JsonAliasState *jsonState,
                                    const ToonLiteralTable *toonTable) {
    const char *cursor = line;
    Buffer out = {0};

    if (!line || !jsonState) {
        return line ? dupRange(line, line + strlen(line)) : NULL;
    }
    if (*line == '\0') {
        return dupRange(line, line);
    }

    while (*cursor) {
        if ((cursor == line || !(isalnum((unsigned char)cursor[-1]) || cursor[-1] == '_')) &&
            ((strncmp(cursor, "toon_parse", 10) == 0 &&
              !(isalnum((unsigned char)cursor[10]) || cursor[10] == '_')) ||
             (strncmp(cursor, "YyjsonRead", 10) == 0 &&
              !(isalnum((unsigned char)cursor[10]) || cursor[10] == '_')))) {
            const char *nameEnd = cursor + 10;
            const char *openParen = skipSpaces(nameEnd);

            if (*openParen == '(') {
                const char *argStart = skipSpaces(openParen + 1);
                const char *argEnd = argStart;
                const char *literal = NULL;
                const char *closeParen;

                while (*argEnd && (isalnum((unsigned char)*argEnd) || *argEnd == '_')) {
                    argEnd++;
                }
                literal = findToonLiteralBinding(toonTable, argStart, (size_t)(argEnd - argStart));
                closeParen = skipSpaces(argEnd);
                if (literal && *closeParen == ')') {
                    jsonState->needed = 1;
                    if (!bufferAppend(&out, "YyjsonRead(") ||
                        !bufferAppend(&out, literal) ||
                        !bufferAppend(&out, ")")) {
                        free(out.data);
                        return NULL;
                    }
                    cursor = closeParen + 1;
                    continue;
                }
            }
        }
        if ((cursor == line || !(isalnum((unsigned char)cursor[-1]) || cursor[-1] == '_')) &&
            isalpha((unsigned char)*cursor)) {
            const char *nameStart = cursor;
            const char *nameEnd = cursor + 1;
            const char *afterName;
            const char *advancedCursor = NULL;

            while (*nameEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
                nameEnd++;
            }
            afterName = skipSpaces(nameEnd);
            if (*afterName == '(' &&
                appendAetherCapabilityAlias(&out,
                                            nameStart,
                                            (size_t)(nameEnd - nameStart),
                                            afterName,
                                            &advancedCursor)) {
                cursor = advancedCursor;
                continue;
            }
            if (*afterName == '(' &&
                appendToonScalarAlias(&out,
                                      nameStart,
                                      (size_t)(nameEnd - nameStart),
                                      afterName,
                                      &advancedCursor,
                                      jsonState)) {
                cursor = advancedCursor;
                continue;
            }
            if (*afterName == '(' &&
                appendToonInspectAlias(&out,
                                       nameStart,
                                       (size_t)(nameEnd - nameStart),
                                       afterName,
                                       &advancedCursor,
                                       jsonState)) {
                cursor = advancedCursor;
                continue;
            }
            if (*afterName == '(' &&
                appendAetherBuiltinAlias(&out, nameStart, (size_t)(nameEnd - nameStart))) {
                cursor = nameEnd;
                continue;
            }
            if (*afterName == '(' &&
                appendJsonAliasReplacement(&out, nameStart, (size_t)(nameEnd - nameStart), jsonState)) {
                cursor = nameEnd;
                continue;
            }
        }
        if ((cursor == line || !(isalnum((unsigned char)cursor[-1]) || cursor[-1] == '_')) &&
            strncmp(cursor, "YyjsonRead(", 11) == 0) {
            const char *argStart = skipSpaces(cursor + 11);
            const char *argEnd = argStart;
            const char *literal = NULL;

            while (*argEnd && (isalnum((unsigned char)*argEnd) || *argEnd == '_')) {
                argEnd++;
            }
            literal = findToonLiteralBinding(toonTable, argStart, (size_t)(argEnd - argStart));
            if (literal) {
                const char *afterArg = skipSpaces(argEnd);
                if (*afterArg == ')') {
                    if (!bufferAppend(&out, "YyjsonRead(") ||
                        !bufferAppend(&out, literal) ||
                        !bufferAppend(&out, ")")) {
                        free(out.data);
                        return NULL;
                    }
                    cursor = afterArg + 1;
                    continue;
                }
            }
        }
        if (!bufferAppendN(&out, cursor, 1)) {
            free(out.data);
            return NULL;
        }
        cursor++;
    }

    return out.data;
}

static int isLineComment(const char *body, const char *lineEnd) {
    return body < lineEnd && (size_t)(lineEnd - body) >= 2 && strncmp(body, "//", 2) == 0;
}

static int startsWithWord(const char *body, const char *lineEnd, const char *word) {
    size_t wordLen = word ? strlen(word) : 0;
    if (!body || !lineEnd || !word || wordLen == 0) {
        return 0;
    }
    if ((size_t)(lineEnd - body) < wordLen) {
        return 0;
    }
    if (strncmp(body, word, wordLen) != 0) {
        return 0;
    }
    if ((size_t)(lineEnd - body) == wordLen) {
        return 1;
    }
    return isspace((unsigned char)body[wordLen]) || body[wordLen] == '{' || body[wordLen] == ';';
}

static char *appendContractExpr(char *existing, const char *expr) {
    Buffer out = {0};

    if (!expr || !*expr) {
        return existing;
    }
    if (!existing || !*existing) {
        free(existing);
        return dupRange(expr, expr + strlen(expr));
    }
    if (!bufferAppend(&out, "(") ||
        !bufferAppend(&out, existing) ||
        !bufferAppend(&out, ") && (") ||
        !bufferAppend(&out, expr) ||
        !bufferAppend(&out, ")")) {
        free(existing);
        free(out.data);
        return NULL;
    }
    free(existing);
    return out.data;
}

static char *extractAnnotationExpr(const char *body, const char *lineEnd, const char *directive) {
    const char *exprStart;
    if (!directive) {
        return NULL;
    }
    exprStart = body + strlen(directive);
    while (exprStart < lineEnd && isspace((unsigned char)*exprStart)) {
        exprStart++;
    }
    return trimmedCopy(exprStart, lineEnd);
}

static int extractFunctionSignature(const char *body,
                                    const char *lineEnd,
                                    char **outName,
                                    char **outReturnType) {
    const char *nameStart = body + 2;
    const char *nameEnd;
    const char *paramsOpen;
    const char *paramsClose;
    const char *arrow;
    const char *typeStart;
    const char *typeEnd;

    if (!outName || !outReturnType) {
        return 0;
    }
    *outName = NULL;
    *outReturnType = NULL;
    while (nameStart < lineEnd && isspace((unsigned char)*nameStart)) {
        nameStart++;
    }
    nameEnd = nameStart;
    while (nameEnd < lineEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
        nameEnd++;
    }
    paramsOpen = findCharInRange(nameEnd, lineEnd, '(');
    paramsClose = paramsOpen ? findLastCharInRange(paramsOpen, lineEnd, ')') : NULL;
    arrow = paramsClose ? findSubstringInRange(paramsClose, lineEnd, "->") : NULL;
    if (!paramsOpen || !paramsClose || !arrow) {
        return 0;
    }
    typeStart = arrow + 2;
    while (typeStart < lineEnd && isspace((unsigned char)*typeStart)) {
        typeStart++;
    }
    typeEnd = typeStart;
    while (typeEnd < lineEnd && *typeEnd != '{') {
        typeEnd++;
    }
    *outName = trimmedCopy(nameStart, nameEnd);
    *outReturnType = trimmedCopy(typeStart, typeEnd);
    return *outName && *outReturnType;
}

static int braceDeltaForLine(const char *line) {
    int delta = 0;
    int inString = 0;
    char quote = '\0';
    const char *cursor = line;

    if (!line) {
        return 0;
    }
    while (*cursor) {
        if (!inString && cursor[0] == '/' && cursor[1] == '/') {
            break;
        }
        if (inString) {
            if (*cursor == '\\' && cursor[1] != '\0') {
                cursor += 2;
                continue;
            }
            if (*cursor == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (*cursor == '"' || *cursor == '\'') {
            inString = 1;
            quote = *cursor;
            cursor++;
            continue;
        }
        if (*cursor == '{') {
            delta++;
        } else if (*cursor == '}') {
            delta--;
        }
        cursor++;
    }
    return delta;
}

static char *buildContractIndent(const char *lineStart, const char *body) {
    Buffer out = {0};
    size_t prefixLen = body > lineStart ? (size_t)(body - lineStart) : 0;

    if (!bufferAppendN(&out, lineStart, prefixLen) ||
        !bufferAppend(&out, "    ")) {
        free(out.data);
        return NULL;
    }
    return out.data;
}

static int appendContractGuard(Buffer *out,
                               const char *indent,
                               const char *fnName,
                               const char *kind,
                               const char *expr) {
    if (!out || !indent || !fnName || !kind || !expr) {
        return 0;
    }
    if (!bufferAppend(out, indent) ||
        !bufferAppend(out, "if (!(") ||
        !bufferAppend(out, expr) ||
        !bufferAppend(out, ")) {\n") ||
        !bufferAppend(out, indent) ||
        !bufferAppend(out, "    writeln(\"Aether @") ||
        !bufferAppend(out, kind) ||
        !bufferAppend(out, " failed in ") ||
        !bufferAppend(out, fnName) ||
        !bufferAppend(out, "\");\n") ||
        !bufferAppend(out, indent) ||
        !bufferAppend(out, "    halt(1);\n") ||
        !bufferAppend(out, indent) ||
        !bufferAppend(out, "}\n")) {
        return 0;
    }
    return 1;
}

static int isStandaloneCloseBrace(const char *body, const char *lineEnd) {
    const char *tail = lineEnd;

    while (tail > body && isspace((unsigned char)tail[-1])) {
        tail--;
    }
    return tail == body + 1 && body < tail && *body == '}';
}

static int isParallelCallStatement(const char *body, const char *lineEnd) {
    const char *semi;
    const char *open;
    const char *tail = lineEnd;

    if (!body || body >= lineEnd) {
        return 0;
    }
    if (startsWithWord(body, lineEnd, "if") ||
        startsWithWord(body, lineEnd, "while") ||
        startsWithWord(body, lineEnd, "let") ||
        startsWithWord(body, lineEnd, "const") ||
        startsWithWord(body, lineEnd, "ret") ||
        startsWithWord(body, lineEnd, "fx") ||
        startsWithWord(body, lineEnd, "par")) {
        return 0;
    }
    while (tail > body && isspace((unsigned char)tail[-1])) {
        tail--;
    }
    if (tail <= body || tail[-1] != ';') {
        return 0;
    }
    semi = tail - 1;
    open = findCharInRange(body, semi, '(');
    return open != NULL;
}

static char *translateParallelCallLine(const char *lineStart,
                                       const char *body,
                                       const char *lineEnd,
                                       ParBlockState *parState) {
    Buffer out = {0};
    char handleName[64];

    if (!parState || !parState->indent) {
        return NULL;
    }
    parState->nextHandle++;
    snprintf(handleName, sizeof(handleName), "__aether_par_%d", parState->nextHandle);

    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppend(&out, "int ") ||
        !bufferAppend(&out, handleName) ||
        !bufferAppend(&out, " = spawn ") ||
        !bufferAppendN(&out, body, (size_t)(lineEnd - body))) {
        free(out.data);
        return NULL;
    }

    if (!bufferAppend(&parState->joinLines, parState->indent) ||
        !bufferAppend(&parState->joinLines, "join ") ||
        !bufferAppend(&parState->joinLines, handleName) ||
        !bufferAppend(&parState->joinLines, ";\n")) {
        free(out.data);
        return NULL;
    }

    return out.data;
}

static char *translateReturnWithPost(const char *lineStart,
                                     const char *body,
                                     const char *lineEnd,
                                     const FunctionContracts *fnState) {
    const char *cursor = body + 3;
    const char *exprStart;
    const char *exprEnd;
    Buffer out = {0};
    char *indent = NULL;

    if (!fnState || !fnState->postExpr) {
        return dupRange(lineStart, lineEnd);
    }

    while (cursor < lineEnd && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    exprStart = cursor;
    exprEnd = lineEnd;
    while (exprEnd > exprStart && isspace((unsigned char)exprEnd[-1])) {
        exprEnd--;
    }
    if (exprEnd > exprStart && exprEnd[-1] == ';') {
        exprEnd--;
    }
    while (exprEnd > exprStart && isspace((unsigned char)exprEnd[-1])) {
        exprEnd--;
    }

    indent = dupRange(lineStart, body);
    if (!indent) {
        return NULL;
    }

    if (exprStart < exprEnd) {
        if (!bufferAppend(&out, indent) ||
            !bufferAppend(&out, "result = ") ||
            !bufferAppendN(&out, exprStart, (size_t)(exprEnd - exprStart)) ||
            !bufferAppend(&out, ";\n")) {
            free(indent);
            free(out.data);
            return NULL;
        }
    }
    if (!appendContractGuard(&out, indent, fnState->name, "post", fnState->postExpr)) {
        free(indent);
        free(out.data);
        return NULL;
    }
    if (!bufferAppend(&out, indent) ||
        !bufferAppend(&out, exprStart < exprEnd ? "return result;" : "return;")) {
        free(indent);
        free(out.data);
        return NULL;
    }
    free(indent);
    return out.data;
}

static int translateParamList(Buffer *out, const char *start, const char *end) {
    const char *cursor = start;
    int first = 1;

    while (cursor < end) {
        const char *segmentStart = cursor;
        const char *segmentEnd = cursor;
        const char *colon = NULL;
        int depth = 0;

        while (segmentEnd < end) {
            char ch = *segmentEnd;
            if (ch == '(' || ch == '[' || ch == '{' || ch == '<') {
                depth++;
            } else if (ch == ')' || ch == ']' || ch == '}' || ch == '>') {
                if (depth > 0) {
                    depth--;
                }
            } else if (ch == ':' && depth == 0 && !colon) {
                colon = segmentEnd;
            } else if (ch == ',' && depth == 0) {
                break;
            }
            segmentEnd++;
        }

        segmentStart = skipSpaces(segmentStart);
        while (segmentEnd > segmentStart && isspace((unsigned char)segmentEnd[-1])) {
            segmentEnd--;
        }
        if (segmentStart < segmentEnd) {
            if (!first && !bufferAppend(out, ", ")) {
                return 0;
            }
            if (colon) {
                if (!appendMappedType(out, colon + 1, segmentEnd) ||
                    !bufferAppend(out, " ") ||
                    !appendIdentifier(out, segmentStart, colon)) {
                    return 0;
                }
            } else {
                if (!bufferAppendN(out, segmentStart, (size_t)(segmentEnd - segmentStart))) {
                    return 0;
                }
            }
            first = 0;
        }

        cursor = segmentEnd;
        if (cursor < end && *cursor == ',') {
            cursor++;
        }
        cursor = skipSpaces(cursor);
    }
    return 1;
}

static char *translateFnLine(const char *lineStart, const char *body, const char *lineEnd) {
    const char *nameStart = body + 2;
    const char *nameEnd;
    const char *paramsOpen;
    const char *paramsClose;
    const char *arrow;
    const char *typeStart;
    const char *typeEnd;
    Buffer out = {0};

    while (*nameStart && isspace((unsigned char)*nameStart)) {
        nameStart++;
    }
    nameEnd = nameStart;
    while (*nameEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
        nameEnd++;
    }
    paramsOpen = findCharInRange(nameEnd, lineEnd, '(');
    paramsClose = paramsOpen ? findLastCharInRange(paramsOpen, lineEnd, ')') : NULL;
    arrow = paramsClose ? findSubstringInRange(paramsClose, lineEnd, "->") : NULL;
    if (!paramsOpen || !paramsClose || !arrow) {
        return dupRange(lineStart, lineEnd);
    }
    typeStart = arrow + 2;
    while (*typeStart && isspace((unsigned char)*typeStart)) {
        typeStart++;
    }
    typeEnd = typeStart;
    while (*typeEnd && *typeEnd != '{') {
        typeEnd++;
    }
    while (typeEnd > typeStart && isspace((unsigned char)typeEnd[-1])) {
        typeEnd--;
    }

    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !appendMappedType(&out, typeStart, typeEnd) ||
        !bufferAppend(&out, " ") ||
        !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart)) ||
        !bufferAppend(&out, "(") ||
        !translateParamList(&out, paramsOpen + 1, paramsClose) ||
        !bufferAppend(&out, ")")) {
        free(out.data);
        return NULL;
    }

    if (*typeEnd) {
        const char *suffix = typeEnd;
        while (suffix < lineEnd && *suffix != '{') {
            suffix++;
        }
        if (suffix < lineEnd && !bufferAppendN(&out, suffix, (size_t)(lineEnd - suffix))) {
            free(out.data);
            return NULL;
        }
    }
    return out.data;
}

static char *translateTypedDeclLine(const char *lineStart,
                                    const char *body,
                                    const char *lineEnd,
                                    int isConst) {
    const char *cursor = body + (isConst ? 5 : 3);
    const char *nameStart;
    const char *nameEnd;
    const char *colon;
    const char *afterColon;
    const char *typeEnd;
    const char *equals = NULL;
    Buffer out = {0};

    cursor = skipSpaces(cursor);
    nameStart = cursor;
    while (*cursor && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    nameEnd = cursor;
    cursor = skipSpaces(cursor);
    if (*cursor != ':') {
        return NULL;
    }
    colon = cursor;
    afterColon = skipSpaces(colon + 1);
    typeEnd = afterColon;
    while (*typeEnd && *typeEnd != '=' && *typeEnd != ';') {
        typeEnd++;
    }
    while (typeEnd > afterColon && isspace((unsigned char)typeEnd[-1])) {
        typeEnd--;
    }
    equals = skipSpaces(typeEnd);

    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart))) {
        free(out.data);
        return NULL;
    }
    if (!isConst && equals < lineEnd && *equals == '=') {
        const char *exprStart = skipSpaces(equals + 1);
        const char *typeNameStart = afterColon;
        const char *typeNameEnd = typeEnd;
        const char *initNameEnd = exprStart;
        const char *openBrace;
        const char *closeBrace;
        const char *indentEnd = body;
        Buffer initOut = {0};
        int typeMatches = 0;

        while (typeNameStart < typeNameEnd && isspace((unsigned char)*typeNameStart)) {
            typeNameStart++;
        }
        while (typeNameEnd > typeNameStart && isspace((unsigned char)typeNameEnd[-1])) {
            typeNameEnd--;
        }
        while (initNameEnd < lineEnd && (isalnum((unsigned char)*initNameEnd) || *initNameEnd == '_')) {
            initNameEnd++;
        }
        openBrace = skipSpaces(initNameEnd);
        closeBrace = lineEnd;
        while (closeBrace > exprStart && isspace((unsigned char)closeBrace[-1])) {
            closeBrace--;
        }
        if (closeBrace > exprStart && closeBrace[-1] == ';') {
            closeBrace--;
        }
        while (closeBrace > exprStart && isspace((unsigned char)closeBrace[-1])) {
            closeBrace--;
        }
        if (openBrace < lineEnd && *openBrace == '{' &&
            closeBrace > openBrace && closeBrace[-1] == '}') {
            typeMatches = ((size_t)(typeNameEnd - typeNameStart) == (size_t)(initNameEnd - exprStart) &&
                           strncmp(typeNameStart, exprStart, (size_t)(typeNameEnd - typeNameStart)) == 0);
        }
        if (typeMatches) {
            const char *entryCursor = openBrace + 1;

            if (!appendMappedType(&initOut, afterColon, typeEnd) ||
                !bufferAppend(&initOut, " ") ||
                !bufferAppendN(&initOut, nameStart, (size_t)(nameEnd - nameStart)) ||
                !bufferAppend(&initOut, " = new ") ||
                !bufferAppendN(&initOut, exprStart, (size_t)(initNameEnd - exprStart)) ||
                !bufferAppend(&initOut, "();")) {
                free(initOut.data);
                free(out.data);
                return NULL;
            }
            while (entryCursor < closeBrace - 1) {
                const char *segmentStart;
                const char *segmentEnd;
                const char *fieldEnd;
                const char *fieldNameEnd;
                const char *valueStart;
                const char *valueEnd;
                int depth = 0;

                while (entryCursor < closeBrace - 1 &&
                       (isspace((unsigned char)*entryCursor) || *entryCursor == ',')) {
                    entryCursor++;
                }
                if (entryCursor >= closeBrace - 1) {
                    break;
                }
                segmentStart = entryCursor;
                segmentEnd = segmentStart;
                fieldEnd = NULL;
                while (segmentEnd < closeBrace - 1) {
                    char ch = *segmentEnd;

                    if (ch == '(' || ch == '[' || ch == '{' || ch == '<') {
                        depth++;
                    } else if (ch == ')' || ch == ']' || ch == '}' || ch == '>') {
                        if (depth > 0) {
                            depth--;
                        }
                    } else if (ch == ':' && depth == 0 && !fieldEnd) {
                        fieldEnd = segmentEnd;
                    } else if (ch == ',' && depth == 0) {
                        break;
                    }
                    segmentEnd++;
                }
                if (!fieldEnd) {
                    free(initOut.data);
                    free(out.data);
                    return dupRange(lineStart, lineEnd);
                }
                fieldNameEnd = fieldEnd;
                while (fieldNameEnd > segmentStart && isspace((unsigned char)fieldNameEnd[-1])) {
                    fieldNameEnd--;
                }
                valueStart = skipSpaces(fieldEnd + 1);
                valueEnd = segmentEnd;
                while (valueEnd > valueStart && isspace((unsigned char)valueEnd[-1])) {
                    valueEnd--;
                }
                if (fieldNameEnd == segmentStart || valueEnd == valueStart) {
                    free(initOut.data);
                    free(out.data);
                    return dupRange(lineStart, lineEnd);
                }
                if (!bufferAppend(&initOut, "\n") ||
                    !bufferAppendN(&initOut, lineStart, (size_t)(indentEnd - lineStart)) ||
                    !bufferAppendN(&initOut, nameStart, (size_t)(nameEnd - nameStart)) ||
                    !bufferAppend(&initOut, ".") ||
                    !bufferAppendN(&initOut, segmentStart, (size_t)(fieldNameEnd - segmentStart)) ||
                    !bufferAppend(&initOut, " = ") ||
                    !bufferAppendN(&initOut, valueStart, (size_t)(valueEnd - valueStart)) ||
                    !bufferAppend(&initOut, ";")) {
                    free(initOut.data);
                    free(out.data);
                    return NULL;
                }
                entryCursor = segmentEnd;
                if (entryCursor < closeBrace - 1 && *entryCursor == ',') {
                    entryCursor++;
                }
            }
            free(out.data);
            return initOut.data;
        }
    }
    if (isConst && !bufferAppend(&out, "const ")) {
        free(out.data);
        return NULL;
    }
    if (!appendMappedType(&out, afterColon, typeEnd) ||
        !bufferAppend(&out, " ") ||
        !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart)) ||
        (typeEnd < lineEnd && !bufferAppendN(&out, typeEnd, (size_t)(lineEnd - typeEnd)))) {
        free(out.data);
        return NULL;
    }
    return out.data;
}

static char *translateInferredDeclLine(const char *lineStart,
                                       const char *body,
                                       const char *lineEnd,
                                       int isConst,
                                       const AetherBindingTable *bindings,
                                       const AetherFunctionTable *functions) {
    const char *cursor = body + (isConst ? 5 : 3);
    const char *nameStart;
    const char *nameEnd;
    const char *equals;
    const char *exprStart;
    char *typeName = NULL;
    Buffer out = {0};

    cursor = skipSpaces(cursor);
    nameStart = cursor;
    while (cursor < lineEnd && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    nameEnd = cursor;
    equals = skipSpaces(cursor);
    if (nameEnd == nameStart || equals >= lineEnd || *equals != '=') {
        return dupRange(lineStart, lineEnd);
    }
    exprStart = skipSpaces(equals + 1);

    if (isConst) {
        if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
            !bufferAppend(&out, "const ") ||
            !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart)) ||
            !bufferAppendN(&out, equals, (size_t)(lineEnd - equals))) {
            free(out.data);
            return NULL;
        }
        return out.data;
    }

    typeName = inferAetherBindingTypeName(exprStart, lineEnd, bindings, functions);
    if (!typeName) {
        fprintf(stderr,
                "Aether declaration rewrite error: let binding '%.*s' needs an explicit type.\n",
                (int)(nameEnd - nameStart),
                nameStart);
        return NULL;
    }

    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppend(&out, mapTypeName(typeName)) ||
        !bufferAppend(&out, " ") ||
        !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart)) ||
        !bufferAppendN(&out, equals, (size_t)(lineEnd - equals))) {
        free(typeName);
        free(out.data);
        return NULL;
    }
    free(typeName);
    return out.data;
}

static char *translateConditionLine(const char *lineStart,
                                    const char *body,
                                    const char *lineEnd,
                                    const char *keyword) {
    const char *exprStart;
    const char *brace;
    Buffer out = {0};
    size_t keywordLen = keyword ? strlen(keyword) : 0;

    if (!keyword || keywordLen == 0) {
        return dupRange(lineStart, lineEnd);
    }
    exprStart = body + keywordLen;
    while (exprStart < lineEnd && isspace((unsigned char)*exprStart)) {
        exprStart++;
    }
    if (exprStart >= lineEnd || *exprStart == '(') {
        return dupRange(lineStart, lineEnd);
    }

    brace = findLastCharInRange(exprStart, lineEnd, '{');
    if (!brace) {
        return dupRange(lineStart, lineEnd);
    }

    while (brace > exprStart && isspace((unsigned char)brace[-1])) {
        brace--;
    }

    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppend(&out, keyword) ||
        !bufferAppend(&out, " (") ||
        !bufferAppendN(&out, exprStart, (size_t)(brace - exprStart)) ||
        !bufferAppend(&out, ")") ||
        !bufferAppendN(&out, brace, (size_t)(lineEnd - brace))) {
        free(out.data);
        return NULL;
    }

    return out.data;
}

static char *translateLoopRangeLine(const char *lineStart, const char *body, const char *lineEnd) {
    const char *nameStart = body + 4;
    const char *nameEnd;
    const char *inKw;
    const char *exprStart;
    const char *rangeOp = NULL;
    const char *exprEnd;
    const char *brace;
    const char *endExprStart;
    const char *endExprEnd;
    const char *scan;
    int depth = 0;
    Buffer out = {0};

    nameStart = skipSpaces(nameStart);
    nameEnd = nameStart;
    while (nameEnd < lineEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
        nameEnd++;
    }
    inKw = skipSpaces(nameEnd);
    if (!startsWithWord(inKw, lineEnd, "in")) {
        return dupRange(lineStart, lineEnd);
    }
    exprStart = skipSpaces(inKw + 2);
    brace = findLastCharInRange(exprStart, lineEnd, '{');
    if (!brace) {
        return dupRange(lineStart, lineEnd);
    }

    scan = exprStart;
    while (scan < brace) {
        if (*scan == '"' || *scan == '\'') {
            char quote = *scan++;
            while (scan < brace) {
                if (*scan == '\\' && scan + 1 < brace) {
                    scan += 2;
                    continue;
                }
                if (*scan == quote) {
                    scan++;
                    break;
                }
                scan++;
            }
            continue;
        }
        if (*scan == '(' || *scan == '[' || *scan == '{') {
            depth++;
        } else if ((*scan == ')' || *scan == ']' || *scan == '}') && depth > 0) {
            depth--;
        } else if (*scan == '.' && scan + 1 < brace && scan[1] == '.' && depth == 0) {
            rangeOp = scan;
            break;
        }
        scan++;
    }
    if (!rangeOp) {
        return dupRange(lineStart, lineEnd);
    }
    exprEnd = rangeOp;
    while (exprEnd > exprStart && isspace((unsigned char)exprEnd[-1])) {
        exprEnd--;
    }
    endExprStart = rangeOp + 2;
    while (endExprStart < brace && isspace((unsigned char)*endExprStart)) {
        endExprStart++;
    }
    endExprEnd = brace;
    while (endExprEnd > endExprStart && isspace((unsigned char)endExprEnd[-1])) {
        endExprEnd--;
    }

    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppend(&out, "for (int ") ||
        !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart)) ||
        !bufferAppend(&out, " = ") ||
        !bufferAppendN(&out, exprStart, (size_t)(exprEnd - exprStart)) ||
        !bufferAppend(&out, "; ") ||
        !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart)) ||
        !bufferAppend(&out, " < ") ||
        !bufferAppendN(&out, endExprStart, (size_t)(endExprEnd - endExprStart)) ||
        !bufferAppend(&out, "; ") ||
        !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart)) ||
        !bufferAppend(&out, " = ") ||
        !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart)) ||
        !bufferAppend(&out, " + 1)") ||
        !bufferAppendN(&out, brace, (size_t)(lineEnd - brace))) {
        free(out.data);
        return NULL;
    }

    return out.data;
}

static char *translateLoopLine(const char *lineStart, const char *body, const char *lineEnd) {
    const char *cursor = skipSpaces(body + 4);
    const char *brace;
    Buffer out = {0};

    if (cursor >= lineEnd || *cursor == '{') {
        brace = findLastCharInRange(cursor, lineEnd, '{');

        if (!brace) {
            return dupRange(lineStart, lineEnd);
        }
        if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
            !bufferAppend(&out, "while (true)") ||
            !bufferAppendN(&out, brace, (size_t)(lineEnd - brace))) {
            free(out.data);
            return NULL;
        }
        return out.data;
    }

    if (findSubstringInRange(cursor, lineEnd, "..") != NULL &&
        findSubstringInRange(cursor, lineEnd, "in") != NULL) {
        return translateLoopRangeLine(lineStart, body, lineEnd);
    }

    brace = findLastCharInRange(cursor, lineEnd, '{');
    if (!brace) {
        return dupRange(lineStart, lineEnd);
    }
    while (brace > cursor && isspace((unsigned char)brace[-1])) {
        brace--;
    }
    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppend(&out, "while (") ||
        !bufferAppendN(&out, cursor, (size_t)(brace - cursor)) ||
        !bufferAppend(&out, ")") ||
        !bufferAppendN(&out, brace, (size_t)(lineEnd - brace))) {
        free(out.data);
        return NULL;
    }
    return out.data;
}

static char *translateForRangeLine(const char *lineStart, const char *body, const char *lineEnd) {
    const char *nameStart = body + 3;
    const char *nameEnd;
    const char *inKw;
    const char *exprStart;
    const char *rangeOp = NULL;
    const char *exprEnd;
    const char *brace;
    const char *endExprStart;
    const char *endExprEnd;
    const char *scan;
    int depth = 0;
    Buffer out = {0};

    nameStart = skipSpaces(nameStart);
    nameEnd = nameStart;
    while (nameEnd < lineEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
        nameEnd++;
    }
    inKw = skipSpaces(nameEnd);
    if (!startsWithWord(inKw, lineEnd, "in")) {
        return dupRange(lineStart, lineEnd);
    }
    exprStart = skipSpaces(inKw + 2);
    brace = findLastCharInRange(exprStart, lineEnd, '{');
    if (!brace) {
        return dupRange(lineStart, lineEnd);
    }

    scan = exprStart;
    while (scan < brace) {
        if (*scan == '"' || *scan == '\'') {
            char quote = *scan++;
            while (scan < brace) {
                if (*scan == '\\' && scan + 1 < brace) {
                    scan += 2;
                    continue;
                }
                if (*scan == quote) {
                    scan++;
                    break;
                }
                scan++;
            }
            continue;
        }
        if (*scan == '(' || *scan == '[' || *scan == '{') {
            depth++;
        } else if ((*scan == ')' || *scan == ']' || *scan == '}') && depth > 0) {
            depth--;
        } else if (*scan == '.' && scan + 1 < brace && scan[1] == '.' && depth == 0) {
            rangeOp = scan;
            break;
        }
        scan++;
    }
    if (!rangeOp) {
        return dupRange(lineStart, lineEnd);
    }
    exprEnd = rangeOp;
    while (exprEnd > exprStart && isspace((unsigned char)exprEnd[-1])) {
        exprEnd--;
    }
    endExprStart = rangeOp + 2;
    while (endExprStart < brace && isspace((unsigned char)*endExprStart)) {
        endExprStart++;
    }
    endExprEnd = brace;
    while (endExprEnd > endExprStart && isspace((unsigned char)endExprEnd[-1])) {
        endExprEnd--;
    }

    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppend(&out, "for (int ") ||
        !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart)) ||
        !bufferAppend(&out, " = ") ||
        !bufferAppendN(&out, exprStart, (size_t)(exprEnd - exprStart)) ||
        !bufferAppend(&out, "; ") ||
        !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart)) ||
        !bufferAppend(&out, " < ") ||
        !bufferAppendN(&out, endExprStart, (size_t)(endExprEnd - endExprStart)) ||
        !bufferAppend(&out, "; ") ||
        !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart)) ||
        !bufferAppend(&out, " = ") ||
        !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart)) ||
        !bufferAppend(&out, " + 1)") ||
        !bufferAppendN(&out, brace, (size_t)(lineEnd - brace))) {
        free(out.data);
        return NULL;
    }

    return out.data;
}

static char *translateExportLine(const char *lineStart,
                                 const char *body,
                                 const char *lineEnd,
                                 const AetherBindingTable *bindings,
                                 const AetherFunctionTable *functions) {
    const char *rest = skipSpaces(body + 6);
    char *translatedRest = NULL;
    Buffer out = {0};

    if (rest >= lineEnd) {
        return dupRange(lineStart, lineEnd);
    }
    if (strncmp(rest, "fn ", 3) == 0) {
        translatedRest = translateFnLine(rest, rest, lineEnd);
    } else if (strncmp(rest, "let ", 4) == 0 && hasTypedDeclSeparator(rest, lineEnd, 0)) {
        translatedRest = translateTypedDeclLine(rest, rest, lineEnd, 0);
    } else if (strncmp(rest, "const ", 6) == 0 && hasTypedDeclSeparator(rest, lineEnd, 1)) {
        translatedRest = translateTypedDeclLine(rest, rest, lineEnd, 1);
    } else if (strncmp(rest, "let ", 4) == 0) {
        translatedRest = translateInferredDeclLine(rest, rest, lineEnd, 0, bindings, functions);
    } else if (strncmp(rest, "const ", 6) == 0) {
        translatedRest = translateInferredDeclLine(rest, rest, lineEnd, 1, bindings, functions);
    } else {
        return dupRange(lineStart, lineEnd);
    }
    if (!translatedRest) {
        return NULL;
    }
    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppend(&out, "export ") ||
        !bufferAppend(&out, translatedRest)) {
        free(translatedRest);
        free(out.data);
        return NULL;
    }
    free(translatedRest);
    return out.data;
}

static char *translateTypeLine(const char *lineStart, const char *body, const char *lineEnd) {
    Buffer out = {0};

    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppend(&out, "class ") ||
        !bufferAppendN(&out, body + 5, (size_t)(lineEnd - (body + 5)))) {
        free(out.data);
        return NULL;
    }
    return out.data;
}

static char *translateTypeFieldLine(const char *lineStart, const char *body, const char *lineEnd) {
    const char *nameStart = body;
    const char *nameEnd = body;
    const char *colon;
    const char *typeStart;
    const char *typeEnd;
    Buffer out = {0};

    while (nameEnd < lineEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
        nameEnd++;
    }
    if (nameEnd == nameStart) {
        return dupRange(lineStart, lineEnd);
    }
    colon = skipSpaces(nameEnd);
    if (colon >= lineEnd || *colon != ':') {
        return dupRange(lineStart, lineEnd);
    }
    typeStart = skipSpaces(colon + 1);
    typeEnd = typeStart;
    while (typeEnd < lineEnd && *typeEnd != ';') {
        typeEnd++;
    }
    while (typeEnd > typeStart && isspace((unsigned char)typeEnd[-1])) {
        typeEnd--;
    }
    if (typeEnd == typeStart || (typeEnd < lineEnd && *typeEnd != ';' && lineEnd[-1] != ';')) {
        return dupRange(lineStart, lineEnd);
    }

    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !appendMappedType(&out, typeStart, typeEnd) ||
        !bufferAppend(&out, " ") ||
        !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart)) ||
        !bufferAppend(&out, ";")) {
        free(out.data);
        return NULL;
    }
    return out.data;
}

static char *translateLine(const char *lineStart,
                           const char *lineEnd,
                           JsonAliasState *jsonState,
                           const AetherBindingTable *bindings,
                           const AetherFunctionTable *functions) {
    const char *body = lineStart;
    Buffer out = {0};

    while (body < lineEnd && (*body == ' ' || *body == '\t')) {
        body++;
    }

    if (body >= lineEnd) {
        return dupRange(lineStart, lineEnd);
    }
    if (strncmp(body, "@pre", 4) == 0 ||
        strncmp(body, "@post", 5) == 0 ||
        strncmp(body, "@cost", 5) == 0 ||
        strncmp(body, "@pure", 5) == 0) {
        if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
            !bufferAppend(&out, "// ") ||
            !bufferAppendN(&out, body, (size_t)(lineEnd - body))) {
            free(out.data);
            return NULL;
        }
        return out.data;
    }
    if (strncmp(body, "export ", 7) == 0) {
        return translateExportLine(lineStart, body, lineEnd, bindings, functions);
    }
    if (strncmp(body, "fn ", 3) == 0) {
        return translateFnLine(lineStart, body, lineEnd);
    }
    if (strncmp(body, "let ", 4) == 0 && hasTypedDeclSeparator(body, lineEnd, 0)) {
        return translateTypedDeclLine(lineStart, body, lineEnd, 0);
    }
    if (strncmp(body, "const ", 6) == 0 && hasTypedDeclSeparator(body, lineEnd, 1)) {
        return translateTypedDeclLine(lineStart, body, lineEnd, 1);
    }
    if (strncmp(body, "let ", 4) == 0) {
        return translateInferredDeclLine(lineStart, body, lineEnd, 0, bindings, functions);
    }
    if (strncmp(body, "const ", 6) == 0) {
        return translateInferredDeclLine(lineStart, body, lineEnd, 1, bindings, functions);
    }
    if (strncmp(body, "use ", 4) == 0) {
        if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
            !bufferAppend(&out, "#import ") ||
            !bufferAppendN(&out, body + 4, (size_t)(lineEnd - (body + 4)))) {
            free(out.data);
            return NULL;
        }
        return out.data;
    }
    if (strncmp(body, "mod ", 4) == 0) {
        if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
            !bufferAppend(&out, "module ") ||
            !bufferAppendN(&out, body + 4, (size_t)(lineEnd - (body + 4)))) {
            free(out.data);
            return NULL;
        }
        return out.data;
    }
    if (strncmp(body, "type ", 5) == 0) {
        return translateTypeLine(lineStart, body, lineEnd);
    }
    if (strncmp(body, "if ", 3) == 0) {
        return translateConditionLine(lineStart, body, lineEnd, "if");
    }
    if (strncmp(body, "for ", 4) == 0) {
        return translateForRangeLine(lineStart, body, lineEnd);
    }
    if (strncmp(body, "loop", 4) == 0 &&
        (body[4] == '\0' || isspace((unsigned char)body[4]) || body[4] == '{')) {
        return translateLoopLine(lineStart, body, lineEnd);
    }
    if (strncmp(body, "while ", 6) == 0) {
        return translateConditionLine(lineStart, body, lineEnd, "while");
    }
    if (strncmp(body, "fx", 2) == 0 &&
        (body[2] == '\0' || isspace((unsigned char)body[2]) || body[2] == '{')) {
        if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
            !bufferAppendN(&out, body + 2, (size_t)(lineEnd - (body + 2)))) {
            free(out.data);
            return NULL;
        }
        return out.data;
    }

    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart))) {
        free(out.data);
        return NULL;
    }

    while (body < lineEnd) {
        if ((body == lineStart || !(isalnum((unsigned char)body[-1]) || body[-1] == '_')) &&
            (size_t)(lineEnd - body) >= 3 &&
            strncmp(body, "ret", 3) == 0 &&
            (body + 3 == lineEnd || !(isalnum((unsigned char)body[3]) || body[3] == '_'))) {
            if (!bufferAppend(&out, "return")) {
                free(out.data);
                return NULL;
            }
            body += 3;
            continue;
        }
        if ((body == lineStart || !(isalnum((unsigned char)body[-1]) || body[-1] == '_')) &&
            (size_t)(lineEnd - body) >= 4 &&
            strncmp(body, "self", 4) == 0 &&
            (body + 4 == lineEnd || !(isalnum((unsigned char)body[4]) || body[4] == '_'))) {
            if (!bufferAppend(&out, "myself")) {
                free(out.data);
                return NULL;
            }
            body += 4;
            continue;
        }
        if ((body == lineStart || !(isalnum((unsigned char)body[-1]) || body[-1] == '_')) &&
            isalpha((unsigned char)*body)) {
            const char *nameStart = body;
            const char *nameEnd = body + 1;
            const char *advancedCursor = NULL;

            while (nameEnd < lineEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
                nameEnd++;
            }
            if (nameEnd < lineEnd &&
                *skipSpacesInRange(nameEnd, lineEnd) == '(' &&
                appendAetherCapabilityAlias(&out,
                                            nameStart,
                                            (size_t)(nameEnd - nameStart),
                                            skipSpacesInRange(nameEnd, lineEnd),
                                            &advancedCursor)) {
                body = advancedCursor;
                continue;
            }
            if (nameEnd < lineEnd &&
                *skipSpacesInRange(nameEnd, lineEnd) == '(' &&
                appendAetherBuiltinAlias(&out, nameStart, (size_t)(nameEnd - nameStart))) {
                body = nameEnd;
                continue;
            }
            if (jsonState &&
                nameEnd < lineEnd &&
                *skipSpacesInRange(nameEnd, lineEnd) == '(' &&
                appendJsonAliasReplacement(&out, nameStart, (size_t)(nameEnd - nameStart), jsonState)) {
                body = nameEnd;
                continue;
            }
        }
        if (!bufferAppendN(&out, body, 1)) {
            free(out.data);
            return NULL;
        }
        body++;
    }

    return out.data;
}

char *aetherRewriteSource(const char *source, const char *path) {
    char *preprocessed = NULL;
    const char *cursor;
    Buffer out = {0};
    PendingContracts pending = {0};
    FunctionContracts fnState = {0};
    ParBlockState parState = {0};
    TypeBlockState typeState = {0};
    JsonAliasState jsonState = {0};
    AetherBindingTable bindingTable = {0};
    AetherFunctionTable functionTable = {0};
    ToonLiteralTable toonTable = {0};
    int braceDepth = 0;
    (void)path;

    if (!source) {
        return NULL;
    }
    preprocessed = preprocessToonBlocks(source, path);
    if (!preprocessed) {
        return NULL;
    }
    cursor = preprocessed;

    while (*cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *body;
        char *translated = NULL;
        int lineDelta = 0;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }

        body = lineStart;
        while (body < lineEnd && (*body == ' ' || *body == '\t')) {
            body++;
        }

        if (!maybeLoadImportedBindings(&bindingTable, &functionTable, body, lineEnd, path)) {
            freePendingContracts(&pending);
            clearFunctionContracts(&fnState);
            clearParBlockState(&parState);
            clearTypeBlockState(&typeState);
            freeAetherBindingTable(&bindingTable);
            freeAetherFunctionTable(&functionTable);
            freeToonLiteralTable(&toonTable);
            free(preprocessed);
            free(out.data);
            return NULL;
        }
        maybeRecordToonLiteralBinding(&toonTable, body, lineEnd);
        maybeRecordAetherFunctionReturnType(&functionTable, body, lineEnd, NULL);
        maybeRecordAetherBindingType(&bindingTable, body, lineEnd, &functionTable);

        if (fnState.active && fnState.postExpr && braceDepth == fnState.bodyDepth) {
            if (isStandaloneCloseBrace(body, lineEnd)) {
                char *indent = dupRange(lineStart, body);
                if (!indent) {
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    clearParBlockState(&parState);
                    clearTypeBlockState(&typeState);
                    freeAetherBindingTable(&bindingTable);
                    freeAetherFunctionTable(&functionTable);
                    freeToonLiteralTable(&toonTable);
                    free(preprocessed);
                    free(out.data);
                    return NULL;
                }
                if (!appendContractGuard(&out, indent, fnState.name, "post", fnState.postExpr)) {
                    free(indent);
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    clearParBlockState(&parState);
                    clearTypeBlockState(&typeState);
                    freeAetherBindingTable(&bindingTable);
                    freeAetherFunctionTable(&functionTable);
                    freeToonLiteralTable(&toonTable);
                    free(preprocessed);
                    free(out.data);
                    return NULL;
                }
                free(indent);
            }
        }

        if (parState.active && braceDepth == parState.bodyDepth) {
            if (isStandaloneCloseBrace(body, lineEnd)) {
                if (parState.joinLines.len > 0 && !bufferAppend(&out, parState.joinLines.data)) {
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    clearParBlockState(&parState);
                    clearTypeBlockState(&typeState);
                    freeAetherBindingTable(&bindingTable);
                    freeAetherFunctionTable(&functionTable);
                    freeToonLiteralTable(&toonTable);
                    free(preprocessed);
                    free(out.data);
                    return NULL;
                }
            } else if (body < lineEnd && !isLineComment(body, lineEnd)) {
                if (!isParallelCallStatement(body, lineEnd)) {
                    fprintf(stderr,
                            "Aether par rewrite error: only direct call statements are allowed inside par blocks.\n");
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    clearParBlockState(&parState);
                    clearTypeBlockState(&typeState);
                    freeAetherBindingTable(&bindingTable);
                    freeAetherFunctionTable(&functionTable);
                    freeToonLiteralTable(&toonTable);
                    free(preprocessed);
                    free(out.data);
                    return NULL;
                }
                translated = translateParallelCallLine(lineStart, body, lineEnd, &parState);
                if (!translated) {
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    clearParBlockState(&parState);
                    clearTypeBlockState(&typeState);
                    freeAetherBindingTable(&bindingTable);
                    freeAetherFunctionTable(&functionTable);
                    freeToonLiteralTable(&toonTable);
                    free(preprocessed);
                    free(out.data);
                    return NULL;
                }
            }
        }

        if (startsWithWord(body, lineEnd, "@pre")) {
            char *expr = extractAnnotationExpr(body, lineEnd, "@pre");
            pending.preExpr = appendContractExpr(pending.preExpr, expr);
            free(expr);
        } else if (startsWithWord(body, lineEnd, "@post")) {
            char *expr = extractAnnotationExpr(body, lineEnd, "@post");
            pending.postExpr = appendContractExpr(pending.postExpr, expr);
            free(expr);
        } else if ((pending.preExpr || pending.postExpr) &&
                   body < lineEnd &&
                   !isLineComment(body, lineEnd) &&
                   !startsWithWord(body, lineEnd, "@pure") &&
                   !startsWithWord(body, lineEnd, "@cost") &&
                   !startsWithWord(body, lineEnd, "fn")) {
            freePendingContracts(&pending);
        }

        if (!translated) {
            if (startsWithWord(body, lineEnd, "par") &&
                findCharInRange(body, lineEnd, '{') != NULL) {
                Buffer parOpen = {0};

                if (!bufferAppendN(&parOpen, lineStart, (size_t)(body - lineStart)) ||
                    !bufferAppend(&parOpen, "{")) {
                    free(parOpen.data);
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    clearParBlockState(&parState);
                    clearTypeBlockState(&typeState);
                    freeAetherBindingTable(&bindingTable);
                    freeAetherFunctionTable(&functionTable);
                    freeToonLiteralTable(&toonTable);
                    free(preprocessed);
                    free(out.data);
                    return NULL;
                }
                translated = parOpen.data;
            } else if (fnState.active && fnState.postExpr && startsWithWord(body, lineEnd, "ret")) {
                translated = translateReturnWithPost(lineStart, body, lineEnd, &fnState);
            } else if (typeState.active &&
                       !startsWithWord(body, lineEnd, "fn") &&
                       !startsWithWord(body, lineEnd, "@pre") &&
                       !startsWithWord(body, lineEnd, "@post") &&
                       !startsWithWord(body, lineEnd, "@pure") &&
                       !startsWithWord(body, lineEnd, "@cost") &&
                       !startsWithWord(body, lineEnd, "if") &&
                       !startsWithWord(body, lineEnd, "while") &&
                       !startsWithWord(body, lineEnd, "for") &&
                       !startsWithWord(body, lineEnd, "let") &&
                       !startsWithWord(body, lineEnd, "const") &&
                       !startsWithWord(body, lineEnd, "ret") &&
                       !startsWithWord(body, lineEnd, "fx") &&
                       !startsWithWord(body, lineEnd, "par") &&
                       !startsWithWord(body, lineEnd, "type") &&
                       !startsWithWord(body, lineEnd, "mod") &&
                       !startsWithWord(body, lineEnd, "use") &&
                       !startsWithWord(body, lineEnd, "export") &&
                       !isStandaloneCloseBrace(body, lineEnd) &&
                       !isLineComment(body, lineEnd)) {
                translated = translateTypeFieldLine(lineStart, body, lineEnd);
            } else {
                translated = translateLine(lineStart, lineEnd, &jsonState, &bindingTable, &functionTable);
            }
        }
        if (!translated) {
            freePendingContracts(&pending);
            clearFunctionContracts(&fnState);
            clearParBlockState(&parState);
            clearTypeBlockState(&typeState);
            freeAetherBindingTable(&bindingTable);
            freeAetherFunctionTable(&functionTable);
            freeToonLiteralTable(&toonTable);
            free(preprocessed);
            free(out.data);
            return NULL;
        }
        {
            char *aliased = applyJsonAliasesToLine(translated, &jsonState, &toonTable);
            if (!aliased) {
                free(translated);
                freePendingContracts(&pending);
                clearFunctionContracts(&fnState);
                clearParBlockState(&parState);
                clearTypeBlockState(&typeState);
                freeAetherBindingTable(&bindingTable);
                freeAetherFunctionTable(&functionTable);
                freeToonLiteralTable(&toonTable);
                free(preprocessed);
                free(out.data);
                return NULL;
            }
            free(translated);
            translated = aliased;
        }
        if (!bufferAppend(&out, translated)) {
            free(translated);
            freePendingContracts(&pending);
            clearFunctionContracts(&fnState);
            clearParBlockState(&parState);
            clearTypeBlockState(&typeState);
            freeAetherBindingTable(&bindingTable);
            freeAetherFunctionTable(&functionTable);
            freeToonLiteralTable(&toonTable);
            free(preprocessed);
            free(out.data);
            return NULL;
        }
        lineDelta = braceDeltaForLine(translated);

        if (startsWithWord(body, lineEnd, "par") && findCharInRange(body, lineEnd, '{') != NULL) {
            clearParBlockState(&parState);
            parState.active = lineDelta > 0;
            parState.bodyDepth = braceDepth + lineDelta;
            parState.nextHandle = 0;
            parState.indent = buildContractIndent(lineStart, body);
            if (!parState.indent) {
                free(translated);
                freePendingContracts(&pending);
                clearFunctionContracts(&fnState);
                clearParBlockState(&parState);
                clearTypeBlockState(&typeState);
                freeAetherBindingTable(&bindingTable);
                freeAetherFunctionTable(&functionTable);
                freeToonLiteralTable(&toonTable);
                free(preprocessed);
                free(out.data);
                return NULL;
            }
        }

        if (startsWithWord(body, lineEnd, "fn")) {
            char *fnName = NULL;
            char *returnType = NULL;

            if (!extractFunctionSignature(body, lineEnd, &fnName, &returnType)) {
                free(translated);
                freePendingContracts(&pending);
                clearFunctionContracts(&fnState);
                clearParBlockState(&parState);
                clearTypeBlockState(&typeState);
                freeAetherBindingTable(&bindingTable);
                freeAetherFunctionTable(&functionTable);
                freeToonLiteralTable(&toonTable);
                free(preprocessed);
                free(out.data);
                return NULL;
            }

            if ((pending.preExpr || pending.postExpr) && lineDelta > 0) {
                char *indent = buildContractIndent(lineStart, body);
                if (!indent) {
                    free(fnName);
                    free(returnType);
                    free(translated);
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    clearParBlockState(&parState);
                    clearTypeBlockState(&typeState);
                    freeAetherBindingTable(&bindingTable);
                    freeAetherFunctionTable(&functionTable);
                    freeToonLiteralTable(&toonTable);
                    free(preprocessed);
                    free(out.data);
                    return NULL;
                }
                if (pending.preExpr &&
                    !appendContractGuard(&out, indent, fnName, "pre", pending.preExpr)) {
                    free(indent);
                    free(fnName);
                    free(returnType);
                    free(translated);
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    clearParBlockState(&parState);
                    clearTypeBlockState(&typeState);
                    freeAetherBindingTable(&bindingTable);
                    freeAetherFunctionTable(&functionTable);
                    freeToonLiteralTable(&toonTable);
                    free(preprocessed);
                    free(out.data);
                    return NULL;
                }
                free(indent);
            }

            clearFunctionContracts(&fnState);
            fnState.active = lineDelta > 0;
            fnState.bodyDepth = braceDepth + lineDelta;
            fnState.isVoid = returnType && strcmp(returnType, "Void") == 0;
            fnState.name = fnName;
            fnState.postExpr = pending.postExpr
                                   ? dupRange(pending.postExpr,
                                              pending.postExpr + strlen(pending.postExpr))
                                   : NULL;

            free(returnType);
            freePendingContracts(&pending);
        }
        free(translated);

        if (*lineEnd == '\n') {
            if (!bufferAppendN(&out, "\n", 1)) {
                freePendingContracts(&pending);
                clearFunctionContracts(&fnState);
                clearParBlockState(&parState);
                clearTypeBlockState(&typeState);
                freeAetherBindingTable(&bindingTable);
                freeAetherFunctionTable(&functionTable);
                freeToonLiteralTable(&toonTable);
                free(preprocessed);
                free(out.data);
                return NULL;
            }
            lineEnd++;
        }

        if (startsWithWord(body, lineEnd, "type") && lineDelta > 0) {
            clearTypeBlockState(&typeState);
            typeState.active = 1;
            typeState.bodyDepth = braceDepth + lineDelta;
        }
        braceDepth += lineDelta;
        if (fnState.active && braceDepth < fnState.bodyDepth) {
            clearFunctionContracts(&fnState);
        }
        if (parState.active && braceDepth < parState.bodyDepth) {
            clearParBlockState(&parState);
        }
        if (typeState.active && braceDepth < typeState.bodyDepth) {
            clearTypeBlockState(&typeState);
        }
        cursor = lineEnd;
    }

    freePendingContracts(&pending);
    clearFunctionContracts(&fnState);
    clearParBlockState(&parState);
    clearTypeBlockState(&typeState);
    freeAetherBindingTable(&bindingTable);
    freeAetherFunctionTable(&functionTable);
    freeToonLiteralTable(&toonTable);
    free(preprocessed);
    return out.data;
}
