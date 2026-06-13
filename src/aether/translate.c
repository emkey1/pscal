#include "aether/translate.h"
#include "aether/diagnostics.h"
#include "aether/state.h"

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

typedef struct TypeBlockState TypeBlockState;

static int trackRewriteOutputLines(const char *text, int *currentOutputLine, int sourceLine) {
    const char *cursor = text;

    if (!currentOutputLine) {
        return 0;
    }
    if (!aetherNoteRewriteLineMapping(*currentOutputLine, sourceLine)) {
        return 0;
    }
    if (!text) {
        return 1;
    }
    while (*cursor) {
        if (*cursor == '\n') {
            (*currentOutputLine)++;
            if (!aetherNoteRewriteLineMapping(*currentOutputLine, sourceLine)) {
                return 0;
            }
        }
        cursor++;
    }
    return 1;
}

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

static int startsWithWord(const char *body, const char *lineEnd, const char *word);
static int braceDeltaForLine(const char *line);
static void freeTupleItemTypes(char **items, size_t count);
static char *extractSelfReceiverTypeName(const char *paramsStart, const char *paramsEnd);
static int extractFunctionSignature(const char *body,
                                    const char *lineEnd,
                                    char **outName,
                                    char **outReturnType);
static int hasExplicitFunctionReturnType(const char *body, const char *lineEnd);
typedef struct AetherBindingTable AetherBindingTable;
typedef struct AetherFunctionTable AetherFunctionTable;
typedef struct AetherFieldTable AetherFieldTable;
static void splitTypeSuffix(const char *typeStart,
                            const char *typeEnd,
                            const char **outBaseEnd,
                            const char **outSuffixStart);
static char *rewriteMethodScopedExpr(const char *start,
                                     const char *end,
                                     const AetherBindingTable *bindings,
                                     const TypeBlockState *typeState,
                                     int isMethod);
static char *rewriteAetherOpaqueNilComparisons(const char *start,
                                               const char *end,
                                               const AetherBindingTable *bindings,
                                               const AetherFunctionTable *functions,
                                               const AetherFieldTable *fields);
static char *rewriteAetherLenPropertyExpr(const char *start,
                                          const char *end,
                                          const AetherBindingTable *bindings,
                                          const AetherFunctionTable *functions,
                                          const AetherFieldTable *fields);

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

static int isUnsupportedTupleLetPattern(const char *body, const char *lineEnd) {
    const char *cursor;

    if (!body || !lineEnd || !startsWithWord(body, lineEnd, "let")) {
        return 0;
    }
    cursor = skipSpacesInRange(body + 3, lineEnd);
    return cursor < lineEnd && *cursor == '(';
}

static int splitTopLevelCommaList(const char *start,
                                  const char *end,
                                  char ***outItems,
                                  size_t *outCount) {
    const char *cursor = start;
    const char *segmentStart = start;
    char **items = NULL;
    size_t count = 0;
    size_t cap = 0;
    int depthParen = 0;
    int depthBracket = 0;
    int depthBrace = 0;
    int depthAngle = 0;
    int inString = 0;
    char quote = '\0';

    if (!outItems || !outCount || !start || !end || end < start) {
        return 0;
    }
    *outItems = NULL;
    *outCount = 0;

    while (cursor <= end) {
        int atEnd = (cursor == end);
        char ch = atEnd ? '\0' : *cursor;

        if (!atEnd && inString) {
            if (ch == '\\' && cursor + 1 < end) {
                cursor += 2;
                continue;
            }
            if (ch == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (!atEnd && (ch == '"' || ch == '\'')) {
            inString = 1;
            quote = ch;
            cursor++;
            continue;
        }
        if (!atEnd) {
            if (ch == '(') {
                depthParen++;
            } else if (ch == ')') {
                if (depthParen > 0) {
                    depthParen--;
                }
            } else if (ch == '[') {
                depthBracket++;
            } else if (ch == ']') {
                if (depthBracket > 0) {
                    depthBracket--;
                }
            } else if (ch == '{') {
                depthBrace++;
            } else if (ch == '}') {
                if (depthBrace > 0) {
                    depthBrace--;
                }
            } else if (ch == '<') {
                depthAngle++;
            } else if (ch == '>') {
                if (depthAngle > 0) {
                    depthAngle--;
                }
            }
        }
        if (atEnd || (ch == ',' && depthParen == 0 && depthBracket == 0 &&
                      depthBrace == 0 && depthAngle == 0)) {
            char *item;
            const char *segmentEnd = cursor;

            while (segmentStart < segmentEnd && isspace((unsigned char)*segmentStart)) {
                segmentStart++;
            }
            while (segmentEnd > segmentStart && isspace((unsigned char)segmentEnd[-1])) {
                segmentEnd--;
            }
            if (segmentEnd <= segmentStart) {
                freeTupleItemTypes(items, count);
                return 0;
            }
            item = dupRange(segmentStart, segmentEnd);
            if (!item) {
                freeTupleItemTypes(items, count);
                return 0;
            }
            if (count == cap) {
                size_t newCap = cap ? cap * 2 : 4;
                char **resized = (char **)realloc(items, newCap * sizeof(char *));
                if (!resized) {
                    free(item);
                    freeTupleItemTypes(items, count);
                    return 0;
                }
                items = resized;
                cap = newCap;
            }
            items[count++] = item;
            segmentStart = cursor + 1;
        }
        cursor++;
    }

    *outItems = items;
    *outCount = count;
    return count > 0;
}

static int parseTupleTypeList(const char *start,
                              const char *end,
                              char ***outTypes,
                              size_t *outCount) {
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    if (end - start < 2 || *start != '(' || end[-1] != ')') {
        return 0;
    }
    return splitTopLevelCommaList(start + 1, end - 1, outTypes, outCount);
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

static int appendMappedParamTypeAndName(Buffer *out,
                                        const char *typeStart,
                                        const char *typeEnd,
                                        const char *nameStart,
                                        const char *nameEnd) {
    const char *suffixStart = typeEnd;
    const char *baseEnd = typeEnd;

    splitTypeSuffix(typeStart, typeEnd, &baseEnd, &suffixStart);

    if (!appendMappedType(out, typeStart, baseEnd) ||
        !bufferAppend(out, " ") ||
        !appendIdentifier(out, nameStart, nameEnd)) {
        return 0;
    }

    if (suffixStart < typeEnd) {
        const char *suffix = suffixStart;
        while (suffix < typeEnd) {
            if (!isspace((unsigned char)*suffix) &&
                !bufferAppendN(out, suffix, 1)) {
                return 0;
            }
            suffix++;
        }
    }

    return 1;
}

static void splitTypeSuffix(const char *typeStart,
                            const char *typeEnd,
                            const char **outBaseEnd,
                            const char **outSuffixStart) {
    const char *suffixStart = typeEnd;
    const char *baseEnd = typeEnd;
    const char *scan;

    if (!outBaseEnd || !outSuffixStart) {
        return;
    }

    while (baseEnd > typeStart && isspace((unsigned char)baseEnd[-1])) {
        baseEnd--;
    }
    scan = baseEnd;
    while (scan > typeStart) {
        const char *cursor = scan;
        while (cursor > typeStart && isspace((unsigned char)cursor[-1])) {
            cursor--;
        }
        if (cursor - typeStart >= 2 && cursor[-2] == '[' && cursor[-1] == ']') {
            scan = cursor - 2;
            suffixStart = scan;
            continue;
        }
        break;
    }
    baseEnd = suffixStart;
    while (baseEnd > typeStart && isspace((unsigned char)baseEnd[-1])) {
        baseEnd--;
    }

    *outBaseEnd = baseEnd;
    *outSuffixStart = suffixStart;
}

static int appendMappedTypeAndCStringName(Buffer *out,
                                          const char *typeText,
                                          const char *nameText) {
    if (!typeText || !nameText) {
        return 0;
    }
    return appendMappedParamTypeAndName(out,
                                        typeText,
                                        typeText + strlen(typeText),
                                        nameText,
                                        nameText + strlen(nameText));
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

static const char *findMatchingCloseParen(const char *open, const char *end) {
    const char *cursor;
    int depth = 0;
    int inString = 0;
    char quote = '\0';

    if (!open || !end || open >= end || *open != '(') {
        return NULL;
    }
    cursor = open;
    while (cursor < end) {
        char ch = *cursor;

        if (inString) {
            if (ch == '\\' && cursor + 1 < end) {
                cursor += 2;
                continue;
            }
            if (ch == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            inString = 1;
            quote = ch;
            cursor++;
            continue;
        }
        if (ch == '(') {
            depth++;
        } else if (ch == ')') {
            depth--;
            if (depth == 0) {
                return cursor;
            }
        }
        cursor++;
    }
    return NULL;
}

static const char *findMatchingCloseBrace(const char *open, const char *end) {
    const char *cursor;
    int depth = 0;

    if (!open || !end || open >= end || *open != '{') {
        return NULL;
    }
    cursor = open;
    while (cursor < end) {
        if (*cursor == '"' || *cursor == '\'') {
            char quote = *cursor++;
            while (cursor < end) {
                if (*cursor == '\\' && cursor + 1 < end) {
                    cursor += 2;
                    continue;
                }
                if (*cursor == quote) {
                    cursor++;
                    break;
                }
                cursor++;
            }
            continue;
        }
        if (*cursor == '{') {
            depth++;
        } else if (*cursor == '}') {
            depth--;
            if (depth == 0) {
                return cursor;
            }
        }
        cursor++;
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

static void reportAetherRewriteError(const char *path,
                                     int line,
                                     const char *kind,
                                     const char *detail,
                                     const char *hint) {
    const char *code = aetherInferDiagnosticCode(kind, detail);

    if (code) {
        fprintf(stderr,
                "%s:%d: [%s] Aether %s rewrite error: %s\n",
                path ? path : "<aether>",
                line > 0 ? line : 1,
                code,
                kind ? kind : "rewrite",
                detail ? detail : "unknown rewrite error.");
    } else {
        fprintf(stderr,
                "%s:%d: Aether %s rewrite error: %s\n",
                path ? path : "<aether>",
                line > 0 ? line : 1,
                kind ? kind : "rewrite",
                detail ? detail : "unknown rewrite error.");
    }
    if (hint && *hint) {
        fprintf(stderr, "hint: %s\n", hint);
    }
}

static void reportAetherCompatibilityWarning(const char *path,
                                             int line,
                                             const char *detail) {
    const char *code = aetherInferDiagnosticCode("compatibility", detail);

    if (!aetherGetVerboseCompatibilityDiagnostics()) {
        return;
    }
    if (code) {
        fprintf(stderr,
                "%s:%d: [%s] Aether compatibility warning: %s\n",
                path ? path : "<aether>",
                line > 0 ? line : 1,
                code,
                detail ? detail : "compatibility fallback applied.");
    } else {
        fprintf(stderr,
                "%s:%d: Aether compatibility warning: %s\n",
                path ? path : "<aether>",
                line > 0 ? line : 1,
                detail ? detail : "compatibility fallback applied.");
    }
}

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

static const char *findLineCommentStartInRange(const char *start, const char *end) {
    const char *cursor = start;
    int inString = 0;
    char quote = '\0';

    while (cursor + 1 < end) {
        if (inString) {
            if (*cursor == '\\' && cursor + 1 < end) {
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
        if (cursor[0] == '/' && cursor[1] == '/') {
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
    int line = 1;

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
                    reportAetherRewriteError(path,
                                             line,
                                             "TOON",
                                             "only whitespace or comments may follow 'toon:'.",
                                             NULL);
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
                reportAetherRewriteError(path,
                                         line,
                                         "TOON",
                                         "'toon:' must be followed by an indented TOON block.",
                                         NULL);
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

            while (cursor < blockEnd) {
                if (*cursor == '\n') {
                    line++;
                }
                cursor++;
            }
            continue;
        }

        if (*lineEnd == '\n') {
            line++;
        }
    }

    return out.data;
}

static int lineStartsInlineIfDecl(const char *lineStart,
                                  const char *lineEnd,
                                  const char **outExprStart) {
    const char *body = skipSpacesInRange(lineStart, lineEnd);
    const char *cursor;
    const char *equals;
    const char *exprStart;

    if (!body || body >= lineEnd) {
        return 0;
    }
    if (!(startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const"))) {
        return 0;
    }
    cursor = body + (startsWithWord(body, lineEnd, "const") ? 5 : 3);
    equals = findCharInRange(cursor, lineEnd, '=');
    if (!equals) {
        return 0;
    }
    exprStart = skipSpacesInRange(equals + 1, lineEnd);
    if (!startsWithWord(exprStart, lineEnd, "if")) {
        return 0;
    }
    if (findCharInRange(exprStart, lineEnd, ';')) {
        return 0;
    }
    if (outExprStart) {
        *outExprStart = exprStart;
    }
    return 1;
}

static char *preprocessInlineIfDecls(const char *source, const char *path) {
    const char *cursor = source;
    Buffer out = {0};

    (void)path;
    if (!source) {
        return NULL;
    }

    while (*cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *exprStart = NULL;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }

        if (!lineStartsInlineIfDecl(lineStart, lineEnd, &exprStart)) {
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
            const char *scan = exprStart;
            const char *blockEnd = lineEnd;
            int depth = 0;
            int inString = 0;
            char quote = '\0';
            int foundEnd = 0;
            Buffer collapsed = {0};

            while (*scan) {
                char ch = *scan;
                if (inString) {
                    if (ch == '\\' && scan[1] != '\0') {
                        scan += 2;
                        continue;
                    }
                    if (ch == quote) {
                        inString = 0;
                        quote = '\0';
                    }
                    scan++;
                    continue;
                }
                if (ch == '"' || ch == '\'') {
                    inString = 1;
                    quote = ch;
                    scan++;
                    continue;
                }
                if (ch == '{') {
                    depth++;
                } else if (ch == '}') {
                    if (depth > 0) {
                        depth--;
                    }
                    if (depth == 0) {
                        const char *tail = skipSpaces(scan + 1);
                        if (*tail == ';') {
                            const char *tailEnd = tail + 1;
                            while (*tailEnd && *tailEnd != '\n') {
                                tailEnd++;
                            }
                            blockEnd = tailEnd;
                            foundEnd = 1;
                            break;
                        }
                    }
                }
                scan++;
            }

            if (!foundEnd) {
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

            if (!bufferAppendN(&collapsed, lineStart, (size_t)(exprStart - lineStart))) {
                free(collapsed.data);
                free(out.data);
                return NULL;
            }

            {
                const char *partCursor = exprStart;
                while (partCursor < blockEnd) {
                    const char *partLineEnd = partCursor;
                    const char *trimmedStart;
                    const char *trimmedEnd;
                    const char *nextCursor;

                    while (partLineEnd < blockEnd && *partLineEnd != '\n') {
                        partLineEnd++;
                    }
                    trimmedStart = skipSpacesInRange(partCursor, partLineEnd);
                    trimmedEnd = partLineEnd;
                    while (trimmedEnd > trimmedStart && isspace((unsigned char)trimmedEnd[-1])) {
                        trimmedEnd--;
                    }
                    if (trimmedEnd > trimmedStart &&
                        trimmedEnd[-1] == ';' &&
                        !(trimmedEnd - trimmedStart >= 2 && trimmedEnd[-2] == '}')) {
                        trimmedEnd--;
                        while (trimmedEnd > trimmedStart && isspace((unsigned char)trimmedEnd[-1])) {
                            trimmedEnd--;
                        }
                    }
                    if (trimmedEnd > trimmedStart) {
                        if (collapsed.len > 0 &&
                            !isspace((unsigned char)collapsed.data[collapsed.len - 1]) &&
                            !bufferAppend(&collapsed, " ")) {
                            free(collapsed.data);
                            free(out.data);
                            return NULL;
                        }
                        if (!bufferAppendN(&collapsed, trimmedStart, (size_t)(trimmedEnd - trimmedStart))) {
                            free(collapsed.data);
                            free(out.data);
                            return NULL;
                        }
                    }
                    nextCursor = partLineEnd < blockEnd && *partLineEnd == '\n'
                                     ? partLineEnd + 1
                                     : partLineEnd;
                    partCursor = nextCursor;
                }
            }

            if (!bufferAppend(&out, collapsed.data ? collapsed.data : "")) {
                free(collapsed.data);
                free(out.data);
                return NULL;
            }
            free(collapsed.data);

            {
                const char *nlCursor = lineEnd;
                while (nlCursor < blockEnd) {
                    if (*nlCursor == '\n' && !bufferAppendN(&out, "\n", 1)) {
                        free(out.data);
                        return NULL;
                    }
                    nlCursor++;
                }
            }

            while (cursor < blockEnd) {
                cursor++;
            }
            continue;
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
    int isMethod;
    int sawValueReturn;
    int sawFallthroughTopLevelStmt;
    size_t bindingCountBefore;
    char *name;
    char *postExpr;
    char *tupleTypeName;
    char **tupleItemTypes;
    size_t tupleItemCount;
} FunctionContracts;

typedef struct ParBlockState {
    int active;
    int bodyDepth;
    int nextHandle;
    char *indent;
    Buffer joinLines;
} ParBlockState;

struct TypeBlockState {
    int active;
    int bodyDepth;
    char *name;
    char **fieldNames;
    size_t fieldCount;
    size_t fieldCap;
};

typedef struct ObjectInitState {
    int active;
    int bodyDepth;
    char *targetName;
} ObjectInitState;

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

typedef struct AetherFieldSig {
    char *typeName;
    char *fieldName;
    char *fieldType;
} AetherFieldSig;

typedef struct AetherFieldTable {
    AetherFieldSig *items;
    size_t count;
    size_t cap;
} AetherFieldTable;

typedef struct ToonLiteralBinding {
    char *name;
    char *literal;
} ToonLiteralBinding;

typedef struct ToonLiteralTable {
    ToonLiteralBinding *items;
    size_t count;
    size_t cap;
} ToonLiteralTable;

typedef struct AetherTupleSig {
    char *functionName;
    char *tupleTypeName;
    char **itemTypes;
    size_t itemCount;
} AetherTupleSig;

typedef struct AetherTupleTable {
    AetherTupleSig *items;
    size_t count;
    size_t cap;
} AetherTupleTable;

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
    size_t i;

    if (!state) {
        return;
    }
    free(state->name);
    free(state->postExpr);
    free(state->tupleTypeName);
    for (i = 0; i < state->tupleItemCount; i++) {
        free(state->tupleItemTypes[i]);
    }
    free(state->tupleItemTypes);
    state->active = 0;
    state->bodyDepth = 0;
    state->isVoid = 0;
    state->isMethod = 0;
    state->sawValueReturn = 0;
    state->sawFallthroughTopLevelStmt = 0;
    state->bindingCountBefore = 0;
    state->name = NULL;
    state->postExpr = NULL;
    state->tupleTypeName = NULL;
    state->tupleItemTypes = NULL;
    state->tupleItemCount = 0;
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
    size_t i;

    if (!state) {
        return;
    }
    free(state->name);
    for (i = 0; i < state->fieldCount; i++) {
        free(state->fieldNames[i]);
    }
    free(state->fieldNames);
    state->active = 0;
    state->bodyDepth = 0;
    state->name = NULL;
    state->fieldNames = NULL;
    state->fieldCount = 0;
    state->fieldCap = 0;
}

static void clearObjectInitState(ObjectInitState *state) {
    if (!state) {
        return;
    }
    free(state->targetName);
    state->active = 0;
    state->bodyDepth = 0;
    state->targetName = NULL;
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

static void restoreAetherBindingTableCount(AetherBindingTable *table, size_t keepCount) {
    if (!table) {
        return;
    }
    while (table->count > keepCount) {
        size_t idx = table->count - 1;
        free(table->items[idx].name);
        free(table->items[idx].typeName);
        table->items[idx].name = NULL;
        table->items[idx].typeName = NULL;
        table->count--;
    }
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

static void freeAetherFieldTable(AetherFieldTable *table) {
    size_t i;

    if (!table) {
        return;
    }
    for (i = 0; i < table->count; i++) {
        free(table->items[i].typeName);
        free(table->items[i].fieldName);
        free(table->items[i].fieldType);
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

static int ensureAetherFieldTable(AetherFieldTable *table, size_t extra) {
    AetherFieldSig *resized;
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
    resized = (AetherFieldSig *)realloc(table->items, newCap * sizeof(AetherFieldSig));
    if (!resized) {
        return 0;
    }
    table->items = resized;
    table->cap = newCap;
    return 1;
}

static int ensureAetherTupleTable(AetherTupleTable *table, size_t extra) {
    AetherTupleSig *resized;
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
    resized = (AetherTupleSig *)realloc(table->items, newCap * sizeof(AetherTupleSig));
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

static int setAetherFieldType(AetherFieldTable *table,
                              const char *typeName,
                              const char *fieldName,
                              const char *fieldType) {
    size_t i;
    char *typeCopy;
    char *nameCopy;
    char *fieldTypeCopy;

    if (!table || !typeName || !fieldName || !fieldType) {
        return 0;
    }
    for (i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].typeName, typeName) == 0 &&
            strcmp(table->items[i].fieldName, fieldName) == 0) {
            fieldTypeCopy = dupCString(fieldType);
            if (!fieldTypeCopy) {
                return 0;
            }
            free(table->items[i].fieldType);
            table->items[i].fieldType = fieldTypeCopy;
            return 1;
        }
    }
    if (!ensureAetherFieldTable(table, 1)) {
        return 0;
    }
    typeCopy = dupCString(typeName);
    nameCopy = dupCString(fieldName);
    fieldTypeCopy = dupCString(fieldType);
    if (!typeCopy || !nameCopy || !fieldTypeCopy) {
        free(typeCopy);
        free(nameCopy);
        free(fieldTypeCopy);
        return 0;
    }
    table->items[table->count].typeName = typeCopy;
    table->items[table->count].fieldName = nameCopy;
    table->items[table->count].fieldType = fieldTypeCopy;
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

static const char *findAetherFieldType(const AetherFieldTable *table,
                                       const char *typeName,
                                       const char *fieldName,
                                       size_t fieldLen) {
    size_t i;

    if (!table || !typeName || !fieldName) {
        return NULL;
    }
    for (i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].typeName, typeName) == 0 &&
            strlen(table->items[i].fieldName) == fieldLen &&
            strncmp(table->items[i].fieldName, fieldName, fieldLen) == 0) {
            return table->items[i].fieldType;
        }
    }
    return NULL;
}

static int setAetherTupleSig(AetherTupleTable *table,
                             const char *functionName,
                             const char *tupleTypeName,
                             char **itemTypes,
                             size_t itemCount) {
    size_t i;
    char *fnCopy = NULL;
    char *typeCopy = NULL;
    char **itemCopies = NULL;

    if (!table || !functionName || !tupleTypeName || !itemTypes || itemCount == 0) {
        return 0;
    }
    for (i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].functionName, functionName) == 0) {
            typeCopy = dupCString(tupleTypeName);
            if (!typeCopy) {
                return 0;
            }
            itemCopies = (char **)calloc(itemCount, sizeof(char *));
            if (!itemCopies) {
                free(typeCopy);
                return 0;
            }
            for (size_t j = 0; j < itemCount; j++) {
                itemCopies[j] = dupCString(itemTypes[j]);
                if (!itemCopies[j]) {
                    free(typeCopy);
                    freeTupleItemTypes(itemCopies, itemCount);
                    return 0;
                }
            }
            free(table->items[i].tupleTypeName);
            freeTupleItemTypes(table->items[i].itemTypes, table->items[i].itemCount);
            table->items[i].tupleTypeName = typeCopy;
            table->items[i].itemTypes = itemCopies;
            table->items[i].itemCount = itemCount;
            return 1;
        }
    }
    if (!ensureAetherTupleTable(table, 1)) {
        return 0;
    }
    fnCopy = dupCString(functionName);
    typeCopy = dupCString(tupleTypeName);
    itemCopies = (char **)calloc(itemCount, sizeof(char *));
    if (!fnCopy || !typeCopy || !itemCopies) {
        free(fnCopy);
        free(typeCopy);
        free(itemCopies);
        return 0;
    }
    for (i = 0; i < itemCount; i++) {
        itemCopies[i] = dupCString(itemTypes[i]);
        if (!itemCopies[i]) {
            free(fnCopy);
            free(typeCopy);
            freeTupleItemTypes(itemCopies, itemCount);
            return 0;
        }
    }
    table->items[table->count].functionName = fnCopy;
    table->items[table->count].tupleTypeName = typeCopy;
    table->items[table->count].itemTypes = itemCopies;
    table->items[table->count].itemCount = itemCount;
    table->count++;
    return 1;
}

static const AetherTupleSig *findAetherTupleSig(const AetherTupleTable *table,
                                                const char *name,
                                                size_t len) {
    size_t i;

    if (!table || !name) {
        return NULL;
    }
    for (i = 0; i < table->count; i++) {
        if (strlen(table->items[i].functionName) == len &&
            strncmp(table->items[i].functionName, name, len) == 0) {
            return &table->items[i];
        }
    }
    return NULL;
}

static int ensureTypeFieldCapacity(TypeBlockState *state, size_t extra) {
    char **resized;
    size_t need;
    size_t newCap;

    if (!state) {
        return 0;
    }
    need = state->fieldCount + extra;
    if (need <= state->fieldCap) {
        return 1;
    }
    newCap = state->fieldCap ? state->fieldCap * 2 : 8;
    while (newCap < need) {
        newCap *= 2;
    }
    resized = (char **)realloc(state->fieldNames, newCap * sizeof(char *));
    if (!resized) {
        return 0;
    }
    state->fieldNames = resized;
    state->fieldCap = newCap;
    return 1;
}

static int typeBlockHasField(const TypeBlockState *state, const char *name, size_t len) {
    size_t i;

    if (!state || !name) {
        return 0;
    }
    for (i = 0; i < state->fieldCount; i++) {
        if (strlen(state->fieldNames[i]) == len &&
            strncmp(state->fieldNames[i], name, len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int recordTypeFieldName(TypeBlockState *state, const char *name, size_t len) {
    char *copy;

    if (!state || !name || len == 0) {
        return 0;
    }
    if (typeBlockHasField(state, name, len)) {
        return 1;
    }
    if (!ensureTypeFieldCapacity(state, 1)) {
        return 0;
    }
    copy = dupRange(name, name + len);
    if (!copy) {
        return 0;
    }
    state->fieldNames[state->fieldCount++] = copy;
    return 1;
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

static void freeTupleItemTypes(char **items, size_t count) {
    size_t i;

    if (!items) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

static void freeAetherTupleTable(AetherTupleTable *table) {
    size_t i;

    if (!table) {
        return;
    }
    for (i = 0; i < table->count; i++) {
        free(table->items[i].functionName);
        free(table->items[i].tupleTypeName);
        freeTupleItemTypes(table->items[i].itemTypes, table->items[i].itemCount);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
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
        (nameLen == 7 && strncmp(nameStart, "ai_chat", 7) == 0) ||
        (nameLen == 13 && strncmp(nameStart, "builtins_json", 13) == 0) ||
        (nameLen == 12 && strncmp(nameStart, "builtin_info", 12) == 0)) {
        return "Text";
    }
    if ((nameLen == 8 && strncmp(nameStart, "toon_len", 8) == 0) ||
        (nameLen == 10 && strncmp(nameStart, "string_len", 10) == 0) ||
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

static char *inferNewObjectTypeName(const char *start, const char *end) {
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
    if ((size_t)(end - start) < 4 || strncmp(start, "new ", 4) != 0) {
        return NULL;
    }
    nameStart = skipSpacesInRange(start + 3, end);
    if (nameStart >= end || !(isalpha((unsigned char)*nameStart) || *nameStart == '_')) {
        return NULL;
    }
    nameEnd = nameStart + 1;
    while (nameEnd < end && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
        nameEnd++;
    }
    cursor = skipSpacesInRange(nameEnd, end);
    if (cursor < end && *cursor == '(' && end[-1] == ')') {
        return trimmedCopy(nameStart, nameEnd);
    }
    return NULL;
}

static char *composeQualifiedLookup(const char *left,
                                    size_t leftLen,
                                    const char *right,
                                    size_t rightLen) {
    char *qualified;

    if (!left || !right || leftLen == 0 || rightLen == 0) {
        return NULL;
    }
    qualified = (char *)malloc(leftLen + 1 + rightLen + 1);
    if (!qualified) {
        return NULL;
    }
    memcpy(qualified, left, leftLen);
    qualified[leftLen] = '.';
    memcpy(qualified + leftLen + 1, right, rightLen);
    qualified[leftLen + 1 + rightLen] = '\0';
    return qualified;
}

static int isAetherNumericTypeName(const char *typeName) {
    if (!typeName) {
        return 0;
    }
    return strcmp(typeName, "Int") == 0 || strcmp(typeName, "Real") == 0;
}

static int isLikelyUnaryOperatorSite(const char *start, const char *op) {
    const char *cursor;

    if (!start || !op || op <= start) {
        return 1;
    }
    cursor = op;
    while (cursor > start) {
        cursor--;
        if (isspace((unsigned char)*cursor)) {
            continue;
        }
        return *cursor == '(' || *cursor == '[' || *cursor == '{' ||
               *cursor == ',' || *cursor == ':' || *cursor == '=' ||
               *cursor == '+' || *cursor == '-' || *cursor == '*' ||
               *cursor == '/' || *cursor == '%' || *cursor == '!' ||
               *cursor == '<' || *cursor == '>' || *cursor == '&' ||
               *cursor == '|';
    }
    return 1;
}

static int isWrappedInOuterParens(const char *start, const char *end) {
    const char *cursor;
    int depth = 0;
    int inString = 0;
    char quote = '\0';

    if (!start || !end || end - start < 2 || *start != '(' || end[-1] != ')') {
        return 0;
    }
    cursor = start;
    while (cursor < end) {
        char ch = *cursor;

        if (inString) {
            if (ch == '\\' && cursor + 1 < end) {
                cursor += 2;
                continue;
            }
            if (ch == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            inString = 1;
            quote = ch;
            cursor++;
            continue;
        }
        if (ch == '(') {
            depth++;
        } else if (ch == ')') {
            depth--;
            if (depth == 0 && cursor + 1 < end) {
                return 0;
            }
        }
        cursor++;
    }
    return depth == 0;
}

static const char *findTopLevelArithmeticOp(const char *start,
                                            const char *end,
                                            const char *ops) {
    const char *cursor = start;
    const char *found = NULL;
    int parenDepth = 0;
    int braceDepth = 0;
    int bracketDepth = 0;
    int inString = 0;
    char quote = '\0';

    if (!start || !end || !ops) {
        return NULL;
    }
    while (cursor < end) {
        char ch = *cursor;

        if (inString) {
            if (ch == '\\' && cursor + 1 < end) {
                cursor += 2;
                continue;
            }
            if (ch == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            inString = 1;
            quote = ch;
            cursor++;
            continue;
        }
        if (ch == '(') {
            parenDepth++;
            cursor++;
            continue;
        }
        if (ch == ')') {
            parenDepth--;
            cursor++;
            continue;
        }
        if (ch == '{') {
            braceDepth++;
            cursor++;
            continue;
        }
        if (ch == '}') {
            braceDepth--;
            cursor++;
            continue;
        }
        if (ch == '[') {
            bracketDepth++;
            cursor++;
            continue;
        }
        if (ch == ']') {
            bracketDepth--;
            cursor++;
            continue;
        }
        if (parenDepth == 0 && braceDepth == 0 && bracketDepth == 0 &&
            strchr(ops, ch) != NULL &&
            !isLikelyUnaryOperatorSite(start, cursor)) {
            found = cursor;
        }
        cursor++;
    }
    return found;
}

static char *inferAetherBindingTypeName(const char *exprStart,
                                        const char *exprEnd,
                                        const AetherBindingTable *bindings,
                                        const AetherFunctionTable *functions,
                                        const AetherFieldTable *fields) {
    const char *trimmedStart = exprStart;
    const char *trimmedEnd = exprEnd;
    const char *nameEnd;
    const char *helperType;
    char *objectInitType;
    char *newObjectType;

    if (!exprStart || !exprEnd || exprEnd < exprStart) {
        return NULL;
    }
    {
        const char *commentStart = findLineCommentStartInRange(trimmedStart, trimmedEnd);
        if (commentStart) {
            trimmedEnd = commentStart;
        }
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
    while (isWrappedInOuterParens(trimmedStart, trimmedEnd)) {
        trimmedStart++;
        trimmedEnd--;
        while (trimmedStart < trimmedEnd && isspace((unsigned char)*trimmedStart)) {
            trimmedStart++;
        }
        while (trimmedEnd > trimmedStart && isspace((unsigned char)trimmedEnd[-1])) {
            trimmedEnd--;
        }
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
    newObjectType = inferNewObjectTypeName(trimmedStart, trimmedEnd);
    if (newObjectType) {
        return newObjectType;
    }
    {
        const char *op = findTopLevelArithmeticOp(trimmedStart, trimmedEnd, "+-");
        if (!op) {
            op = findTopLevelArithmeticOp(trimmedStart, trimmedEnd, "*");
        }
        if (op) {
            char *leftType = inferAetherBindingTypeName(trimmedStart, op, bindings, functions, fields);
            char *rightType = inferAetherBindingTypeName(op + 1, trimmedEnd, bindings, functions, fields);
            char *resultType = NULL;

            if (leftType && rightType &&
                isAetherNumericTypeName(leftType) &&
                isAetherNumericTypeName(rightType)) {
                if (*op == '+' || *op == '-' || *op == '*') {
                    resultType = dupCString((strcmp(leftType, "Real") == 0 ||
                                             strcmp(rightType, "Real") == 0)
                                                ? "Real"
                                                : "Int");
                }
            }
            free(leftType);
            free(rightType);
            if (resultType) {
                return resultType;
            }
        }
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
                {
                    const char *dot = findCharInRange(trimmedStart, callNameEnd, '.');
                    if (dot && bindings) {
                        const char *receiverType = findAetherBindingType(bindings,
                                                                         trimmedStart,
                                                                         (size_t)(dot - trimmedStart));
                        if (receiverType) {
                            char *qualified = composeQualifiedLookup(receiverType,
                                                                     strlen(receiverType),
                                                                     dot + 1,
                                                                     (size_t)(callNameEnd - (dot + 1)));
                            if (qualified) {
                                functionType = findAetherFunctionReturnType(functions,
                                                                            qualified,
                                                                            strlen(qualified));
                                free(qualified);
                                if (functionType) {
                                    return dupCString(functionType);
                                }
                            }
                        }
                    }
                }
            }
            if (functions && bindings && fields) {
                const char *dot = skipSpacesInRange(nameEnd, trimmedEnd);
                if (dot < trimmedEnd && *dot == '.') {
                    const char *fieldStart = skipSpacesInRange(dot + 1, trimmedEnd);
                    const char *fieldEnd = fieldStart;
                    const char *fieldTail;

                    while (fieldEnd < trimmedEnd &&
                           (isalnum((unsigned char)*fieldEnd) || *fieldEnd == '_')) {
                        fieldEnd++;
                    }
                    fieldTail = skipSpacesInRange(fieldEnd, trimmedEnd);
                    if (fieldEnd > fieldStart && fieldTail == trimmedEnd) {
                        const char *receiverType = findAetherBindingType(bindings,
                                                                         trimmedStart,
                                                                         (size_t)(nameEnd - trimmedStart));
                        if (receiverType) {
                            if ((size_t)(fieldEnd - fieldStart) == 3 &&
                                strncmp(fieldStart, "len", 3) == 0) {
                                if (strcmp(receiverType, "Text") == 0 ||
                                    strstr(receiverType, "[]") != NULL) {
                                    return dupCString("Int");
                                }
                            }
                            helperType = findAetherFieldType(fields,
                                                             receiverType,
                                                             fieldStart,
                                                             (size_t)(fieldEnd - fieldStart));
                            if (helperType) {
                                return dupCString(helperType);
                            }
                        }
                    }
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
                                         const AetherFunctionTable *functions,
                                         const AetherFieldTable *fields) {
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
                                                  functions,
                                                  fields);
        }
    }

    if (typeName) {
        setAetherBindingType(table, name, typeName);
    }
    free(name);
    free(typeName);
}

static int recordAetherFunctionParamBindings(AetherBindingTable *table,
                                             const char *body,
                                             const char *lineEnd) {
    const char *paramsOpen;
    const char *paramsClose;
    const char *cursor;

    if (!table || !body || !lineEnd || !startsWithWord(body, lineEnd, "fn")) {
        return 1;
    }
    paramsOpen = findCharInRange(body, lineEnd, '(');
    paramsClose = paramsOpen ? findMatchingCloseParen(paramsOpen, lineEnd) : NULL;
    if (!paramsOpen || !paramsClose || paramsClose <= paramsOpen + 1) {
        return 1;
    }
    cursor = paramsOpen + 1;
    while (cursor < paramsClose) {
        const char *segmentStart = skipSpacesInRange(cursor, paramsClose);
        const char *segmentEnd = segmentStart;
        const char *colon;
        const char *nameEnd;
        const char *typeStart;
        const char *typeEnd;
        char *paramName = NULL;
        char *paramType = NULL;

        while (segmentEnd < paramsClose && *segmentEnd != ',') {
            segmentEnd++;
        }
        while (segmentEnd > segmentStart && isspace((unsigned char)segmentEnd[-1])) {
            segmentEnd--;
        }
        colon = findCharInRange(segmentStart, segmentEnd, ':');
        if (colon) {
            nameEnd = colon;
            while (nameEnd > segmentStart && isspace((unsigned char)nameEnd[-1])) {
                nameEnd--;
            }
            typeStart = skipSpacesInRange(colon + 1, segmentEnd);
            typeEnd = segmentEnd;
            if (nameEnd > segmentStart && typeEnd > typeStart) {
                paramName = trimmedCopy(segmentStart, nameEnd);
                paramType = trimmedCopy(typeStart, typeEnd);
                if ((!paramName || !paramType) ||
                    !setAetherBindingType(table, paramName, paramType)) {
                    free(paramName);
                    free(paramType);
                    return 0;
                }
            }
        }
        free(paramName);
        free(paramType);
        cursor = segmentEnd < paramsClose ? segmentEnd + 1 : segmentEnd;
    }
    return 1;
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

static char *extractTypeNameFromLine(const char *body, const char *lineEnd) {
    const char *nameStart;
    const char *nameEnd;

    if (!body || !lineEnd || !startsWithWord(body, lineEnd, "type")) {
        return NULL;
    }
    nameStart = skipSpacesInRange(body + 4, lineEnd);
    nameEnd = nameStart;
    while (nameEnd < lineEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
        nameEnd++;
    }
    if (nameEnd == nameStart) {
        return NULL;
    }
    return trimmedCopy(nameStart, nameEnd);
}

static void maybeRecordAetherFunctionReturnType(AetherFunctionTable *table,
                                                const char *body,
                                                const char *lineEnd,
                                                const char *moduleName,
                                                const char *typeName) {
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
    char *receiverType = NULL;

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
    paramsClose = paramsOpen ? findMatchingCloseParen(paramsOpen, lineEnd) : NULL;
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
    if (typeName && *typeName &&
        snprintf(qualifiedName, sizeof(qualifiedName), "%s.%s", typeName, fnName) < (int)sizeof(qualifiedName)) {
        setAetherFunctionReturnType(table, qualifiedName, returnType);
    }
    receiverType = extractSelfReceiverTypeName(paramsOpen + 1, paramsClose);
    if (receiverType &&
        snprintf(qualifiedName, sizeof(qualifiedName), "%s.%s", receiverType, fnName) < (int)sizeof(qualifiedName)) {
        setAetherFunctionReturnType(table, qualifiedName, returnType);
    }
    free(receiverType);
    free(fnName);
    free(returnType);
}

static char *extractSelfReceiverTypeName(const char *paramsStart, const char *paramsEnd) {
    const char *cursor;
    const char *nameStart;
    const char *nameEnd;
    const char *colon;
    const char *typeStart;
    const char *typeEnd;
    int depth = 0;

    if (!paramsStart || !paramsEnd || paramsEnd <= paramsStart) {
        return NULL;
    }
    cursor = skipSpacesInRange(paramsStart, paramsEnd);
    nameStart = cursor;
    while (cursor < paramsEnd && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    nameEnd = cursor;
    cursor = skipSpacesInRange(cursor, paramsEnd);
    if (nameEnd == nameStart || cursor >= paramsEnd || *cursor != ':') {
        return NULL;
    }
    if (!(((size_t)(nameEnd - nameStart) == 4 && strncmp(nameStart, "self", 4) == 0) ||
          ((size_t)(nameEnd - nameStart) == 6 && strncmp(nameStart, "myself", 6) == 0) ||
          ((size_t)(nameEnd - nameStart) == 2 && strncmp(nameStart, "my", 2) == 0))) {
        return NULL;
    }
    colon = cursor;
    typeStart = skipSpacesInRange(colon + 1, paramsEnd);
    typeEnd = typeStart;
    while (typeEnd < paramsEnd) {
        char ch = *typeEnd;
        if (ch == '(' || ch == '[' || ch == '{' || ch == '<') {
            depth++;
        } else if ((ch == ')' || ch == ']' || ch == '}' || ch == '>') && depth > 0) {
            depth--;
        } else if (ch == ',' && depth == 0) {
            break;
        }
        typeEnd++;
    }
    while (typeEnd > typeStart && isspace((unsigned char)typeEnd[-1])) {
        typeEnd--;
    }
    return trimmedCopy(typeStart, typeEnd);
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
    AetherFieldTable localFields = {0};
    char *moduleName = NULL;
    char *currentTypeName = NULL;
    const char *cursor = source;
    int braceDepth = 0;

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

        if (startsWithWord(body, lineEnd, "type")) {
            const char *nameStart = skipSpacesInRange(body + 4, lineEnd);
            const char *nameEnd = nameStart;
            while (nameEnd < lineEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
                nameEnd++;
            }
            free(currentTypeName);
            currentTypeName = (nameEnd > nameStart) ? trimmedCopy(nameStart, nameEnd) : NULL;
        }

        if (startsWithWord(body, lineEnd, "export")) {
            const char *rest = skipSpacesInRange(body + 6, lineEnd);

            maybeRecordAetherFunctionReturnType(functions, rest, lineEnd, moduleName, currentTypeName);
            maybeRecordAetherBindingType(&local, rest, lineEnd, functions, &localFields);
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
            maybeRecordAetherFunctionReturnType(functions, body, lineEnd, moduleName, currentTypeName);
            maybeRecordAetherBindingType(&local, body, lineEnd, functions, &localFields);
        }

        braceDepth += braceDeltaForLine(body);
        if (currentTypeName && braceDepth <= 0) {
            free(currentTypeName);
            currentTypeName = NULL;
        }

        cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
    }

    free(currentTypeName);
    free(moduleName);
    freeAetherBindingTable(&local);
    freeAetherFieldTable(&localFields);
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
    if (nameLen == 13 && strncmp(nameStart, "builtins_json", nameLen) == 0) {
        return bufferAppend(out, "aetherbuiltinsjson");
    }
    if (nameLen == 12 && strncmp(nameStart, "builtin_info", nameLen) == 0) {
        return bufferAppend(out, "aetherbuiltininfo");
    }
    if (nameLen == 7 && strncmp(nameStart, "println", nameLen) == 0) {
        return bufferAppend(out, "writeln");
    }
    if (nameLen == 5 && strncmp(nameStart, "sleep", nameLen) == 0) {
        return bufferAppend(out, "delay");
    }
    if (nameLen == 5 && strncmp(nameStart, "print", nameLen) == 0) {
        return bufferAppend(out, "write");
    }
    if (nameLen == 10 && strncmp(nameStart, "string_len", nameLen) == 0) {
        return bufferAppend(out, "length");
    }
    if (nameLen == 3 && strncmp(nameStart, "len", nameLen) == 0) {
        return bufferAppend(out, "length");
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

static int appendAetherInlineCallAlias(Buffer *out,
                                       const char *nameStart,
                                       size_t nameLen,
                                       const char *openParen,
                                       const char *lineEnd,
                                       const char **outCursor) {
    const char *closeParen;
    const char *arg1Start;
    const char *arg1End;
    const char *arg2Start;
    const char *arg2End;
    const char *comma = NULL;
    const char *scan;
    int depth = 0;

    if (!out || !nameStart || !openParen || *openParen != '(' || !lineEnd || !outCursor) {
        return 0;
    }
    if (!(nameLen == 9 && strncmp(nameStart, "string_eq", nameLen) == 0)) {
        return 0;
    }
    closeParen = findMatchingCloseParen(openParen, lineEnd);
    if (!closeParen) {
        return 0;
    }
    arg1Start = skipSpacesInRange(openParen + 1, closeParen);
    if (arg1Start >= closeParen) {
        return 0;
    }
    scan = arg1Start;
    while (scan < closeParen) {
        char ch = *scan;

        if (ch == '"' || ch == '\'') {
            char quote = ch;
            scan++;
            while (scan < closeParen) {
                if (*scan == '\\' && scan + 1 < closeParen) {
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
        if (ch == '(' || ch == '[' || ch == '{' || ch == '<') {
            depth++;
        } else if ((ch == ')' || ch == ']' || ch == '}' || ch == '>') && depth > 0) {
            depth--;
        } else if (ch == ',' && depth == 0) {
            comma = scan;
            break;
        }
        scan++;
    }
    if (!comma) {
        return 0;
    }
    arg1End = comma;
    while (arg1End > arg1Start && isspace((unsigned char)arg1End[-1])) {
        arg1End--;
    }
    arg2Start = skipSpacesInRange(comma + 1, closeParen);
    arg2End = closeParen;
    while (arg2End > arg2Start && isspace((unsigned char)arg2End[-1])) {
        arg2End--;
    }
    if (arg1End <= arg1Start || arg2End <= arg2Start) {
        return 0;
    }
    if (!bufferAppend(out, "(") ||
        !bufferAppendN(out, arg1Start, (size_t)(arg1End - arg1Start)) ||
        !bufferAppend(out, " == ") ||
        !bufferAppendN(out, arg2Start, (size_t)(arg2End - arg2Start)) ||
        !bufferAppend(out, ")")) {
        return 0;
    }
    *outCursor = closeParen + 1;
    return 1;
}

static int appendAetherExtensionCallRewrite(Buffer *out,
                                            const char *nameStart,
                                            size_t nameLen,
                                            const char *openParen,
                                            const char *lineEnd,
                                            const AetherBindingTable *bindings,
                                            const AetherFunctionTable *functions,
                                            const AetherFieldTable *fields,
                                            const char **outCursor) {
    const char *closeParen;
    const char *argStart;
    const char *argEnd;
    const char *comma = NULL;
    const char *scan;
    int depth = 0;
    char *receiverType = NULL;
    char qualifiedName[512];
    const char *resolvedReturnType;

    if (!out || !nameStart || !openParen || *openParen != '(' || !lineEnd ||
        !bindings || !functions || !outCursor) {
        return 0;
    }
    closeParen = findMatchingCloseParen(openParen, lineEnd);
    if (!closeParen) {
        return 0;
    }
    argStart = skipSpacesInRange(openParen + 1, closeParen);
    if (argStart >= closeParen) {
        return 0;
    }

    scan = argStart;
    while (scan < closeParen) {
        char ch = *scan;

        if (ch == '"' || ch == '\'') {
            char quote = ch;
            scan++;
            while (scan < closeParen) {
                if (*scan == '\\' && scan + 1 < closeParen) {
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
        if (ch == '(' || ch == '[' || ch == '{' || ch == '<') {
            depth++;
        } else if ((ch == ')' || ch == ']' || ch == '}' || ch == '>') && depth > 0) {
            depth--;
        } else if (ch == ',' && depth == 0) {
            comma = scan;
            break;
        }
        scan++;
    }
    argEnd = comma ? comma : closeParen;
    while (argEnd > argStart && isspace((unsigned char)argEnd[-1])) {
        argEnd--;
    }
    receiverType = inferAetherBindingTypeName(argStart, argEnd, bindings, functions, fields);
    if (!receiverType) {
        return 0;
    }
    if (snprintf(qualifiedName, sizeof(qualifiedName), "%s.%.*s",
                 receiverType, (int)nameLen, nameStart) >= (int)sizeof(qualifiedName)) {
        free(receiverType);
        return 0;
    }
    resolvedReturnType = findAetherFunctionReturnType(functions,
                                                      qualifiedName,
                                                      strlen(qualifiedName));
    free(receiverType);
    if (!resolvedReturnType) {
        return 0;
    }

    if (!bufferAppendN(out, argStart, (size_t)(argEnd - argStart)) ||
        !bufferAppend(out, ".") ||
        !bufferAppendN(out, nameStart, nameLen) ||
        !bufferAppend(out, "(")) {
        return 0;
    }
    if (comma) {
        const char *restStart = skipSpacesInRange(comma + 1, closeParen);
        const char *restEnd = closeParen;

        while (restEnd > restStart && isspace((unsigned char)restEnd[-1])) {
            restEnd--;
        }
        if (restEnd > restStart && !bufferAppendN(out, restStart, (size_t)(restEnd - restStart))) {
            return 0;
        }
    }
    if (!bufferAppend(out, ")")) {
        return 0;
    }
    *outCursor = closeParen + 1;
    return 1;
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

static char *applyJsonAliasesToLine(const char *line,
                                    JsonAliasState *jsonState,
                                    const ToonLiteralTable *toonTable);

static int appendAliasedTrimmedRange(Buffer *out,
                                     const char *start,
                                     const char *end,
                                     JsonAliasState *jsonState,
                                     const ToonLiteralTable *toonTable) {
    char *segment;
    char *aliased;
    int ok;

    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    segment = dupRange(start, end);
    if (!segment) {
        return 0;
    }
    aliased = applyJsonAliasesToLine(segment, jsonState, toonTable);
    free(segment);
    if (!aliased) {
        return 0;
    }
    ok = bufferAppend(out, aliased);
    free(aliased);
    return ok;
}

static int isSingleCharDoubleQuotedLiteral(const char *start, const char *end) {
    size_t innerLen;

    if (!start || !end) {
        return 0;
    }
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    if ((size_t)(end - start) < 3 || *start != '"' || end[-1] != '"') {
        return 0;
    }
    innerLen = (size_t)(end - start - 2);
    if (innerLen == 1) {
        return 1;
    }
    if (innerLen == 2 && start[1] == '\\') {
        return 1;
    }
    return 0;
}

static int appendToonTextKeyArg(Buffer *out,
                                const char *start,
                                const char *end,
                                JsonAliasState *jsonState,
                                const ToonLiteralTable *toonTable) {
    if (isSingleCharDoubleQuotedLiteral(start, end)) {
        return bufferAppend(out, "(\"\" + ") &&
               appendAliasedTrimmedRange(out, start, end, jsonState, toonTable) &&
               bufferAppend(out, ")");
    }
    return appendAliasedTrimmedRange(out, start, end, jsonState, toonTable);
}

static int appendToonScalarAlias(Buffer *out,
                                 const char *nameStart,
                                 size_t nameLen,
                                 const char *openParen,
                                 const char **outCursor,
                                 JsonAliasState *jsonState,
                                 const ToonLiteralTable *toonTable) {
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
            !appendAliasedTrimmedRange(out, arg1Start, arg1End, jsonState, toonTable) ||
            !bufferAppend(out, ", ") ||
            !appendToonTextKeyArg(out, arg2Start, arg2End, jsonState, toonTable) ||
            !bufferAppend(out, ") ? ") ||
            !bufferAppend(out, getter) ||
            !bufferAppend(out, "(YyjsonGetKey(") ||
            !appendAliasedTrimmedRange(out, arg1Start, arg1End, jsonState, toonTable) ||
            !bufferAppend(out, ", ") ||
            !appendToonTextKeyArg(out, arg2Start, arg2End, jsonState, toonTable) ||
            !bufferAppend(out, ")) : ") ||
            !appendAliasedTrimmedRange(out, arg3Start, arg3End, jsonState, toonTable) ||
            !bufferAppend(out, ")")) {
            return 0;
        }
    } else {
        if (!bufferAppend(out, getter) ||
            !bufferAppend(out, "(YyjsonGetKey(") ||
            !appendAliasedTrimmedRange(out, arg1Start, arg1End, jsonState, toonTable) ||
            !bufferAppend(out, ", ") ||
            !appendToonTextKeyArg(out, arg2Start, arg2End, jsonState, toonTable) ||
            !bufferAppend(out, "))")) {
            return 0;
        }
    }
    *outCursor = cursor + 1;
    return 1;
}

static int appendToonQueryAlias(Buffer *out,
                                const char *nameStart,
                                size_t nameLen,
                                const char *openParen,
                                const char **outCursor,
                                JsonAliasState *jsonState,
                                const ToonLiteralTable *toonTable) {
    const char *replacement = NULL;
    const char *cursor;
    const char *arg1Start;
    const char *arg1End = NULL;
    const char *arg2Start = NULL;
    const char *arg2End = NULL;
    int depth = 0;
    int inString = 0;
    char quote = '\0';
    int keyHelper = 0;

    if (!out || !nameStart || !openParen || *openParen != '(' || !outCursor) {
        return 0;
    }
    if (nameLen == 8 && strncmp(nameStart, "toon_key", nameLen) == 0) {
        replacement = "YyjsonGetKey";
        keyHelper = 1;
    } else if (nameLen == 12 && strncmp(nameStart, "toon_has_key", nameLen) == 0) {
        replacement = "YyjsonHasKey";
        keyHelper = 1;
    } else if (nameLen == 7 && strncmp(nameStart, "toon_at", nameLen) == 0) {
        replacement = "YyjsonGetIndex";
    } else if (nameLen == 11 && strncmp(nameStart, "toon_has_at", nameLen) == 0) {
        replacement = "YyjsonHasIndex";
    } else {
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
                arg2End = cursor;
                break;
            }
            depth--;
        } else if (*cursor == ',' && depth == 0 && !arg1End) {
            arg1End = cursor;
            arg2Start = cursor + 1;
        }
        cursor++;
    }
    if (!arg1End || !arg2Start || !arg2End) {
        return 0;
    }

    jsonState->needed = 1;
    if (!bufferAppend(out, replacement) ||
        !bufferAppend(out, "(") ||
        !appendAliasedTrimmedRange(out, arg1Start, arg1End, jsonState, toonTable) ||
        !bufferAppend(out, ", ")) {
        return 0;
    }
    if (keyHelper) {
        if (!appendToonTextKeyArg(out, arg2Start, arg2End, jsonState, toonTable)) {
            return 0;
        }
    } else {
        if (!appendAliasedTrimmedRange(out, arg2Start, arg2End, jsonState, toonTable)) {
            return 0;
        }
    }
    if (!bufferAppend(out, ")")) {
        return 0;
    }
    *outCursor = cursor + 1;
    return 1;
}

static int appendToonInspectAlias(Buffer *out,
                                  const char *nameStart,
                                  size_t nameLen,
                                  const char *openParen,
                                  const char **outCursor,
                                  JsonAliasState *jsonState,
                                  const ToonLiteralTable *toonTable) {
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
                        !appendAliasedTrimmedRange(out, argStart, argEnd, jsonState, toonTable) ||
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
                    !appendAliasedTrimmedRange(out, argStart, argEnd, jsonState, toonTable) ||
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
                                      jsonState,
                                      toonTable)) {
                cursor = advancedCursor;
                continue;
            }
            if (*afterName == '(' &&
                appendToonQueryAlias(&out,
                                     nameStart,
                                     (size_t)(nameEnd - nameStart),
                                     afterName,
                                     &advancedCursor,
                                     jsonState,
                                     toonTable)) {
                cursor = advancedCursor;
                continue;
            }
            if (*afterName == '(' &&
                appendToonInspectAlias(&out,
                                       nameStart,
                                       (size_t)(nameEnd - nameStart),
                                       afterName,
                                       &advancedCursor,
                                       jsonState,
                                       toonTable)) {
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

static char *rewriteInlineIfExpression(const char *exprStart,
                                       const char *exprEnd,
                                       const AetherBindingTable *bindings,
                                       const TypeBlockState *typeState,
                                       int isMethod) {
    const char *cursor;
    const char *condStart;
    const char *condEnd;
    const char *thenOpen;
    const char *thenClose;
    const char *thenExprStart;
    const char *thenExprEnd;
    const char *elseKw;
    const char *elseOpen;
    const char *elseClose;
    const char *elseExprStart;
    const char *elseExprEnd;
    const char *rest;
    const char *scan;
    int depth = 0;
    char *rewrittenCond = NULL;
    char *rewrittenThen = NULL;
    char *rewrittenElse = NULL;
    Buffer out = {0};

    if (!exprStart || !exprEnd || exprEnd <= exprStart) {
        return NULL;
    }
    cursor = skipSpacesInRange(exprStart, exprEnd);
    if (!startsWithWord(cursor, exprEnd, "if")) {
        return NULL;
    }
    condStart = skipSpacesInRange(cursor + 2, exprEnd);
    scan = condStart;
    thenOpen = NULL;
    while (scan < exprEnd) {
        if (*scan == '"' || *scan == '\'') {
            char quote = *scan++;
            while (scan < exprEnd) {
                if (*scan == '\\' && scan + 1 < exprEnd) {
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
        if (*scan == '(' || *scan == '[') {
            depth++;
        } else if ((*scan == ')' || *scan == ']') && depth > 0) {
            depth--;
        } else if (*scan == '{' && depth == 0) {
            thenOpen = scan;
            break;
        }
        scan++;
    }
    if (!thenOpen) {
        return NULL;
    }
    condEnd = thenOpen;
    while (condEnd > condStart && isspace((unsigned char)condEnd[-1])) {
        condEnd--;
    }
    thenClose = findMatchingCloseBrace(thenOpen, exprEnd);
    if (!thenClose) {
        return NULL;
    }
    elseKw = skipSpacesInRange(thenClose + 1, exprEnd);
    if (!startsWithWord(elseKw, exprEnd, "else")) {
        return NULL;
    }
    elseOpen = skipSpacesInRange(elseKw + 4, exprEnd);
    if (elseOpen >= exprEnd || *elseOpen != '{') {
        return NULL;
    }
    elseClose = findMatchingCloseBrace(elseOpen, exprEnd);
    if (!elseClose) {
        return NULL;
    }
    rest = skipSpacesInRange(elseClose + 1, exprEnd);
    if (rest != exprEnd) {
        return NULL;
    }

    thenExprStart = skipSpacesInRange(thenOpen + 1, thenClose);
    thenExprEnd = thenClose;
    while (thenExprEnd > thenExprStart && isspace((unsigned char)thenExprEnd[-1])) {
        thenExprEnd--;
    }
    if (thenExprEnd > thenExprStart && thenExprEnd[-1] == ';') {
        thenExprEnd--;
    }
    while (thenExprEnd > thenExprStart && isspace((unsigned char)thenExprEnd[-1])) {
        thenExprEnd--;
    }

    elseExprStart = skipSpacesInRange(elseOpen + 1, elseClose);
    elseExprEnd = elseClose;
    while (elseExprEnd > elseExprStart && isspace((unsigned char)elseExprEnd[-1])) {
        elseExprEnd--;
    }
    if (elseExprEnd > elseExprStart && elseExprEnd[-1] == ';') {
        elseExprEnd--;
    }
    while (elseExprEnd > elseExprStart && isspace((unsigned char)elseExprEnd[-1])) {
        elseExprEnd--;
    }

    rewrittenCond = rewriteMethodScopedExpr(condStart, condEnd, bindings, typeState, isMethod);
    rewrittenThen = rewriteMethodScopedExpr(thenExprStart, thenExprEnd, bindings, typeState, isMethod);
    rewrittenElse = rewriteMethodScopedExpr(elseExprStart, elseExprEnd, bindings, typeState, isMethod);
    if (!rewrittenCond || !rewrittenThen || !rewrittenElse) {
        free(rewrittenCond);
        free(rewrittenThen);
        free(rewrittenElse);
        free(out.data);
        return NULL;
    }
    if (!bufferAppend(&out, "((") ||
        !bufferAppend(&out, rewrittenCond) ||
        !bufferAppend(&out, ") ? (") ||
        !bufferAppend(&out, rewrittenThen) ||
        !bufferAppend(&out, ") : (") ||
        !bufferAppend(&out, rewrittenElse) ||
        !bufferAppend(&out, "))")) {
        free(rewrittenCond);
        free(rewrittenThen);
        free(rewrittenElse);
        free(out.data);
        return NULL;
    }
    free(rewrittenCond);
    free(rewrittenThen);
    free(rewrittenElse);
    return out.data;
}

static void reportMisplacedContractBlock(const char *path,
                                         const char *cursor,
                                         int lineNumber) {
    const char *scan = cursor;
    int scanLine = lineNumber;

    while (scan && *scan) {
        const char *lineStart = scan;
        const char *lineEnd = scan;
        const char *body;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }

        body = lineStart;
        while (body < lineEnd && (*body == ' ' || *body == '\t')) {
            body++;
        }

        if (startsWithWord(body, lineEnd, "@pre")) {
            reportAetherRewriteError(path,
                                     scanLine,
                                     "contract",
                                     "@pre must annotate the next function declaration.",
                                     "move @pre above the `fn ...` line instead of placing it inside the function body.");
        } else if (startsWithWord(body, lineEnd, "@post")) {
            reportAetherRewriteError(path,
                                     scanLine,
                                     "contract",
                                     "@post must annotate the next function declaration.",
                                     "move @post above the `fn ...` line instead of placing it inside the function body.");
        } else if (body == lineEnd || isLineComment(body, lineEnd)) {
            /* Keep scanning through blank/comment lines so adjacent misplaced
             * contracts are all reported together. */
        } else {
            break;
        }

        if (!*lineEnd) {
            break;
        }
        scan = lineEnd + 1;
        scanLine++;
    }
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
    paramsClose = paramsOpen ? findMatchingCloseParen(paramsOpen, lineEnd) : NULL;
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

static int hasExplicitFunctionReturnType(const char *body, const char *lineEnd) {
    const char *nameStart = body + 2;
    const char *nameEnd;
    const char *paramsOpen;
    const char *paramsClose;

    if (!body || !lineEnd || !startsWithWord(body, lineEnd, "fn")) {
        return 0;
    }
    while (nameStart < lineEnd && isspace((unsigned char)*nameStart)) {
        nameStart++;
    }
    nameEnd = nameStart;
    while (nameEnd < lineEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
        nameEnd++;
    }
    paramsOpen = findCharInRange(nameEnd, lineEnd, '(');
    paramsClose = paramsOpen ? findMatchingCloseParen(paramsOpen, lineEnd) : NULL;
    if (!paramsOpen || !paramsClose) {
        return 0;
    }
    return findSubstringInRange(paramsClose, lineEnd, "->") != NULL;
}

static int typeFieldLineEndsWithComma(const char *body, const char *lineEnd) {
    const char *cursor = lineEnd;

    if (!body || !lineEnd) {
        return 0;
    }
    while (cursor > body && isspace((unsigned char)cursor[-1])) {
        cursor--;
    }
    return cursor > body && cursor[-1] == ',';
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

static char *rewriteMethodScopedExpr(const char *start,
                                     const char *end,
                                     const AetherBindingTable *bindings,
                                     const TypeBlockState *typeState,
                                     int isMethod) {
    const char *cursor = start;
    Buffer out = {0};

    if (!start || !end || end < start) {
        return NULL;
    }

    while (cursor < end) {
        if (*cursor == '"' || *cursor == '\'') {
            char quote = *cursor++;

            if (!bufferAppendN(&out, cursor - 1, 1)) {
                free(out.data);
                return NULL;
            }
            while (cursor < end) {
                if (!bufferAppendN(&out, cursor, 1)) {
                    free(out.data);
                    return NULL;
                }
                if (*cursor == '\\' && cursor + 1 < end) {
                    cursor++;
                    if (!bufferAppendN(&out, cursor, 1)) {
                        free(out.data);
                        return NULL;
                    }
                } else if (*cursor == quote) {
                    cursor++;
                    break;
                }
                cursor++;
            }
            continue;
        }
        if ((cursor == start || !isIdentifierChar((unsigned char)cursor[-1])) &&
            (isalpha((unsigned char)*cursor) || *cursor == '_')) {
            const char *nameStart = cursor;
            const char *nameEnd = cursor + 1;
            const char *afterName;
            const char *advancedCursor = NULL;

            while (nameEnd < end && isIdentifierChar((unsigned char)*nameEnd)) {
                nameEnd++;
            }
            afterName = skipSpacesInRange(nameEnd, end);
            if (afterName < end && *afterName == '(' &&
                appendAetherInlineCallAlias(&out,
                                            nameStart,
                                            (size_t)(nameEnd - nameStart),
                                            afterName,
                                            end,
                                            &advancedCursor)) {
                cursor = advancedCursor;
                continue;
            }
            if (isMethod &&
                (size_t)(nameEnd - nameStart) == 4 &&
                strncmp(nameStart, "self", 4) == 0) {
                if (!bufferAppend(&out, "myself")) {
                    free(out.data);
                    return NULL;
                }
                cursor = nameEnd;
                continue;
            }
            if (typeState &&
                (cursor == start || cursor[-1] != '.') &&
                !(afterName < end && *afterName == '(') &&
                !findAetherBindingType(bindings, nameStart, (size_t)(nameEnd - nameStart)) &&
                typeBlockHasField(typeState, nameStart, (size_t)(nameEnd - nameStart))) {
                if (!bufferAppend(&out, "myself.") ||
                    !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart))) {
                    free(out.data);
                    return NULL;
                }
                cursor = nameEnd;
                continue;
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

static char *rewriteTuplePostExpr(const char *start,
                                  const char *end,
                                  const FunctionContracts *fnState,
                                  char *detail,
                                  size_t detailSize) {
    const char *cursor = start;
    Buffer out = {0};

    if (detail && detailSize > 0) {
        detail[0] = '\0';
    }
    if (!start || !end || end < start || !fnState || !fnState->tupleTypeName) {
        return NULL;
    }

    while (cursor < end) {
        if (*cursor == '"' || *cursor == '\'') {
            char quote = *cursor++;

            if (!bufferAppendN(&out, cursor - 1, 1)) {
                free(out.data);
                return NULL;
            }
            while (cursor < end) {
                if (!bufferAppendN(&out, cursor, 1)) {
                    free(out.data);
                    return NULL;
                }
                if (*cursor == '\\' && cursor + 1 < end) {
                    cursor++;
                    if (!bufferAppendN(&out, cursor, 1)) {
                        free(out.data);
                        return NULL;
                    }
                } else if (*cursor == quote) {
                    cursor++;
                    break;
                }
                cursor++;
            }
            continue;
        }
        if ((cursor == start || !isIdentifierChar((unsigned char)cursor[-1])) &&
            (isalpha((unsigned char)*cursor) || *cursor == '_')) {
            const char *nameStart = cursor;
            const char *nameEnd = cursor + 1;

            while (nameEnd < end && isIdentifierChar((unsigned char)*nameEnd)) {
                nameEnd++;
            }
            if ((size_t)(nameEnd - nameStart) == 6 &&
                strncmp(nameStart, "result", 6) == 0) {
                const char *dot = skipSpacesInRange(nameEnd, end);

                if (dot < end && *dot == '.') {
                    const char *indexStart = skipSpacesInRange(dot + 1, end);
                    const char *indexEnd = indexStart;
                    unsigned long tupleIndex = 0;
                    char fieldName[64];

                    while (indexEnd < end && isdigit((unsigned char)*indexEnd)) {
                        indexEnd++;
                    }
                    if (indexEnd == indexStart) {
                        if (detail && detailSize > 0) {
                            snprintf(detail,
                                     detailSize,
                                     "tuple @post access must use `result.0`, `result.1`, and so on.");
                        }
                        free(out.data);
                        return NULL;
                    }
                    tupleIndex = strtoul(indexStart, NULL, 10);
                    if (tupleIndex >= fnState->tupleItemCount) {
                        if (detail && detailSize > 0) {
                            snprintf(detail,
                                     detailSize,
                                     "tuple @post index %lu is out of range; this function returns %zu values.",
                                     tupleIndex,
                                     fnState->tupleItemCount);
                        }
                        free(out.data);
                        return NULL;
                    }
                    if (indexEnd < end && isIdentifierChar((unsigned char)*indexEnd)) {
                        if (detail && detailSize > 0) {
                            snprintf(detail,
                                     detailSize,
                                     "tuple @post access must use a numeric slot like `result.0`.");
                        }
                        free(out.data);
                        return NULL;
                    }
                    snprintf(fieldName,
                             sizeof(fieldName),
                             "%s_item%lu",
                             fnState->tupleTypeName,
                             tupleIndex);
                    if (!bufferAppend(&out, fieldName)) {
                        free(out.data);
                        return NULL;
                    }
                    cursor = indexEnd;
                    continue;
                }

                if (detail && detailSize > 0) {
                    snprintf(detail,
                             detailSize,
                             "tuple-return @post checks must reference slots explicitly, for example `result.0` or `result.1`.");
                }
                free(out.data);
                return NULL;
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

static int isAetherOpaqueHandleTypeName(const char *typeName) {
    return typeName &&
           (strcmp(typeName, "ToonDoc") == 0 || strcmp(typeName, "ToonNode") == 0);
}

static const char *trimRangeStart(const char *start, const char *end) {
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    return start;
}

static const char *trimRangeEnd(const char *start, const char *end) {
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    return end;
}

static int rangeEqualsWord(const char *start, const char *end, const char *word) {
    size_t len;

    if (!start || !end || !word) {
        return 0;
    }
    start = trimRangeStart(start, end);
    end = trimRangeEnd(start, end);
    len = strlen(word);
    return (size_t)(end - start) == len && strncmp(start, word, len) == 0;
}

static const char *findOpaqueNilOperandStart(const char *lineStart, const char *cursor) {
    const char *scan = cursor;
    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;

    while (scan > lineStart) {
        char ch = scan[-1];

        if (ch == ')') {
            parenDepth++;
            scan--;
            continue;
        }
        if (ch == ']') {
            bracketDepth++;
            scan--;
            continue;
        }
        if (ch == '}') {
            braceDepth++;
            scan--;
            continue;
        }
        if (ch == '(') {
            if (parenDepth == 0) {
                break;
            }
            parenDepth--;
            scan--;
            continue;
        }
        if (ch == '[') {
            if (bracketDepth == 0) {
                break;
            }
            bracketDepth--;
            scan--;
            continue;
        }
        if (ch == '{') {
            if (braceDepth == 0) {
                break;
            }
            braceDepth--;
            scan--;
            continue;
        }
        if (parenDepth == 0 && bracketDepth == 0 && braceDepth == 0) {
            if (ch == ';' || ch == ',' || ch == '{' || ch == '}' || ch == ':') {
                break;
            }
            if ((ch == '|' || ch == '&') && scan - 2 >= lineStart && scan[-2] == ch) {
                break;
            }
            if ((ch == '=' || ch == '!') && scan - 2 >= lineStart && scan[-2] == '=') {
                break;
            }
        }
        scan--;
    }
    return scan;
}

static const char *findOpaqueNilOperandEnd(const char *cursor, const char *lineEnd) {
    const char *scan = cursor;
    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;

    while (scan < lineEnd) {
        char ch = *scan;

        if (ch == '(') {
            parenDepth++;
            scan++;
            continue;
        }
        if (ch == '[') {
            bracketDepth++;
            scan++;
            continue;
        }
        if (ch == '{') {
            braceDepth++;
            scan++;
            continue;
        }
        if (ch == ')') {
            if (parenDepth == 0) {
                break;
            }
            parenDepth--;
            scan++;
            continue;
        }
        if (ch == ']') {
            if (bracketDepth == 0) {
                break;
            }
            bracketDepth--;
            scan++;
            continue;
        }
        if (ch == '}') {
            if (braceDepth == 0) {
                break;
            }
            braceDepth--;
            scan++;
            continue;
        }
        if (parenDepth == 0 && bracketDepth == 0 && braceDepth == 0) {
            if (ch == ';' || ch == ',' || ch == '{' || ch == '}' || ch == ':') {
                break;
            }
            if ((ch == '|' || ch == '&') && scan + 1 < lineEnd && scan[1] == ch) {
                break;
            }
            if ((ch == '=' || ch == '!') && scan + 1 < lineEnd && scan[1] == '=') {
                break;
            }
        }
        scan++;
    }
    return scan;
}

static char *rewriteAetherOpaqueNilComparisons(const char *start,
                                               const char *end,
                                               const AetherBindingTable *bindings,
                                               const AetherFunctionTable *functions,
                                               const AetherFieldTable *fields) {
    const char *cursor = start;
    const char *lastCopied = start;
    Buffer out = {0};
    int changed = 0;

    if (!start || !end || end < start) {
        return NULL;
    }
    while (cursor < end) {
        if (cursor + 1 < end && cursor[0] == '/' && cursor[1] == '/') {
            break;
        }
        if (*cursor == '"' || *cursor == '\'') {
            char quote = *cursor++;
            while (cursor < end) {
                if (*cursor == '\\' && cursor + 1 < end) {
                    cursor += 2;
                    continue;
                }
                if (*cursor == quote) {
                    cursor++;
                    break;
                }
                cursor++;
            }
            continue;
        }
        if (cursor + 1 < end &&
            ((cursor[0] == '=' && cursor[1] == '=') ||
             (cursor[0] == '!' && cursor[1] == '='))) {
            const char *lhsStart = findOpaqueNilOperandStart(start, cursor);
            const char *lhsEnd = cursor;
            const char *rhsStart = cursor + 2;
            const char *rhsEnd = findOpaqueNilOperandEnd(rhsStart, end);
            const char *trimmedLhsStart = trimRangeStart(lhsStart, lhsEnd);
            const char *trimmedLhsEnd = trimRangeEnd(trimmedLhsStart, lhsEnd);
            const char *trimmedRhsStart = trimRangeStart(rhsStart, rhsEnd);
            const char *trimmedRhsEnd = trimRangeEnd(trimmedRhsStart, rhsEnd);
            int lhsIsNil = rangeEqualsWord(trimmedLhsStart, trimmedLhsEnd, "nil");
            int rhsIsNil = rangeEqualsWord(trimmedRhsStart, trimmedRhsEnd, "nil");
            char *opaqueType = NULL;
            const char *replaceStart = NULL;
            const char *replaceEnd = NULL;

            if (lhsIsNil ^ rhsIsNil) {
                const char *opaqueStart = lhsIsNil ? trimmedRhsStart : trimmedLhsStart;
                const char *opaqueEnd = lhsIsNil ? trimmedRhsEnd : trimmedLhsEnd;

                opaqueType = inferAetherBindingTypeName(opaqueStart,
                                                        opaqueEnd,
                                                        bindings,
                                                        functions,
                                                        fields);
                if (isAetherOpaqueHandleTypeName(opaqueType)) {
                    replaceStart = lhsIsNil ? trimmedLhsStart : trimmedRhsStart;
                    replaceEnd = lhsIsNil ? trimmedLhsEnd : trimmedRhsEnd;
                }
            }

            if (replaceStart && replaceEnd) {
                if (!bufferAppendN(&out, lastCopied, (size_t)(replaceStart - lastCopied)) ||
                    !bufferAppend(&out, "-1")) {
                    free(opaqueType);
                    free(out.data);
                    return NULL;
                }
                changed = 1;
                lastCopied = replaceEnd;
            }
            free(opaqueType);
            cursor += 2;
            continue;
        }
        cursor++;
    }

    if (!changed) {
        free(out.data);
        return dupRange(start, end);
    }
    if (!bufferAppendN(&out, lastCopied, (size_t)(end - lastCopied))) {
        free(out.data);
        return NULL;
    }
    return out.data;
}

static char *rewriteAetherLenPropertyExpr(const char *start,
                                          const char *end,
                                          const AetherBindingTable *bindings,
                                          const AetherFunctionTable *functions,
                                          const AetherFieldTable *fields) {
    const char *cursor = start;
    Buffer out = {0};

    if (!start || !end || end < start) {
        return NULL;
    }
    if (start == end) {
        return dupCString("");
    }

    while (cursor < end) {
        if (cursor + 1 < end && cursor[0] == '/' && cursor[1] == '/') {
            if (!bufferAppendN(&out, cursor, (size_t)(end - cursor))) {
                free(out.data);
                return NULL;
            }
            return out.data;
        }
        if (*cursor == '"' || *cursor == '\'') {
            char quote = *cursor++;

            if (!bufferAppendN(&out, cursor - 1, 1)) {
                free(out.data);
                return NULL;
            }
            while (cursor < end) {
                if (!bufferAppendN(&out, cursor, 1)) {
                    free(out.data);
                    return NULL;
                }
                if (*cursor == '\\' && cursor + 1 < end) {
                    cursor++;
                    if (!bufferAppendN(&out, cursor, 1)) {
                        free(out.data);
                        return NULL;
                    }
                } else if (*cursor == quote) {
                    cursor++;
                    break;
                }
                cursor++;
            }
            continue;
        }
        if ((cursor == start || !isIdentifierChar((unsigned char)cursor[-1])) &&
            (isalpha((unsigned char)*cursor) || *cursor == '_')) {
            const char *chainEnd = cursor + 1;
            const char *lastDot = NULL;
            const char *lastSegmentStart = cursor;
            const char *lastSegmentEnd = chainEnd;
            const char *tail;
            char *receiverType = NULL;
            const char *helperName = NULL;

            while (chainEnd < end && isIdentifierChar((unsigned char)*chainEnd)) {
                chainEnd++;
            }
            lastSegmentEnd = chainEnd;

            while (1) {
                const char *dot = skipSpacesInRange(chainEnd, end);
                const char *segmentStart;
                const char *segmentEnd;

                if (dot >= end || *dot != '.') {
                    break;
                }
                segmentStart = skipSpacesInRange(dot + 1, end);
                if (segmentStart >= end ||
                    !(isalpha((unsigned char)*segmentStart) || *segmentStart == '_')) {
                    break;
                }
                segmentEnd = segmentStart + 1;
                while (segmentEnd < end && isIdentifierChar((unsigned char)*segmentEnd)) {
                    segmentEnd++;
                }
                lastDot = dot;
                lastSegmentStart = segmentStart;
                lastSegmentEnd = segmentEnd;
                chainEnd = segmentEnd;
            }

            tail = skipSpacesInRange(chainEnd, end);
            if (lastDot &&
                lastSegmentEnd > lastSegmentStart &&
                (size_t)(lastSegmentEnd - lastSegmentStart) == 3 &&
                strncmp(lastSegmentStart, "len", 3) == 0 &&
                (tail >= end || *tail != '(')) {
                receiverType = inferAetherBindingTypeName(cursor,
                                                          lastDot,
                                                          bindings,
                                                          functions,
                                                          fields);
                if (receiverType) {
                    if (strcmp(receiverType, "Text") == 0) {
                        helperName = "string_len";
                    } else if (strstr(receiverType, "[]") != NULL) {
                        helperName = "length";
                    }
                }
                if (helperName) {
                    if (!bufferAppend(&out, helperName) ||
                        !bufferAppend(&out, "(") ||
                        !bufferAppendN(&out, cursor, (size_t)(lastDot - cursor)) ||
                        !bufferAppend(&out, ")")) {
                        free(receiverType);
                        free(out.data);
                        return NULL;
                    }
                    free(receiverType);
                    cursor = chainEnd;
                    continue;
                }
                free(receiverType);
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

static int appendContractGuard(Buffer *out,
                               const char *indent,
                               const char *fnName,
                               const char *kind,
                               const char *expr,
                               JsonAliasState *jsonState,
                               const ToonLiteralTable *toonTable) {
    char *aliasedExpr = NULL;

    if (!out || !indent || !fnName || !kind || !expr) {
        return 0;
    }
    aliasedExpr = applyJsonAliasesToLine(expr, jsonState, toonTable);
    if (!aliasedExpr) {
        return 0;
    }
    if (!bufferAppend(out, indent) ||
        !bufferAppend(out, "if (!(") ||
        !bufferAppend(out, aliasedExpr) ||
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
        free(aliasedExpr);
        return 0;
    }
    free(aliasedExpr);
    return 1;
}

static int isStandaloneCloseBrace(const char *body, const char *lineEnd) {
    const char *tail = lineEnd;

    while (tail > body && isspace((unsigned char)tail[-1])) {
        tail--;
    }
    return tail == body + 1 && body < tail && *body == '}';
}

static int isStandaloneCloseBraceWithOptionalSemicolon(const char *body, const char *lineEnd) {
    const char *tail = lineEnd;

    while (tail > body && isspace((unsigned char)tail[-1])) {
        tail--;
    }
    if (tail > body && tail[-1] == ';') {
        tail--;
    }
    while (tail > body && isspace((unsigned char)tail[-1])) {
        tail--;
    }
    return tail == body + 1 && body < tail && *body == '}';
}

static char *translateObjectInitFieldLine(const char *lineStart,
                                          const char *body,
                                          const char *lineEnd,
                                          const char *targetName) {
    const char *segmentEnd = lineEnd;
    const char *fieldEnd = NULL;
    const char *fieldNameEnd;
    const char *valueStart;
    const char *valueEnd;
    const char *cursor = body;
    int depth = 0;
    Buffer out = {0};

    if (!targetName || !*targetName) {
        return dupRange(lineStart, lineEnd);
    }
    while (cursor < lineEnd) {
        char ch = *cursor;
        if (ch == '"' || ch == '\'') {
            char quote = ch;
            cursor++;
            while (cursor < lineEnd) {
                if (*cursor == '\\' && cursor + 1 < lineEnd) {
                    cursor += 2;
                    continue;
                }
                if (*cursor == quote) {
                    cursor++;
                    break;
                }
                cursor++;
            }
            continue;
        }
        if (ch == '(' || ch == '[' || ch == '{') {
            depth++;
        } else if ((ch == ')' || ch == ']' || ch == '}') && depth > 0) {
            depth--;
        } else if (ch == ':' && depth == 0) {
            fieldEnd = cursor;
            break;
        }
        cursor++;
    }
    if (!fieldEnd) {
        return dupRange(lineStart, lineEnd);
    }
    fieldNameEnd = fieldEnd;
    while (fieldNameEnd > body && isspace((unsigned char)fieldNameEnd[-1])) {
        fieldNameEnd--;
    }
    valueStart = skipSpaces(fieldEnd + 1);
    while (segmentEnd > valueStart && isspace((unsigned char)segmentEnd[-1])) {
        segmentEnd--;
    }
    if (segmentEnd > valueStart && segmentEnd[-1] == ',') {
        segmentEnd--;
    }
    valueEnd = segmentEnd;
    while (valueEnd > valueStart && isspace((unsigned char)valueEnd[-1])) {
        valueEnd--;
    }
    if (fieldNameEnd == body || valueEnd == valueStart) {
        return dupRange(lineStart, lineEnd);
    }

    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppend(&out, targetName) ||
        !bufferAppend(&out, ".") ||
        !bufferAppendN(&out, body, (size_t)(fieldNameEnd - body)) ||
        !bufferAppend(&out, " = ") ||
        !bufferAppendN(&out, valueStart, (size_t)(valueEnd - valueStart)) ||
        !bufferAppend(&out, ";")) {
        free(out.data);
        return NULL;
    }
    return out.data;
}

static char *translateMultiLineTypedObjectInitOpenLine(const char *lineStart,
                                                       const char *body,
                                                       const char *lineEnd,
                                                       ObjectInitState *state) {
    const char *cursor = body + 3;
    const char *nameStart;
    const char *nameEnd;
    const char *colon;
    const char *afterColon;
    const char *typeEnd;
    const char *equals;
    const char *exprStart;
    const char *initNameEnd;
    const char *openBrace;
    const char *closeBrace;
    const char *typeNameStart;
    const char *typeNameEnd;
    Buffer out = {0};

    if (!startsWithWord(body, lineEnd, "let")) {
        return NULL;
    }
    cursor = skipSpaces(cursor);
    nameStart = cursor;
    while (cursor < lineEnd && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    nameEnd = cursor;
    cursor = skipSpaces(cursor);
    if (cursor >= lineEnd || *cursor != ':') {
        return NULL;
    }
    colon = cursor;
    afterColon = skipSpaces(colon + 1);
    typeEnd = afterColon;
    while (typeEnd < lineEnd && *typeEnd != '=' && *typeEnd != ';') {
        typeEnd++;
    }
    while (typeEnd > afterColon && isspace((unsigned char)typeEnd[-1])) {
        typeEnd--;
    }
    equals = skipSpaces(typeEnd);
    if (equals >= lineEnd || *equals != '=') {
        return NULL;
    }
    exprStart = skipSpaces(equals + 1);
    initNameEnd = exprStart;
    while (initNameEnd < lineEnd && (isalnum((unsigned char)*initNameEnd) || *initNameEnd == '_')) {
        initNameEnd++;
    }
    openBrace = skipSpaces(initNameEnd);
    if (!(openBrace < lineEnd && *openBrace == '{')) {
        return NULL;
    }
    closeBrace = findLastCharInRange(openBrace + 1, lineEnd, '}');
    if (closeBrace) {
        return NULL;
    }
    typeNameStart = afterColon;
    typeNameEnd = typeEnd;
    while (typeNameStart < typeNameEnd && isspace((unsigned char)*typeNameStart)) {
        typeNameStart++;
    }
    while (typeNameEnd > typeNameStart && isspace((unsigned char)typeNameEnd[-1])) {
        typeNameEnd--;
    }
    if ((size_t)(typeNameEnd - typeNameStart) != (size_t)(initNameEnd - exprStart) ||
        strncmp(typeNameStart, exprStart, (size_t)(typeNameEnd - typeNameStart)) != 0) {
        return NULL;
    }
    if (!state) {
        return NULL;
    }
    clearObjectInitState(state);
    state->targetName = dupRange(nameStart, nameEnd);
    if (!state->targetName) {
        return NULL;
    }
    if (!appendMappedParamTypeAndName(&out,
                                      afterColon,
                                      typeEnd,
                                      nameStart,
                                      nameEnd) ||
        !bufferAppend(&out, " = new ") ||
        !bufferAppendN(&out, exprStart, (size_t)(initNameEnd - exprStart)) ||
        !bufferAppend(&out, "();")) {
        clearObjectInitState(state);
        free(out.data);
        return NULL;
    }
    return out.data;
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
                                     const FunctionContracts *fnState,
                                     const AetherBindingTable *bindings,
                                     const TypeBlockState *typeState,
                                     JsonAliasState *jsonState,
                                     const ToonLiteralTable *toonTable) {
    const char *cursor = body + 3;
    const char *exprStart;
    const char *exprEnd;
    Buffer out = {0};
    char *indent = NULL;
    char *rewrittenExpr = NULL;

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
        rewrittenExpr = rewriteMethodScopedExpr(exprStart,
                                                exprEnd,
                                                bindings,
                                                typeState,
                                                fnState->isMethod);
        if (!rewrittenExpr) {
            free(indent);
            free(out.data);
            return NULL;
        }
        if (!bufferAppend(&out, indent) ||
            !bufferAppend(&out, "result = ") ||
            !bufferAppend(&out, rewrittenExpr) ||
            !bufferAppend(&out, ";\n")) {
            free(rewrittenExpr);
            free(indent);
            free(out.data);
            return NULL;
        }
    }
    if (!appendContractGuard(&out,
                             indent,
                             fnState->name,
                             "post",
                             fnState->postExpr,
                             jsonState,
                             toonTable)) {
        free(indent);
        free(out.data);
        return NULL;
    }
    if (!bufferAppend(&out, indent) ||
        !bufferAppend(&out, exprStart < exprEnd ? "return result;" : "return;")) {
        free(rewrittenExpr);
        free(indent);
        free(out.data);
        return NULL;
    }
    free(rewrittenExpr);
    free(indent);
    return out.data;
}

static int isSelfLikeParamNameRange(const char *start, const char *end) {
    size_t len;

    if (!start || !end || end <= start) {
        return 0;
    }
    len = (size_t)(end - start);
    return (len == 4 && strncmp(start, "self", 4) == 0) ||
           (len == 2 && strncmp(start, "my", 2) == 0) ||
           (len == 6 && strncmp(start, "myself", 6) == 0);
}

static int translateParamListEx(Buffer *out,
                                const char *start,
                                const char *end,
                                int skipSelfLikeFirstParam) {
    const char *cursor = start;
    int first = 1;
    int paramIndex = 0;

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
            int shouldSkip = 0;

            if (skipSelfLikeFirstParam && paramIndex == 0 && colon) {
                const char *nameEnd = colon;

                while (nameEnd > segmentStart && isspace((unsigned char)nameEnd[-1])) {
                    nameEnd--;
                }
                shouldSkip = isSelfLikeParamNameRange(segmentStart, nameEnd);
            }

            if (!shouldSkip) {
                if (!first && !bufferAppend(out, ", ")) {
                    return 0;
                }
                if (colon) {
                    if (!appendMappedParamTypeAndName(out,
                                                      colon + 1,
                                                      segmentEnd,
                                                      segmentStart,
                                                      colon)) {
                        return 0;
                    }
                } else {
                    if (!bufferAppendN(out, segmentStart, (size_t)(segmentEnd - segmentStart))) {
                        return 0;
                    }
                }
                first = 0;
            }
            paramIndex++;
        }

        cursor = segmentEnd;
        if (cursor < end && *cursor == ',') {
            cursor++;
        }
        cursor = skipSpaces(cursor);
    }
    return 1;
}

static int translateParamList(Buffer *out, const char *start, const char *end) {
    return translateParamListEx(out, start, end, 0);
}

static int functionHasSelfLikeReceiverParam(const char *body, const char *lineEnd) {
    const char *nameStart = body + 2;
    const char *nameEnd;
    const char *paramsOpen;
    const char *paramsClose;
    const char *cursor;
    const char *colon;

    while (nameStart < lineEnd && isspace((unsigned char)*nameStart)) {
        nameStart++;
    }
    nameEnd = nameStart;
    while (nameEnd < lineEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
        nameEnd++;
    }
    paramsOpen = findCharInRange(nameEnd, lineEnd, '(');
    paramsClose = paramsOpen ? findMatchingCloseParen(paramsOpen, lineEnd) : NULL;
    if (!paramsOpen || !paramsClose) {
        return 0;
    }
    cursor = skipSpacesInRange(paramsOpen + 1, paramsClose);
    if (cursor >= paramsClose) {
        return 0;
    }
    colon = findCharInRange(cursor, paramsClose, ':');
    if (!colon) {
        return 0;
    }
    while (colon > cursor && isspace((unsigned char)colon[-1])) {
        colon--;
    }
    if ((size_t)(colon - cursor) == 4 && strncmp(cursor, "self", 4) == 0) {
        return 1;
    }
    if ((size_t)(colon - cursor) == 2 && strncmp(cursor, "my", 2) == 0) {
        return 1;
    }
    if ((size_t)(colon - cursor) == 6 && strncmp(cursor, "myself", 6) == 0) {
        return 1;
    }
    return 0;
}

static char *translateFnLine(const char *lineStart,
                             const char *body,
                             const char *lineEnd,
                             const TypeBlockState *typeState) {
    const char *nameStart = body + 2;
    const char *nameEnd;
    const char *paramsOpen;
    const char *paramsClose;
    const char *arrow;
    const char *typeStart;
    const char *typeEnd;
    const char *baseTypeEnd;
    const char *typeSuffixStart;
    Buffer out = {0};

    while (*nameStart && isspace((unsigned char)*nameStart)) {
        nameStart++;
    }
    nameEnd = nameStart;
    while (*nameEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
        nameEnd++;
    }
    paramsOpen = findCharInRange(nameEnd, lineEnd, '(');
    paramsClose = paramsOpen ? findMatchingCloseParen(paramsOpen, lineEnd) : NULL;
    arrow = paramsClose ? findSubstringInRange(paramsClose, lineEnd, "->") : NULL;
    if (!paramsOpen || !paramsClose || !arrow) {
        return NULL;
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
    splitTypeSuffix(typeStart, typeEnd, &baseTypeEnd, &typeSuffixStart);

    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !appendMappedType(&out, typeStart, baseTypeEnd) ||
        !bufferAppend(&out, " ") ||
        !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart)) ||
        !bufferAppend(&out, "(") ||
        !translateParamListEx(&out,
                              paramsOpen + 1,
                              paramsClose,
                              typeState && typeState->active) ||
        !bufferAppend(&out, ")")) {
        free(out.data);
        return NULL;
    }

    if (typeSuffixStart < typeEnd) {
        const char *suffix = typeSuffixStart;
        while (suffix < typeEnd) {
            if (!isspace((unsigned char)*suffix) &&
                !bufferAppendN(&out, suffix, 1)) {
                free(out.data);
                return NULL;
            }
            suffix++;
        }
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

static char *translateFnForwardDeclLine(const char *lineStart,
                                        const char *body,
                                        const char *lineEnd,
                                        const TypeBlockState *typeState) {
    char *translated;
    char *brace;
    Buffer out = {0};

    translated = translateFnLine(lineStart, body, lineEnd, typeState);
    if (!translated) {
        return NULL;
    }
    brace = strchr(translated, '{');
    if (!brace) {
        free(translated);
        return NULL;
    }
    while (brace > translated && isspace((unsigned char)brace[-1])) {
        brace--;
    }
    if (!bufferAppendN(&out, translated, (size_t)(brace - translated)) ||
        !bufferAppend(&out, ";")) {
        free(translated);
        free(out.data);
        return NULL;
    }
    free(translated);
    return out.data;
}

static int appendTopLevelForwardDeclarations(Buffer *out,
                                             const char *source,
                                             int *outputLineNumber,
                                             AetherTupleTable *tupleTable,
                                             int *nextTupleTypeId) {
    const char *cursor = source;
    int lineNumber = 1;
    int emittedAny = 0;

    if (!out || !source || !outputLineNumber || !tupleTable || !nextTupleTypeId) {
        return 0;
    }

    while (*cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *body;
        size_t indentWidth;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }
        body = skipSpacesInRange(lineStart, lineEnd);
        indentWidth = leadingIndentWidth(lineStart, lineEnd);

        if (indentWidth == 0 &&
            startsWithWord(body, lineEnd, "fn") &&
            !functionHasSelfLikeReceiverParam(body, lineEnd)) {
            char *fnName = NULL;
            char *returnType = NULL;
            char **tupleItemTypes = NULL;
            size_t tupleItemCount = 0;
            int hasTupleReturn = 0;
            char *forwardDecl = translateFnForwardDeclLine(lineStart, body, lineEnd, NULL);

            if (!extractFunctionSignature(body, lineEnd, &fnName, &returnType)) {
                free(forwardDecl);
                forwardDecl = NULL;
            } else {
                hasTupleReturn = parseTupleTypeList(returnType,
                                                   returnType + strlen(returnType),
                                                   &tupleItemTypes,
                                                   &tupleItemCount);
            }

            if (hasTupleReturn && fnName) {
                char tupleTypeName[64];

                (*nextTupleTypeId)++;
                snprintf(tupleTypeName, sizeof(tupleTypeName), "__aether_tuple_%d", *nextTupleTypeId);
                if (!setAetherTupleSig(tupleTable,
                                       fnName,
                                       tupleTypeName,
                                       tupleItemTypes,
                                       tupleItemCount)) {
                    free(fnName);
                    free(returnType);
                    freeTupleItemTypes(tupleItemTypes, tupleItemCount);
                    free(forwardDecl);
                    return 0;
                }
            }

            if (!hasTupleReturn && forwardDecl) {
                if (!bufferAppend(out, forwardDecl) ||
                    !trackRewriteOutputLines(forwardDecl, outputLineNumber, lineNumber)) {
                    free(fnName);
                    free(returnType);
                    freeTupleItemTypes(tupleItemTypes, tupleItemCount);
                    free(forwardDecl);
                    return 0;
                }
                if (!bufferAppend(out, "\n")) {
                    free(fnName);
                    free(returnType);
                    freeTupleItemTypes(tupleItemTypes, tupleItemCount);
                    free(forwardDecl);
                    return 0;
                }
                (*outputLineNumber)++;
                emittedAny = 1;
            }
            free(fnName);
            free(returnType);
            freeTupleItemTypes(tupleItemTypes, tupleItemCount);
            free(forwardDecl);
        }

        cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
        if (*lineEnd == '\n') {
            lineNumber++;
        }
    }

    if (emittedAny && !bufferAppend(out, "\n")) {
        return 0;
    }
    if (emittedAny) {
        (*outputLineNumber)++;
    }
    return 1;
}

static char *translateTupleFnLine(const char *lineStart,
                                  const char *body,
                                  const char *lineEnd,
                                  const char *tupleTypeName,
                                  char **itemTypes,
                                  size_t itemCount) {
    const char *nameStart = body + 2;
    const char *nameEnd;
    const char *paramsOpen;
    const char *paramsClose;
    const char *brace;
    Buffer out = {0};
    size_t i;

    if (!tupleTypeName || !itemTypes || itemCount == 0) {
        return NULL;
    }
    while (nameStart < lineEnd && isspace((unsigned char)*nameStart)) {
        nameStart++;
    }
    nameEnd = nameStart;
    while (nameEnd < lineEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
        nameEnd++;
    }
    paramsOpen = findCharInRange(nameEnd, lineEnd, '(');
    paramsClose = paramsOpen ? findMatchingCloseParen(paramsOpen, lineEnd) : NULL;
    brace = findLastCharInRange(paramsClose ? paramsClose : body, lineEnd, '{');
    if (!paramsOpen || !paramsClose || !brace) {
        return dupRange(lineStart, lineEnd);
    }

    for (i = 0; i < itemCount; i++) {
        char fieldName[64];

        snprintf(fieldName, sizeof(fieldName), "%s_item%zu", tupleTypeName, i);
        if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
            !appendMappedTypeAndCStringName(&out, itemTypes[i], fieldName) ||
            !bufferAppend(&out, ";\n")) {
            free(out.data);
            return NULL;
        }
    }
    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppend(&out, "void ") ||
        !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart)) ||
        !bufferAppend(&out, "(") ||
        !translateParamList(&out, paramsOpen + 1, paramsClose) ||
        !bufferAppend(&out, ")") ||
        !bufferAppendN(&out, brace, (size_t)(lineEnd - brace))) {
        free(out.data);
        return NULL;
    }
    return out.data;
}

static char *translateTupleReturnLine(const char *lineStart,
                                      const char *body,
                                      const char *lineEnd,
                                      const FunctionContracts *fnState,
                                      JsonAliasState *jsonState,
                                      const ToonLiteralTable *toonTable) {
    const char *cursor = skipSpaces(body + 3);
    const char *exprEnd = lineEnd;
    char **items = NULL;
    size_t itemCount = 0;
    Buffer out = {0};
    char *indent = NULL;
    (void)jsonState;
    (void)toonTable;

    if (!fnState || !fnState->tupleTypeName) {
        return dupRange(lineStart, lineEnd);
    }
    while (exprEnd > cursor && isspace((unsigned char)exprEnd[-1])) {
        exprEnd--;
    }
    if (exprEnd > cursor && exprEnd[-1] == ';') {
        exprEnd--;
    }
    while (exprEnd > cursor && isspace((unsigned char)exprEnd[-1])) {
        exprEnd--;
    }
    if (!parseTupleTypeList(cursor, exprEnd, &items, &itemCount) ||
        itemCount != fnState->tupleItemCount) {
        freeTupleItemTypes(items, itemCount);
        return dupRange(lineStart, lineEnd);
    }

    indent = dupRange(lineStart, body);
    if (!indent) {
        freeTupleItemTypes(items, itemCount);
        return NULL;
    }
    for (size_t i = 0; i < itemCount; i++) {
        char fieldName[64];

        snprintf(fieldName, sizeof(fieldName), "%s_item%zu", fnState->tupleTypeName, i);
        if (!bufferAppend(&out, indent) ||
            !bufferAppend(&out, fieldName) ||
            !bufferAppend(&out, " = ")) {
            free(indent);
            freeTupleItemTypes(items, itemCount);
            free(out.data);
            return NULL;
        }
        if (!bufferAppend(&out, items[i]) ||
            !bufferAppend(&out, ";\n")) {
            free(indent);
            freeTupleItemTypes(items, itemCount);
            free(out.data);
            return NULL;
        }
    }
    if (fnState->postExpr &&
        !appendContractGuard(&out,
                             indent,
                             fnState->name,
                             "post",
                             fnState->postExpr,
                             jsonState,
                             toonTable)) {
        free(indent);
        freeTupleItemTypes(items, itemCount);
        free(out.data);
        return NULL;
    }
    if (!bufferAppend(&out, indent) ||
        !bufferAppend(&out, "return;")) {
        free(indent);
        freeTupleItemTypes(items, itemCount);
        free(out.data);
        return NULL;
    }
    free(indent);
    freeTupleItemTypes(items, itemCount);
    return out.data;
}

static int extractDirectTupleCallName(const char *exprStart,
                                      const char *exprEnd,
                                      const char **outNameStart,
                                      size_t *outNameLen) {
    const char *cursor = exprStart;
    const char *nameStart;
    const char *nameEnd;
    const char *afterName;

    if (!exprStart || !exprEnd || !outNameStart || !outNameLen) {
        return 0;
    }
    while (cursor < exprEnd && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (cursor >= exprEnd || !(isalpha((unsigned char)*cursor) || *cursor == '_')) {
        return 0;
    }
    nameStart = cursor;
    while (cursor < exprEnd && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    nameEnd = cursor;
    afterName = skipSpacesInRange(nameEnd, exprEnd);
    if (afterName >= exprEnd || *afterName != '(') {
        return 0;
    }
    *outNameStart = nameStart;
    *outNameLen = (size_t)(nameEnd - nameStart);
    return 1;
}

static char *translateTupleDestructureLetLine(const char *lineStart,
                                              const char *body,
                                              const char *lineEnd,
                                              const AetherTupleTable *tupleTable,
                                              const char *path,
                                              int lineNumber) {
    const char *cursor = skipSpaces(body + 3);
    const char *patternEnd;
    const char *equals;
    const char *equalsToken;
    const char *exprStart;
    const char *exprEnd = lineEnd;
    const char *callNameStart = NULL;
    size_t callNameLen = 0;
    char **names = NULL;
    size_t nameCount = 0;
    const AetherTupleSig *tupleSig = NULL;
    Buffer out = {0};

    if (!tupleTable || cursor >= lineEnd || *cursor != '(') {
        return NULL;
    }
    equalsToken = findCharInRange(cursor, lineEnd, '=');
    patternEnd = equalsToken ? findMatchingCloseParen(cursor, equalsToken) : NULL;
    equals = equalsToken ? equalsToken : NULL;
    if (!patternEnd || !equals || equals >= lineEnd || *equals != '=') {
        return NULL;
    }
    exprStart = skipSpacesInRange(equals + 1, lineEnd);
    while (exprEnd > exprStart && isspace((unsigned char)exprEnd[-1])) {
        exprEnd--;
    }
    if (exprEnd > exprStart && exprEnd[-1] == ';') {
        exprEnd--;
    }
    while (exprEnd > exprStart && isspace((unsigned char)exprEnd[-1])) {
        exprEnd--;
    }
    if (!splitTopLevelCommaList(cursor + 1, patternEnd, &names, &nameCount)) {
        return NULL;
    }
    if (!extractDirectTupleCallName(exprStart, exprEnd, &callNameStart, &callNameLen)) {
        reportAetherRewriteError(path,
                                 lineNumber,
                                 "feature",
                                 "tuple destructuring currently requires a direct call to a known tuple-return function.",
                                 "use `let tmp = fnCall();` and then read fields, or destructure a direct tuple-return call.");
        freeTupleItemTypes(names, nameCount);
        return NULL;
    }
    tupleSig = findAetherTupleSig(tupleTable, callNameStart, callNameLen);
    if (!tupleSig) {
        reportAetherRewriteError(path,
                                 lineNumber,
                                 "feature",
                                 "tuple destructuring target is not a known tuple-return function.",
                                 "destructure only direct calls to tuple-return Aether functions in the current module.");
        freeTupleItemTypes(names, nameCount);
        return NULL;
    }
    if (tupleSig->itemCount != nameCount) {
        reportAetherRewriteError(path,
                                 lineNumber,
                                 "feature",
                                 "tuple destructuring arity does not match the function return tuple.",
                                 "make the number of bindings match the number of returned tuple elements.");
        freeTupleItemTypes(names, nameCount);
        return NULL;
    }

    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppendN(&out, exprStart, (size_t)(exprEnd - exprStart)) ||
        !bufferAppend(&out, ";\n")) {
        freeTupleItemTypes(names, nameCount);
        free(out.data);
        return NULL;
    }
    for (size_t i = 0; i < nameCount; i++) {
        char fieldName[64];

        snprintf(fieldName, sizeof(fieldName), "%s_item%zu", tupleSig->tupleTypeName, i);
        if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
            !appendMappedTypeAndCStringName(&out, tupleSig->itemTypes[i], names[i]) ||
            !bufferAppend(&out, " = ") ||
            !bufferAppend(&out, fieldName) ||
            !bufferAppend(&out, ";\n")) {
            freeTupleItemTypes(names, nameCount);
            free(out.data);
            return NULL;
        }
    }
    if (out.len > 0 && out.data[out.len - 1] == '\n') {
        out.data[out.len - 1] = '\0';
        out.len--;
    }
    freeTupleItemTypes(names, nameCount);
    return out.data;
}

static int findTupleInitializerCallee(const char *body,
                                      const char *lineEnd,
                                      const AetherTupleTable *tupleTable) {
    const char *cursor;
    const char *equals;
    const char *exprStart;
    const char *exprEnd;
    const char *callNameStart = NULL;
    size_t callNameLen = 0;

    if (!body || !lineEnd || !tupleTable || !startsWithWord(body, lineEnd, "let")) {
        return 0;
    }
    cursor = skipSpacesInRange(body + 3, lineEnd);
    while (cursor < lineEnd && *cursor != '=') {
        cursor++;
    }
    if (cursor >= lineEnd || *cursor != '=') {
        return 0;
    }
    equals = cursor;
    exprStart = skipSpacesInRange(equals + 1, lineEnd);
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
    if (!extractDirectTupleCallName(exprStart, exprEnd, &callNameStart, &callNameLen)) {
        return 0;
    }
    return findAetherTupleSig(tupleTable, callNameStart, callNameLen) != NULL;
}

static char *translateInlineObjectInitMethodDeclLine(const char *lineStart,
                                                     const char *indentStart,
                                                     const char *nameStart,
                                                     const char *nameEnd,
                                                     const char *exprStart,
                                                     const char *exprEnd,
                                                     const char *declTypeNameStart,
                                                     const char *declTypeNameEnd,
                                                     const AetherFunctionTable *functions,
                                                     int lineNumber) {
    const char *typeNameStart = exprStart;
    const char *typeNameEnd = exprStart;
    const char *openBrace;
    const char *closeBrace = NULL;
    const char *methodDot;
    const char *methodNameStart;
    const char *methodNameEnd;
    const char *methodArgsStart;
    const char *methodArgsEnd = NULL;
    const char *cursor;
    const char *returnTypeName = NULL;
    char *qualifiedMethodName = NULL;
    char *inferredReturnType = NULL;
    char *declTypeName = NULL;
    char tempName[64];
    Buffer out = {0};
    int depth = 0;
    int inString = 0;
    char quote = '\0';

    if (!lineStart || !indentStart || !nameStart || !nameEnd || !exprStart || !exprEnd) {
        return NULL;
    }
    {
        const char *commentStart = findLineCommentStartInRange(exprStart, exprEnd);
        if (commentStart) {
            exprEnd = commentStart;
        }
    }
    while (exprEnd > exprStart && isspace((unsigned char)exprEnd[-1])) {
        exprEnd--;
    }
    if (exprEnd > exprStart && exprEnd[-1] == ';') {
        exprEnd--;
    }
    while (exprEnd > exprStart && isspace((unsigned char)exprEnd[-1])) {
        exprEnd--;
    }
    if (declTypeNameStart && declTypeNameEnd && declTypeNameEnd > declTypeNameStart) {
        declTypeName = trimmedCopy(declTypeNameStart, declTypeNameEnd);
        if (!declTypeName) {
            return NULL;
        }
        returnTypeName = declTypeName;
    }
    while (typeNameEnd < exprEnd && (isalnum((unsigned char)*typeNameEnd) || *typeNameEnd == '_')) {
        typeNameEnd++;
    }
    if (typeNameEnd == typeNameStart) {
        return NULL;
    }
    openBrace = skipSpacesInRange(typeNameEnd, exprEnd);
    if (openBrace >= exprEnd || *openBrace != '{') {
        return NULL;
    }
    cursor = openBrace;
    while (cursor < exprEnd) {
        char ch = *cursor;

        if (inString) {
            if (ch == '\\' && cursor + 1 < exprEnd) {
                cursor += 2;
                continue;
            }
            if (ch == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            inString = 1;
            quote = ch;
            cursor++;
            continue;
        }
        if (ch == '{') {
            depth++;
        } else if (ch == '}') {
            depth--;
            if (depth == 0) {
                closeBrace = cursor;
                break;
            }
        }
        cursor++;
    }
    if (!closeBrace) {
        return NULL;
    }
    methodDot = skipSpacesInRange(closeBrace + 1, exprEnd);
    if (methodDot >= exprEnd || *methodDot != '.') {
        return NULL;
    }
    methodNameStart = skipSpacesInRange(methodDot + 1, exprEnd);
    methodNameEnd = methodNameStart;
    while (methodNameEnd < exprEnd && (isalnum((unsigned char)*methodNameEnd) || *methodNameEnd == '_')) {
        methodNameEnd++;
    }
    if (methodNameEnd == methodNameStart) {
        return NULL;
    }
    methodArgsStart = skipSpacesInRange(methodNameEnd, exprEnd);
    if (methodArgsStart >= exprEnd || *methodArgsStart != '(') {
        return NULL;
    }
    cursor = methodArgsStart;
    depth = 0;
    inString = 0;
    quote = '\0';
    while (cursor < exprEnd) {
        char ch = *cursor;

        if (inString) {
            if (ch == '\\' && cursor + 1 < exprEnd) {
                cursor += 2;
                continue;
            }
            if (ch == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            inString = 1;
            quote = ch;
            cursor++;
            continue;
        }
        if (ch == '(') {
            depth++;
        } else if (ch == ')') {
            depth--;
            if (depth == 0) {
                methodArgsEnd = cursor;
                break;
            }
        }
        cursor++;
    }
    if (!methodArgsEnd) {
        return NULL;
    }
    cursor = skipSpacesInRange(methodArgsEnd + 1, exprEnd);
    if (cursor != exprEnd) {
        return NULL;
    }

    if (!returnTypeName) {
        if (!functions) {
            return NULL;
        }
        qualifiedMethodName = composeQualifiedLookup(typeNameStart,
                                                     (size_t)(typeNameEnd - typeNameStart),
                                                     methodNameStart,
                                                     (size_t)(methodNameEnd - methodNameStart));
        if (!qualifiedMethodName) {
            free(declTypeName);
            return NULL;
        }
        returnTypeName = findAetherFunctionReturnType(functions,
                                                      qualifiedMethodName,
                                                      strlen(qualifiedMethodName));
        if (!returnTypeName) {
            free(qualifiedMethodName);
            free(declTypeName);
            return NULL;
        }
        inferredReturnType = dupCString(returnTypeName);
        free(qualifiedMethodName);
        if (!inferredReturnType) {
            free(declTypeName);
            return NULL;
        }
        returnTypeName = inferredReturnType;
    }

    snprintf(tempName, sizeof(tempName), "__aether_obj_%d", lineNumber > 0 ? lineNumber : 0);
    if (!appendMappedType(&out, typeNameStart, typeNameEnd) ||
        !bufferAppend(&out, " ") ||
        !bufferAppend(&out, tempName) ||
        !bufferAppend(&out, " = new ") ||
        !bufferAppendN(&out, typeNameStart, (size_t)(typeNameEnd - typeNameStart)) ||
        !bufferAppend(&out, "();")) {
        free(declTypeName);
        free(inferredReturnType);
        free(out.data);
        return NULL;
    }

    cursor = openBrace + 1;
    while (cursor < closeBrace) {
        const char *segmentStart;
        const char *segmentEnd;
        const char *fieldEnd = NULL;
        const char *fieldNameEnd;
        const char *valueStart;
        const char *valueEnd;
        int fieldDepth = 0;

        while (cursor < closeBrace &&
               (isspace((unsigned char)*cursor) || *cursor == ',')) {
            cursor++;
        }
        if (cursor >= closeBrace) {
            break;
        }
        segmentStart = cursor;
        segmentEnd = segmentStart;
        while (segmentEnd < closeBrace) {
            char ch = *segmentEnd;

            if (ch == '(' || ch == '[' || ch == '{' || ch == '<') {
                fieldDepth++;
            } else if (ch == ')' || ch == ']' || ch == '}' || ch == '>') {
                if (fieldDepth > 0) {
                    fieldDepth--;
                }
            } else if (ch == ':' && fieldDepth == 0 && !fieldEnd) {
                fieldEnd = segmentEnd;
            } else if (ch == ',' && fieldDepth == 0) {
                break;
            }
            segmentEnd++;
        }
        if (!fieldEnd) {
            free(declTypeName);
            free(inferredReturnType);
            free(out.data);
            return NULL;
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
        if (!bufferAppend(&out, "\n") ||
            !bufferAppendN(&out, lineStart, (size_t)(indentStart - lineStart)) ||
            !bufferAppend(&out, tempName) ||
            !bufferAppend(&out, ".") ||
            !bufferAppendN(&out, segmentStart, (size_t)(fieldNameEnd - segmentStart)) ||
            !bufferAppend(&out, " = ") ||
            !bufferAppendN(&out, valueStart, (size_t)(valueEnd - valueStart)) ||
            !bufferAppend(&out, ";")) {
            free(declTypeName);
            free(inferredReturnType);
            free(out.data);
            return NULL;
        }
        cursor = segmentEnd;
        if (cursor < closeBrace && *cursor == ',') {
            cursor++;
        }
    }

    if (!bufferAppend(&out, "\n") ||
        !appendMappedType(&out,
                          returnTypeName,
                          returnTypeName + strlen(returnTypeName)) ||
        !bufferAppend(&out, " ") ||
        !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart)) ||
        !bufferAppend(&out, " = ") ||
        !bufferAppend(&out, tempName) ||
        !bufferAppend(&out, ".") ||
        !bufferAppendN(&out, methodNameStart, (size_t)(methodNameEnd - methodNameStart)) ||
        !bufferAppendN(&out, methodArgsStart, (size_t)(methodArgsEnd - methodArgsStart + 1)) ||
        !bufferAppend(&out, ";")) {
        free(declTypeName);
        free(inferredReturnType);
        free(out.data);
        return NULL;
    }

    free(declTypeName);
    free(inferredReturnType);
    return out.data;
}

static char *translateTypedDeclLine(const char *lineStart,
                                    const char *body,
                                    const char *lineEnd,
                                    int isConst,
                                    const AetherFunctionTable *functions,
                                    int lineNumber) {
    const char *cursor = body + (isConst ? 5 : 3);
    const char *nameStart;
    const char *nameEnd;
    const char *colon;
    const char *afterColon;
    const char *typeEnd;
    const char *equals = NULL;
    char *inlineIfExpr = NULL;
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

    if (!isConst && equals < lineEnd && *equals == '=') {
        const char *exprStart = skipSpaces(equals + 1);
        const char *exprEnd = lineEnd;
        const char *scan = typeEnd;
        int hasOpenArraySuffix = 0;

        while (exprEnd > exprStart && isspace((unsigned char)exprEnd[-1])) {
            exprEnd--;
        }
        if (exprEnd > exprStart && exprEnd[-1] == ';') {
            exprEnd--;
        }
        while (exprEnd > exprStart && isspace((unsigned char)exprEnd[-1])) {
            exprEnd--;
        }

        while (scan > afterColon && isspace((unsigned char)scan[-1])) {
            scan--;
        }
        if (scan - afterColon >= 2 && scan[-2] == '[' && scan[-1] == ']') {
            hasOpenArraySuffix = 1;
        }
        if (hasOpenArraySuffix &&
            exprEnd - exprStart == 2 &&
            exprStart[0] == '[' &&
            exprStart[1] == ']') {
            if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
                !appendMappedParamTypeAndName(&out,
                                              afterColon,
                                              typeEnd,
                                              nameStart,
                                              nameEnd) ||
                !bufferAppend(&out, ";")) {
                free(out.data);
                return NULL;
            }
            return out.data;
        }
    }

    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart))) {
        free(out.data);
        return NULL;
    }
    if (!isConst && equals < lineEnd && *equals == '=') {
        const char *exprStart = skipSpaces(equals + 1);
        const char *typeNameStart = afterColon;
        const char *typeNameEnd = typeEnd;
        const char *initNameEnd = exprStart;
        const char *openDelim;
        const char *closeDelim;
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
        openDelim = skipSpaces(initNameEnd);
        closeDelim = lineEnd;
        while (closeDelim > exprStart && isspace((unsigned char)closeDelim[-1])) {
            closeDelim--;
        }
        if (closeDelim > exprStart && closeDelim[-1] == ';') {
            closeDelim--;
        }
        while (closeDelim > exprStart && isspace((unsigned char)closeDelim[-1])) {
            closeDelim--;
        }
        if (openDelim < lineEnd &&
            (*openDelim == '{' || *openDelim == '(') &&
            closeDelim > openDelim &&
            ((*openDelim == '{' && closeDelim[-1] == '}') ||
             (*openDelim == '(' && closeDelim[-1] == ')'))) {
            typeMatches = ((size_t)(typeNameEnd - typeNameStart) == (size_t)(initNameEnd - exprStart) &&
                           strncmp(typeNameStart, exprStart, (size_t)(typeNameEnd - typeNameStart)) == 0);
        }
        if (typeMatches) {
            const char *entryCursor = openDelim + 1;
            const char *entryLimit = closeDelim - 1;

            if (!appendMappedParamTypeAndName(&initOut,
                                              afterColon,
                                              typeEnd,
                                              nameStart,
                                              nameEnd) ||
                !bufferAppend(&initOut, " = new ") ||
                !bufferAppendN(&initOut, exprStart, (size_t)(initNameEnd - exprStart)) ||
                !bufferAppend(&initOut, "();")) {
                free(initOut.data);
                free(out.data);
                return NULL;
            }
            while (entryCursor < entryLimit) {
                const char *segmentStart;
                const char *segmentEnd;
                const char *fieldEnd;
                const char *fieldNameEnd;
                const char *valueStart;
                const char *valueEnd;
                int depth = 0;

                while (entryCursor < entryLimit &&
                       (isspace((unsigned char)*entryCursor) || *entryCursor == ',')) {
                    entryCursor++;
                }
                if (entryCursor >= entryLimit) {
                    break;
                }
                segmentStart = entryCursor;
                segmentEnd = segmentStart;
                fieldEnd = NULL;
                while (segmentEnd < entryLimit) {
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
                if (entryCursor < entryLimit && *entryCursor == ',') {
                    entryCursor++;
                }
            }
            free(out.data);
            return initOut.data;
        }
        if (!isConst) {
            char *inlineMethodDecl = translateInlineObjectInitMethodDeclLine(lineStart,
                                                                             body,
                                                                             nameStart,
                                                                             nameEnd,
                                                                             exprStart,
                                                                             lineEnd,
                                                                             typeNameStart,
                                                                             typeNameEnd,
                                                                             functions,
                                                                             lineNumber);
            if (inlineMethodDecl) {
                free(out.data);
                return inlineMethodDecl;
            }
        }
    }
    if (isConst && !bufferAppend(&out, "const ")) {
        free(out.data);
        return NULL;
    }
    if (equals < lineEnd && *equals == '=') {
        const char *exprStart = skipSpaces(equals + 1);
        const char *exprEnd = lineEnd;
        while (exprEnd > exprStart && isspace((unsigned char)exprEnd[-1])) {
            exprEnd--;
        }
        if (exprEnd > exprStart && exprEnd[-1] == ';') {
            exprEnd--;
        }
        while (exprEnd > exprStart && isspace((unsigned char)exprEnd[-1])) {
            exprEnd--;
        }
        inlineIfExpr = rewriteInlineIfExpression(exprStart, exprEnd, NULL, NULL, 0);
    }
    if (!appendMappedParamTypeAndName(&out,
                                      afterColon,
                                      typeEnd,
                                      nameStart,
                                      nameEnd)) {
        free(out.data);
        free(inlineIfExpr);
        return NULL;
    }
    if (inlineIfExpr) {
        if (!bufferAppend(&out, " = ") ||
            !bufferAppend(&out, inlineIfExpr) ||
            !bufferAppend(&out, ";")) {
            free(out.data);
            free(inlineIfExpr);
            return NULL;
        }
        free(inlineIfExpr);
        return out.data;
    }
    if (typeEnd < lineEnd && !bufferAppendN(&out, typeEnd, (size_t)(lineEnd - typeEnd))) {
        free(out.data);
        return NULL;
    }
    return out.data;
}

static char *translateArrayAppendLine(const char *lineStart,
                                      const char *body,
                                      const char *lineEnd,
                                      const AetherBindingTable *bindings,
                                      const TypeBlockState *typeState,
                                      int isMethod) {
    const char *equals;
    const char *lhsStart;
    const char *lhsEnd;
    const char *rhsStart;
    const char *plus;
    const char *itemOpen;
    const char *itemClose;
    const char *itemStart;
    const char *itemEnd;
    char *rewrittenLhs = NULL;
    char *rewrittenItem = NULL;
    Buffer out = {0};

    if (!body || !lineEnd ||
        startsWithWord(body, lineEnd, "let") ||
        startsWithWord(body, lineEnd, "const") ||
        startsWithWord(body, lineEnd, "if") ||
        startsWithWord(body, lineEnd, "while") ||
        startsWithWord(body, lineEnd, "for") ||
        startsWithWord(body, lineEnd, "loop") ||
        startsWithWord(body, lineEnd, "ret") ||
        startsWithWord(body, lineEnd, "fx") ||
        startsWithWord(body, lineEnd, "par")) {
        return NULL;
    }

    equals = findCharInRange(body, lineEnd, '=');
    if (!equals || (equals + 1 < lineEnd && equals[1] == '=')) {
        return NULL;
    }
    lhsStart = skipSpacesInRange(body, equals);
    lhsEnd = equals;
    while (lhsEnd > lhsStart && isspace((unsigned char)lhsEnd[-1])) {
        lhsEnd--;
    }
    if (lhsEnd == lhsStart) {
        return NULL;
    }
    rhsStart = skipSpacesInRange(equals + 1, lineEnd);
    plus = findCharInRange(rhsStart, lineEnd, '+');
    if (!plus) {
        return NULL;
    }
    itemOpen = skipSpacesInRange(plus + 1, lineEnd);
    if (itemOpen >= lineEnd || *itemOpen != '[') {
        return NULL;
    }
    itemClose = itemOpen ? findLastCharInRange(itemOpen + 1, lineEnd, ']') : NULL;
    if (!itemOpen || !itemClose || itemClose <= itemOpen) {
        return NULL;
    }

    {
        const char *rhsNameEnd = plus;
        while (rhsNameEnd > rhsStart && isspace((unsigned char)rhsNameEnd[-1])) {
            rhsNameEnd--;
        }
        if ((size_t)(rhsNameEnd - rhsStart) != (size_t)(lhsEnd - lhsStart) ||
            strncmp(lhsStart, rhsStart, (size_t)(lhsEnd - lhsStart)) != 0) {
            return NULL;
        }
    }

    itemStart = skipSpacesInRange(itemOpen + 1, itemClose);
    itemEnd = itemClose;
    while (itemEnd > itemStart && isspace((unsigned char)itemEnd[-1])) {
        itemEnd--;
    }
    if (itemEnd == itemStart) {
        return NULL;
    }

    rewrittenLhs = rewriteMethodScopedExpr(lhsStart, lhsEnd, bindings, typeState, isMethod);
    rewrittenItem = rewriteMethodScopedExpr(itemStart, itemEnd, bindings, typeState, isMethod);
    if (!rewrittenLhs || !rewrittenItem) {
        free(rewrittenLhs);
        free(rewrittenItem);
        free(out.data);
        return NULL;
    }

    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppend(&out, "setlength(") ||
        !bufferAppend(&out, rewrittenLhs) ||
        !bufferAppend(&out, ", length(") ||
        !bufferAppend(&out, rewrittenLhs) ||
        !bufferAppend(&out, ") + 1);\n") ||
        !bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppend(&out, rewrittenLhs) ||
        !bufferAppend(&out, "[length(") ||
        !bufferAppend(&out, rewrittenLhs) ||
        !bufferAppend(&out, ") - 1] = ") ||
        !bufferAppend(&out, rewrittenItem) ||
        !bufferAppend(&out, ";")) {
        free(rewrittenLhs);
        free(rewrittenItem);
        free(out.data);
        return NULL;
    }
    free(rewrittenLhs);
    free(rewrittenItem);
    return out.data;
}

static char *translateInferredDeclLine(const char *lineStart,
                                       const char *body,
                                       const char *lineEnd,
                                       int isConst,
                                       const AetherBindingTable *bindings,
                                       const AetherFunctionTable *functions,
                                       const AetherFieldTable *fields,
                                       const char *path,
                                       int lineNumber) {
    const char *cursor = body + (isConst ? 5 : 3);
    const char *nameStart;
    const char *nameEnd;
    const char *equals;
    const char *exprStart;
    char *typeName = NULL;
    char *inlineIfExpr = NULL;
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
    {
        const char *exprEnd = lineEnd;
        while (exprEnd > exprStart && isspace((unsigned char)exprEnd[-1])) {
            exprEnd--;
        }
        if (exprEnd > exprStart && exprEnd[-1] == ';') {
            exprEnd--;
        }
        while (exprEnd > exprStart && isspace((unsigned char)exprEnd[-1])) {
            exprEnd--;
        }
        inlineIfExpr = rewriteInlineIfExpression(exprStart, exprEnd, NULL, NULL, 0);
    }

    if (isConst) {
        if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
            !bufferAppend(&out, "const ") ||
            !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart)) ||
            !(inlineIfExpr
                  ? (bufferAppend(&out, " = ") &&
                     bufferAppend(&out, inlineIfExpr) &&
                     bufferAppend(&out, ";"))
                  : bufferAppendN(&out, equals, (size_t)(lineEnd - equals)))) {
            free(inlineIfExpr);
            free(out.data);
            return NULL;
        }
        free(inlineIfExpr);
        return out.data;
    }

    {
        char *inlineMethodDecl = translateInlineObjectInitMethodDeclLine(lineStart,
                                                                         body,
                                                                         nameStart,
                                                                         nameEnd,
                                                                         exprStart,
                                                                         lineEnd,
                                                                         NULL,
                                                                         NULL,
                                                                         functions,
                                                                         lineNumber);
        if (inlineMethodDecl) {
            return inlineMethodDecl;
        }
    }

    typeName = inferAetherBindingTypeName(exprStart, lineEnd, bindings, functions, fields);
    if (!typeName) {
        char detail[512];
        char hint[512];
        char *newObjectType = inferNewObjectTypeName(exprStart, lineEnd);

        snprintf(detail,
                 sizeof(detail),
                 "cannot infer the type of '%.*s' from its initializer.",
                 (int)(nameEnd - nameStart),
                 nameStart);
        if (newObjectType) {
            snprintf(hint,
                     sizeof(hint),
                     "write `let %.*s: %s = new %s();`.",
                     (int)(nameEnd - nameStart),
                     nameStart,
                     newObjectType,
                     newObjectType);
            free(newObjectType);
        } else {
            snprintf(hint,
                     sizeof(hint),
                     "add an explicit type, for example `let %.*s: Int = ...;`.",
                     (int)(nameEnd - nameStart),
                     nameStart);
        }
        reportAetherRewriteError(path, lineNumber, "declaration", detail, hint);
        free(inlineIfExpr);
        return NULL;
    }

    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppend(&out, mapTypeName(typeName)) ||
        !bufferAppend(&out, " ") ||
        !bufferAppendN(&out, nameStart, (size_t)(nameEnd - nameStart)) ||
        !(inlineIfExpr
              ? (bufferAppend(&out, " = ") &&
                 bufferAppend(&out, inlineIfExpr) &&
                 bufferAppend(&out, ";"))
              : bufferAppendN(&out, equals, (size_t)(lineEnd - equals)))) {
        free(typeName);
        free(inlineIfExpr);
        free(out.data);
        return NULL;
    }
    free(typeName);
    free(inlineIfExpr);
    return out.data;
}

static char *translateConditionLine(const char *lineStart,
                                    const char *body,
                                    const char *lineEnd,
                                    const char *keyword) {
    const char *exprStart;
    const char *brace;
    char *rewrittenExpr = NULL;
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
    rewrittenExpr = rewriteMethodScopedExpr(exprStart, brace, NULL, NULL, 0);
    if (!rewrittenExpr) {
        return NULL;
    }

    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppend(&out, keyword) ||
        !bufferAppend(&out, " (") ||
        !bufferAppend(&out, rewrittenExpr) ||
        !bufferAppend(&out, ")") ||
        !bufferAppendN(&out, brace, (size_t)(lineEnd - brace))) {
        free(rewrittenExpr);
        free(out.data);
        return NULL;
    }
    free(rewrittenExpr);

    return out.data;
}

static char *translateReturnInlineIfLine(const char *lineStart,
                                         const char *body,
                                         const char *lineEnd,
                                         const AetherBindingTable *bindings,
                                         const TypeBlockState *typeState,
                                         int isMethod) {
    const char *exprStart;
    const char *exprEnd;
    char *rewrittenExpr;
    Buffer out = {0};

    if (!startsWithWord(body, lineEnd, "ret")) {
        return NULL;
    }
    exprStart = skipSpacesInRange(body + 3, lineEnd);
    if (exprStart >= lineEnd) {
        return NULL;
    }
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
    rewrittenExpr = rewriteInlineIfExpression(exprStart, exprEnd, bindings, typeState, isMethod);
    if (!rewrittenExpr) {
        return NULL;
    }
    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppend(&out, "return ") ||
        !bufferAppend(&out, rewrittenExpr) ||
        !bufferAppend(&out, ";")) {
        free(rewrittenExpr);
        free(out.data);
        return NULL;
    }
    free(rewrittenExpr);
    return out.data;
}

static char *translateReturnObjectInitLine(const char *lineStart,
                                           const char *body,
                                           const char *lineEnd,
                                           const AetherBindingTable *bindings,
                                           const TypeBlockState *typeState,
                                           int isMethod,
                                           int lineNumber) {
    const char *exprStart;
    const char *exprEnd;
    const char *typeNameStart;
    const char *typeNameEnd;
    const char *openBrace;
    const char *closeBrace;
    const char *entryCursor;
    const char *entryLimit;
    char **items = NULL;
    size_t itemCount = 0;
    char tempName[64];
    Buffer out = {0};

    if (!startsWithWord(body, lineEnd, "ret")) {
        return NULL;
    }
    exprStart = skipSpacesInRange(body + 3, lineEnd);
    if (exprStart >= lineEnd || (!isalpha((unsigned char)*exprStart) && *exprStart != '_')) {
        return NULL;
    }
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

    typeNameStart = exprStart;
    typeNameEnd = exprStart;
    while (typeNameEnd < exprEnd &&
           (isalnum((unsigned char)*typeNameEnd) || *typeNameEnd == '_')) {
        typeNameEnd++;
    }
    openBrace = skipSpacesInRange(typeNameEnd, exprEnd);
    if (typeNameEnd == typeNameStart || openBrace >= exprEnd || *openBrace != '{') {
        return NULL;
    }
    closeBrace = findMatchingCloseBrace(openBrace, exprEnd);
    if (!closeBrace) {
        return NULL;
    }
    if (skipSpacesInRange(closeBrace + 1, exprEnd) != exprEnd) {
        return NULL;
    }

    entryCursor = openBrace + 1;
    entryLimit = closeBrace;
    if (entryCursor < entryLimit) {
        if (!splitTopLevelCommaList(entryCursor, entryLimit, &items, &itemCount)) {
            return NULL;
        }
    }

    snprintf(tempName, sizeof(tempName), "__aether_retobj_%d", lineNumber);
    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppendN(&out, typeNameStart, (size_t)(typeNameEnd - typeNameStart)) ||
        !bufferAppend(&out, " ") ||
        !bufferAppend(&out, tempName) ||
        !bufferAppend(&out, " = new ") ||
        !bufferAppendN(&out, typeNameStart, (size_t)(typeNameEnd - typeNameStart)) ||
        !bufferAppend(&out, "();")) {
        freeTupleItemTypes(items, itemCount);
        free(out.data);
        return NULL;
    }

    for (size_t i = 0; i < itemCount; i++) {
        const char *segmentStart = items[i];
        const char *segmentEnd = items[i] + strlen(items[i]);
        const char *fieldSep = NULL;
        const char *cursor = segmentStart;
        int depth = 0;
        char *rewrittenValue = NULL;
        const char *fieldNameEnd;
        const char *valueStart;
        const char *valueEnd;

        while (cursor < segmentEnd) {
            char ch = *cursor;
            if (ch == '"' || ch == '\'') {
                char quote = ch;
                cursor++;
                while (cursor < segmentEnd) {
                    if (*cursor == '\\' && cursor + 1 < segmentEnd) {
                        cursor += 2;
                        continue;
                    }
                    if (*cursor == quote) {
                        cursor++;
                        break;
                    }
                    cursor++;
                }
                continue;
            }
            if (ch == '(' || ch == '[' || ch == '{' || ch == '<') {
                depth++;
            } else if ((ch == ')' || ch == ']' || ch == '}' || ch == '>') && depth > 0) {
                depth--;
            } else if (ch == ':' && depth == 0) {
                fieldSep = cursor;
                break;
            }
            cursor++;
        }

        if (!fieldSep) {
            freeTupleItemTypes(items, itemCount);
            free(out.data);
            return NULL;
        }
        fieldNameEnd = fieldSep;
        while (fieldNameEnd > segmentStart && isspace((unsigned char)fieldNameEnd[-1])) {
            fieldNameEnd--;
        }
        valueStart = skipSpacesInRange(fieldSep + 1, segmentEnd);
        valueEnd = segmentEnd;
        while (valueEnd > valueStart && isspace((unsigned char)valueEnd[-1])) {
            valueEnd--;
        }
        rewrittenValue = rewriteInlineIfExpression(valueStart, valueEnd, bindings, typeState, isMethod);
        if (!rewrittenValue) {
            rewrittenValue = rewriteMethodScopedExpr(valueStart, valueEnd, bindings, typeState, isMethod);
        }
        if (!rewrittenValue) {
            freeTupleItemTypes(items, itemCount);
            free(out.data);
            return NULL;
        }
        if (!bufferAppend(&out, "\n") ||
            !bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
            !bufferAppend(&out, tempName) ||
            !bufferAppend(&out, ".") ||
            !bufferAppendN(&out, segmentStart, (size_t)(fieldNameEnd - segmentStart)) ||
            !bufferAppend(&out, " = ") ||
            !bufferAppend(&out, rewrittenValue) ||
            !bufferAppend(&out, ";")) {
            free(rewrittenValue);
            freeTupleItemTypes(items, itemCount);
            free(out.data);
            return NULL;
        }
        free(rewrittenValue);
    }

    if (!bufferAppend(&out, "\n") ||
        !bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppend(&out, "return ") ||
        !bufferAppend(&out, tempName) ||
        !bufferAppend(&out, ";")) {
        freeTupleItemTypes(items, itemCount);
        free(out.data);
        return NULL;
    }

    freeTupleItemTypes(items, itemCount);
    return out.data;
}

static char *translateCallInlineIfArgsLine(const char *lineStart,
                                           const char *body,
                                           const char *lineEnd,
                                           const TypeBlockState *typeState,
                                           int isMethod) {
    const char *openParen;
    const char *closeParen;
    const char *tail;
    char **items = NULL;
    size_t itemCount = 0;
    Buffer out = {0};
    int rewrittenAny = 0;

    if (!body || !lineEnd ||
        startsWithWord(body, lineEnd, "let") ||
        startsWithWord(body, lineEnd, "const") ||
        startsWithWord(body, lineEnd, "if") ||
        startsWithWord(body, lineEnd, "while") ||
        startsWithWord(body, lineEnd, "for") ||
        startsWithWord(body, lineEnd, "loop") ||
        startsWithWord(body, lineEnd, "ret") ||
        startsWithWord(body, lineEnd, "fx") ||
        startsWithWord(body, lineEnd, "par")) {
        return NULL;
    }

    openParen = findCharInRange(body, lineEnd, '(');
    closeParen = openParen ? findMatchingCloseParen(openParen, lineEnd) : NULL;
    if (!openParen || !closeParen) {
        return NULL;
    }
    tail = skipSpacesInRange(closeParen + 1, lineEnd);
    if (!(tail == lineEnd || (tail + 1 == lineEnd && *tail == ';'))) {
        return NULL;
    }

    if (!bufferAppendN(&out, lineStart, (size_t)(openParen + 1 - lineStart))) {
        free(out.data);
        return NULL;
    }
    if (openParen + 1 < closeParen) {
        if (!splitTopLevelCommaList(openParen + 1, closeParen, &items, &itemCount)) {
            free(out.data);
            return NULL;
        }
    }
    for (size_t i = 0; i < itemCount; i++) {
        const char *itemStart = items[i];
        const char *itemEnd = items[i] + strlen(items[i]);
        char *rewritten = rewriteInlineIfExpression(itemStart, itemEnd, NULL, typeState, isMethod);
        if (rewritten) {
            rewrittenAny = 1;
            if (!bufferAppend(&out, rewritten)) {
                free(rewritten);
                freeTupleItemTypes(items, itemCount);
                free(out.data);
                return NULL;
            }
            free(rewritten);
        } else if (!bufferAppend(&out, items[i])) {
            freeTupleItemTypes(items, itemCount);
            free(out.data);
            return NULL;
        }
        if (i + 1 < itemCount && !bufferAppend(&out, ", ")) {
            freeTupleItemTypes(items, itemCount);
            free(out.data);
            return NULL;
        }
    }
    freeTupleItemTypes(items, itemCount);
    if (!rewrittenAny) {
        free(out.data);
        return NULL;
    }
    if (!bufferAppendN(&out, closeParen, (size_t)(lineEnd - closeParen))) {
        free(out.data);
        return NULL;
    }
    return out.data;
}

static char *translateAssignInlineIfLine(const char *lineStart,
                                         const char *body,
                                         const char *lineEnd,
                                         const TypeBlockState *typeState,
                                         int isMethod) {
    const char *equals;
    const char *lhsStart;
    const char *lhsEnd;
    const char *rhsStart;
    const char *rhsEnd;
    char *rewrittenLhs = NULL;
    char *rewrittenRhs = NULL;
    Buffer out = {0};

    if (!body || !lineEnd ||
        startsWithWord(body, lineEnd, "let") ||
        startsWithWord(body, lineEnd, "const") ||
        startsWithWord(body, lineEnd, "if") ||
        startsWithWord(body, lineEnd, "while") ||
        startsWithWord(body, lineEnd, "for") ||
        startsWithWord(body, lineEnd, "loop") ||
        startsWithWord(body, lineEnd, "ret") ||
        startsWithWord(body, lineEnd, "fx") ||
        startsWithWord(body, lineEnd, "par")) {
        return NULL;
    }

    equals = findCharInRange(body, lineEnd, '=');
    if (!equals ||
        (equals + 1 < lineEnd && equals[1] == '=') ||
        (equals > body && equals[-1] == '=') ||
        (equals + 1 < lineEnd && equals[1] == '>')) {
        return NULL;
    }
    lhsStart = skipSpacesInRange(body, equals);
    lhsEnd = equals;
    while (lhsEnd > lhsStart && isspace((unsigned char)lhsEnd[-1])) {
        lhsEnd--;
    }
    rhsStart = skipSpacesInRange(equals + 1, lineEnd);
    rhsEnd = lineEnd;
    while (rhsEnd > rhsStart && isspace((unsigned char)rhsEnd[-1])) {
        rhsEnd--;
    }
    if (rhsEnd > rhsStart && rhsEnd[-1] == ';') {
        rhsEnd--;
    }
    while (rhsEnd > rhsStart && isspace((unsigned char)rhsEnd[-1])) {
        rhsEnd--;
    }
    rewrittenRhs = rewriteInlineIfExpression(rhsStart, rhsEnd, NULL, typeState, isMethod);
    if (!rewrittenRhs) {
        return NULL;
    }
    rewrittenLhs = rewriteMethodScopedExpr(lhsStart, lhsEnd, NULL, typeState, isMethod);
    if (!rewrittenLhs) {
        free(rewrittenRhs);
        return NULL;
    }
    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
        !bufferAppend(&out, rewrittenLhs) ||
        !bufferAppend(&out, " = ") ||
        !bufferAppend(&out, rewrittenRhs) ||
        !bufferAppend(&out, ";")) {
        free(rewrittenLhs);
        free(rewrittenRhs);
        free(out.data);
        return NULL;
    }
    free(rewrittenLhs);
    free(rewrittenRhs);
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
                                 const AetherFunctionTable *functions,
                                 const AetherFieldTable *fields,
                                 const char *path,
                                 int lineNumber) {
    const char *rest = skipSpaces(body + 6);
    char *translatedRest = NULL;
    Buffer out = {0};

    if (rest >= lineEnd) {
        return dupRange(lineStart, lineEnd);
    }
    if (strncmp(rest, "fn ", 3) == 0) {
        translatedRest = translateFnLine(rest, rest, lineEnd, NULL);
    } else if (strncmp(rest, "let ", 4) == 0 && hasTypedDeclSeparator(rest, lineEnd, 0)) {
        translatedRest = translateTypedDeclLine(rest, rest, lineEnd, 0, functions, lineNumber);
    } else if (strncmp(rest, "const ", 6) == 0 && hasTypedDeclSeparator(rest, lineEnd, 1)) {
        translatedRest = translateTypedDeclLine(rest, rest, lineEnd, 1, functions, lineNumber);
    } else if (strncmp(rest, "let ", 4) == 0) {
        translatedRest = translateInferredDeclLine(rest, rest, lineEnd, 0, bindings, functions, fields, path, lineNumber);
    } else if (strncmp(rest, "const ", 6) == 0) {
        translatedRest = translateInferredDeclLine(rest, rest, lineEnd, 1, bindings, functions, fields, path, lineNumber);
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
        !appendMappedParamTypeAndName(&out,
                                      typeStart,
                                      typeEnd,
                                      nameStart,
                                      nameEnd) ||
        !bufferAppend(&out, ";")) {
        free(out.data);
        return NULL;
    }
    return out.data;
}

static int isTypeFieldDeclarationLine(const char *body, const char *lineEnd) {
    const char *cursor;

    if (!body || !lineEnd) {
        return 0;
    }
    cursor = body;
    if (cursor >= lineEnd || !(isalpha((unsigned char)*cursor) || *cursor == '_')) {
        return 0;
    }
    while (cursor < lineEnd && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    cursor = skipSpaces(cursor);
    return cursor < lineEnd && *cursor == ':';
}

static char *extractTypeFieldTypeName(const char *body, const char *lineEnd) {
    const char *cursor;
    const char *typeStart;
    const char *typeEnd;

    if (!isTypeFieldDeclarationLine(body, lineEnd)) {
        return NULL;
    }
    cursor = body;
    while (cursor < lineEnd && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    cursor = skipSpaces(cursor);
    if (cursor >= lineEnd || *cursor != ':') {
        return NULL;
    }
    typeStart = skipSpacesInRange(cursor + 1, lineEnd);
    typeEnd = typeStart;
    while (typeEnd < lineEnd && *typeEnd != ';' && *typeEnd != '=') {
        typeEnd++;
    }
    while (typeEnd > typeStart && isspace((unsigned char)typeEnd[-1])) {
        typeEnd--;
    }
    if (typeEnd <= typeStart) {
        return NULL;
    }
    return trimmedCopy(typeStart, typeEnd);
}

static char *translateLine(const char *lineStart,
                           const char *lineEnd,
                           JsonAliasState *jsonState,
                           const AetherBindingTable *bindings,
                           const AetherFunctionTable *functions,
                           const AetherFieldTable *fields,
                           const char *path,
                           int lineNumber) {
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
        return translateExportLine(lineStart, body, lineEnd, bindings, functions, fields, path, lineNumber);
    }
    if (strncmp(body, "fn ", 3) == 0) {
        return translateFnLine(lineStart, body, lineEnd, NULL);
    }
    if (strncmp(body, "let mut ", 8) == 0) {
        char *normalized = NULL;
        const char *normalizedBody;
        const char *normalizedEnd;

        reportAetherCompatibilityWarning(path,
                                         lineNumber,
                                         "`let mut` is accepted as `let`; `mut` is ignored.");

        if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart)) ||
            !bufferAppend(&out, "let ") ||
            !bufferAppendN(&out, body + 8, (size_t)(lineEnd - (body + 8)))) {
            free(out.data);
            return NULL;
        }

        normalized = out.data;
        normalizedBody = normalized + (body - lineStart);
        normalizedEnd = normalized + strlen(normalized);

        if (hasTypedDeclSeparator(normalizedBody, normalizedEnd, 0)) {
            return translateTypedDeclLine(normalized,
                                          normalizedBody,
                                          normalizedEnd,
                                          0,
                                          functions,
                                          lineNumber);
        }
        return translateInferredDeclLine(normalized,
                                         normalizedBody,
                                         normalizedEnd,
                                         0,
                                         bindings,
                                         functions,
                                         fields,
                                         path,
                                         lineNumber);
    }
    if (strncmp(body, "let ", 4) == 0 && hasTypedDeclSeparator(body, lineEnd, 0)) {
        return translateTypedDeclLine(lineStart, body, lineEnd, 0, functions, lineNumber);
    }
    if (strncmp(body, "const ", 6) == 0 && hasTypedDeclSeparator(body, lineEnd, 1)) {
        return translateTypedDeclLine(lineStart, body, lineEnd, 1, functions, lineNumber);
    }
    if (strncmp(body, "let ", 4) == 0) {
        return translateInferredDeclLine(lineStart, body, lineEnd, 0, bindings, functions, fields, path, lineNumber);
    }
    if (strncmp(body, "const ", 6) == 0) {
        return translateInferredDeclLine(lineStart, body, lineEnd, 1, bindings, functions, fields, path, lineNumber);
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
    {
        char *translated = translateReturnInlineIfLine(lineStart, body, lineEnd, NULL, NULL, 0);
        if (!translated) {
            translated = translateReturnObjectInitLine(lineStart, body, lineEnd, NULL, NULL, 0, lineNumber);
        }
        if (!translated) {
            translated = translateAssignInlineIfLine(lineStart, body, lineEnd, NULL, 0);
        }
        if (!translated) {
            translated = translateCallInlineIfArgsLine(lineStart, body, lineEnd, NULL, 0);
        }
        if (translated) {
            return translated;
        }
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
                appendAetherInlineCallAlias(&out,
                                            nameStart,
                                            (size_t)(nameEnd - nameStart),
                                            skipSpacesInRange(nameEnd, lineEnd),
                                            lineEnd,
                                            &advancedCursor)) {
                body = advancedCursor;
                continue;
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
                appendAetherExtensionCallRewrite(&out,
                                                 nameStart,
                                                 (size_t)(nameEnd - nameStart),
                                                 skipSpacesInRange(nameEnd, lineEnd),
                                                 lineEnd,
                                                 bindings,
                                                 functions,
                                                 fields,
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

static char *translateLineInMethod(const char *lineStart,
                                   const char *lineEnd,
                                   JsonAliasState *jsonState,
                                   const AetherBindingTable *bindings,
                                   const AetherFunctionTable *functions,
                                   const AetherFieldTable *fields,
                                   const TypeBlockState *typeState,
                                   int isMethod,
                                   const char *path,
                                   int lineNumber) {
    char *translated = translateLine(lineStart,
                                     lineEnd,
                                     jsonState,
                                     bindings,
                                     functions,
                                     fields,
                                     path,
                                     lineNumber);
    char *rewritten;
    char *opaqueNilRewritten;
    char *lenRewritten;

    if (!translated) {
        return NULL;
    }
    if (isMethod && typeState) {
        rewritten = rewriteMethodScopedExpr(translated,
                                            translated + strlen(translated),
                                            bindings,
                                            typeState,
                                            isMethod);
        free(translated);
        if (!rewritten) {
            return NULL;
        }
    } else {
        rewritten = translated;
    }
    opaqueNilRewritten = rewriteAetherOpaqueNilComparisons(rewritten,
                                                           rewritten + strlen(rewritten),
                                                           bindings,
                                                           functions,
                                                           fields);
    free(rewritten);
    if (!opaqueNilRewritten) {
        return NULL;
    }
    lenRewritten = rewriteAetherLenPropertyExpr(opaqueNilRewritten,
                                                opaqueNilRewritten + strlen(opaqueNilRewritten),
                                                bindings,
                                                functions,
                                                fields);
    free(opaqueNilRewritten);
    return lenRewritten;
}

char *aetherRewriteSource(const char *source, const char *path) {
    char *preprocessed = NULL;
    const char *cursor;
    Buffer out = {0};
    PendingContracts pending = {0};
    FunctionContracts fnState = {0};
    ParBlockState parState = {0};
    TypeBlockState typeState = {0};
    ObjectInitState objectInitState = {0};
    JsonAliasState jsonState = {0};
    AetherBindingTable bindingTable = {0};
    AetherFunctionTable functionTable = {0};
    AetherFieldTable fieldTable = {0};
    ToonLiteralTable toonTable = {0};
    AetherTupleTable tupleTable = {0};
    int braceDepth = 0;
    int lineNumber = 1;
    int outputLineNumber = 1;
    int nextTupleTypeId = 0;

    if (!source) {
        return NULL;
    }
    aetherClearRewriteLineMap();
    preprocessed = preprocessToonBlocks(source, path);
    if (!preprocessed) {
        return NULL;
    }
    {
        char *inlineIfPreprocessed = preprocessInlineIfDecls(preprocessed, path);
        if (!inlineIfPreprocessed) {
            free(preprocessed);
            return NULL;
        }
        free(preprocessed);
        preprocessed = inlineIfPreprocessed;
    }
    if (!appendTopLevelForwardDeclarations(&out,
                                           preprocessed,
                                           &outputLineNumber,
                                           &tupleTable,
                                           &nextTupleTypeId)) {
        free(preprocessed);
        freeAetherTupleTable(&tupleTable);
        free(out.data);
        return NULL;
    }
    cursor = preprocessed;

    while (*cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *body;
        char *translated = NULL;
        int lineDelta = 0;
        int braceDeltaOverrideSet = 0;
        int braceDeltaOverride = 0;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }

        body = lineStart;
        while (body < lineEnd && (*body == ' ' || *body == '\t')) {
            body++;
        }

        if (body == lineEnd) {
            if (!bufferAppend(&out, "\n") ||
                !trackRewriteOutputLines("\n", &outputLineNumber, lineNumber)) {
                freePendingContracts(&pending);
                clearFunctionContracts(&fnState);
                clearParBlockState(&parState);
                clearTypeBlockState(&typeState);
                freeAetherBindingTable(&bindingTable);
                freeAetherFunctionTable(&functionTable);
                freeToonLiteralTable(&toonTable);
                freeAetherTupleTable(&tupleTable);
                free(preprocessed);
                free(out.data);
                return NULL;
            }
            if (*lineEnd == '\n') {
                cursor = lineEnd + 1;
                lineNumber++;
            } else {
                cursor = lineEnd;
            }
            continue;
        }

        size_t lineOutputStart = out.len;
        int outputLineBeforeLine = outputLineNumber;

        if (!maybeLoadImportedBindings(&bindingTable, &functionTable, body, lineEnd, path)) {
            freePendingContracts(&pending);
            clearFunctionContracts(&fnState);
            clearParBlockState(&parState);
            clearTypeBlockState(&typeState);
            freeAetherBindingTable(&bindingTable);
            freeAetherFunctionTable(&functionTable);
            freeToonLiteralTable(&toonTable);
            freeAetherTupleTable(&tupleTable);
            free(preprocessed);
            free(out.data);
            return NULL;
        }
        maybeRecordToonLiteralBinding(&toonTable, body, lineEnd);
        maybeRecordAetherFunctionReturnType(&functionTable, body, lineEnd, NULL, typeState.name);
        maybeRecordAetherBindingType(&bindingTable, body, lineEnd, &functionTable, &fieldTable);

        if (fnState.active &&
            braceDepth == fnState.bodyDepth &&
            startsWithWord(body, lineEnd, "ret")) {
            const char *exprStart = skipSpacesInRange(body + 3, lineEnd);
            const char *exprEnd = lineEnd;
            while (exprEnd > exprStart && isspace((unsigned char)exprEnd[-1])) {
                exprEnd--;
            }
            if (exprEnd > exprStart && exprEnd[-1] == ';') {
                exprEnd--;
            }
            while (exprEnd > exprStart && isspace((unsigned char)exprEnd[-1])) {
                exprEnd--;
            }
            if (exprEnd > exprStart) {
                fnState.sawValueReturn = 1;
            }
        } else if (fnState.active &&
                   braceDepth == fnState.bodyDepth &&
                   body < lineEnd &&
                   *body != '}' &&
                   !startsWithWord(body, lineEnd, "if") &&
                   !startsWithWord(body, lineEnd, "else") &&
                   !startsWithWord(body, lineEnd, "loop") &&
                   !startsWithWord(body, lineEnd, "for") &&
                   !startsWithWord(body, lineEnd, "while") &&
                   !startsWithWord(body, lineEnd, "let") &&
                   !startsWithWord(body, lineEnd, "const") &&
                   !startsWithWord(body, lineEnd, "fn")) {
            fnState.sawFallthroughTopLevelStmt = 1;
        }

        if (fnState.active &&
            fnState.postExpr &&
            fnState.isVoid &&
            !fnState.tupleTypeName &&
            braceDepth == fnState.bodyDepth) {
            if (isStandaloneCloseBrace(body, lineEnd)) {
                char *indent = dupRange(lineStart, body);
                size_t outLenBefore = out.len;
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
                if (!appendContractGuard(&out,
                                         indent,
                                         fnState.name,
                                         "post",
                                         fnState.postExpr,
                                         &jsonState,
                                         &toonTable)) {
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
                if (!trackRewriteOutputLines(out.data + outLenBefore, &outputLineNumber, lineNumber)) {
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
                size_t outLenBefore = out.len;
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
                if (parState.joinLines.len > 0 &&
                    !trackRewriteOutputLines(out.data + outLenBefore, &outputLineNumber, lineNumber)) {
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
                    reportAetherRewriteError(path,
                                             lineNumber,
                                             "par",
                                             "only direct call statements are allowed inside par blocks.",
                                             "move side effects into direct calls inside `par { ... }`.");
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

        if (!translated &&
            objectInitState.active &&
            braceDepth == objectInitState.bodyDepth) {
            if (isStandaloneCloseBraceWithOptionalSemicolon(body, lineEnd)) {
                translated = dupCString("");
                braceDeltaOverrideSet = 1;
                braceDeltaOverride = -1;
            } else if (body < lineEnd && !isLineComment(body, lineEnd)) {
                translated = translateObjectInitFieldLine(lineStart, body, lineEnd, objectInitState.targetName);
            }
        }

        if (fnState.active &&
            (startsWithWord(body, lineEnd, "@pre") ||
             startsWithWord(body, lineEnd, "@post"))) {
            reportMisplacedContractBlock(path, lineStart, lineNumber);
            freePendingContracts(&pending);
            clearFunctionContracts(&fnState);
            clearParBlockState(&parState);
            clearTypeBlockState(&typeState);
            freeAetherBindingTable(&bindingTable);
            freeAetherFunctionTable(&functionTable);
            freeToonLiteralTable(&toonTable);
            freeAetherTupleTable(&tupleTable);
            free(preprocessed);
            free(out.data);
            return NULL;
        } else if (startsWithWord(body, lineEnd, "@pre")) {
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
            } else if (startsWithWord(body, lineEnd, "fn")) {
                translated = dupRange(lineStart, lineEnd);
            } else if (isUnsupportedTupleLetPattern(body, lineEnd)) {
                translated = translateTupleDestructureLetLine(lineStart,
                                                              body,
                                                              lineEnd,
                                                              &tupleTable,
                                                              path,
                                                              lineNumber);
                if (!translated) {
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    clearParBlockState(&parState);
                    clearTypeBlockState(&typeState);
                    freeAetherBindingTable(&bindingTable);
                    freeAetherFunctionTable(&functionTable);
                    freeToonLiteralTable(&toonTable);
                    freeAetherTupleTable(&tupleTable);
                    free(preprocessed);
                    free(out.data);
                    return NULL;
                }
            } else if (startsWithWord(body, lineEnd, "let")) {
                if (findTupleInitializerCallee(body, lineEnd, &tupleTable)) {
                    reportAetherRewriteError(path,
                                             lineNumber,
                                             "feature",
                                             "tuple-return calls must be destructured directly.",
                                             "use `let (a, b) = pair();` rather than binding the tuple call to one name.");
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    clearParBlockState(&parState);
                    clearTypeBlockState(&typeState);
                    freeAetherBindingTable(&bindingTable);
                    freeAetherFunctionTable(&functionTable);
                    freeToonLiteralTable(&toonTable);
                    freeAetherTupleTable(&tupleTable);
                    free(preprocessed);
                    free(out.data);
                    return NULL;
                }
                translated = translateMultiLineTypedObjectInitOpenLine(lineStart,
                                                                       body,
                                                                       lineEnd,
                                                                       &objectInitState);
                if (translated) {
                    braceDeltaOverrideSet = 1;
                    braceDeltaOverride = 1;
                } else {
                    translated = translateLineInMethod(lineStart,
                                                       lineEnd,
                                                       &jsonState,
                                                       &bindingTable,
                                                       &functionTable,
                                                       &fieldTable,
                                                       &typeState,
                                                       fnState.active && fnState.isMethod,
                                                       path,
                                                       lineNumber);
                }
            } else if (fnState.active && fnState.tupleTypeName && startsWithWord(body, lineEnd, "ret")) {
                translated = translateTupleReturnLine(lineStart,
                                                      body,
                                                      lineEnd,
                                                      &fnState,
                                                      &jsonState,
                                                      &toonTable);
                if (!translated) {
                    reportAetherRewriteError(path,
                                             lineNumber,
                                             "internal",
                                             "tuple return rewrite failed.",
                                             "this is a compiler defect; simplify the tuple return or report the issue.");
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    clearParBlockState(&parState);
                    clearTypeBlockState(&typeState);
                    freeAetherBindingTable(&bindingTable);
                    freeAetherFunctionTable(&functionTable);
                    freeToonLiteralTable(&toonTable);
                    freeAetherTupleTable(&tupleTable);
                    free(preprocessed);
                    free(out.data);
                    return NULL;
                }
            } else if (fnState.active && startsWithWord(body, lineEnd, "ret")) {
                translated = translateReturnObjectInitLine(lineStart,
                                                           body,
                                                           lineEnd,
                                                           &bindingTable,
                                                           &typeState,
                                                           fnState.isMethod,
                                                           lineNumber);
                if (!translated && fnState.postExpr) {
                    translated = translateReturnWithPost(lineStart,
                                                         body,
                                                         lineEnd,
                                                         &fnState,
                                                         &bindingTable,
                                                         &typeState,
                                                         &jsonState,
                                                         &toonTable);
                }
                if (!translated) {
                    translated = translateLineInMethod(lineStart,
                                                       lineEnd,
                                                       &jsonState,
                                                       &bindingTable,
                                                       &functionTable,
                                                       &fieldTable,
                                                       &typeState,
                                                       fnState.active && fnState.isMethod,
                                                       path,
                                                       lineNumber);
                }
            } else if (typeState.active &&
                       isTypeFieldDeclarationLine(body, lineEnd) &&
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
                if (typeFieldLineEndsWithComma(body, lineEnd)) {
                    reportAetherRewriteError(path,
                                             lineNumber,
                                             "type",
                                             "type fields must end with ';', not ','.",
                                             "write `fieldName: Type;` for each field inside a `type` block.");
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    clearParBlockState(&parState);
                    clearTypeBlockState(&typeState);
                    freeAetherBindingTable(&bindingTable);
                    freeAetherFunctionTable(&functionTable);
                    freeToonLiteralTable(&toonTable);
                    freeAetherTupleTable(&tupleTable);
                    free(preprocessed);
                    free(out.data);
                    return NULL;
                }
                translated = translateTypeFieldLine(lineStart, body, lineEnd);
            } else {
                translated = translateArrayAppendLine(lineStart,
                                                     body,
                                                     lineEnd,
                                                     &bindingTable,
                                                     &typeState,
                                                     fnState.active && fnState.isMethod);
                if (!translated) {
                    translated = translateLineInMethod(lineStart,
                                                       lineEnd,
                                                       &jsonState,
                                                       &bindingTable,
                                                       &functionTable,
                                                       &fieldTable,
                                                       &typeState,
                                                       fnState.active && fnState.isMethod,
                                                       path,
                                                       lineNumber);
                }
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
                freeAetherTupleTable(&tupleTable);
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
        if (!trackRewriteOutputLines(translated, &outputLineNumber, lineNumber)) {
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
        lineDelta = braceDeltaOverrideSet ? braceDeltaOverride : braceDeltaForLine(translated);

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
                freeAetherTupleTable(&tupleTable);
                free(preprocessed);
                free(out.data);
                return NULL;
            }
        }

        if (braceDeltaOverrideSet && braceDeltaOverride > 0 && objectInitState.targetName) {
            objectInitState.active = 1;
            objectInitState.bodyDepth = braceDepth + braceDeltaOverride;
        }

        if (startsWithWord(body, lineEnd, "fn")) {
            char *fnName = NULL;
            char *returnType = NULL;
            char **tupleItemTypes = NULL;
            size_t tupleItemCount = 0;
            char tupleTypeName[64];
            const AetherTupleSig *existingTupleSig = NULL;
            int hasTupleReturn = 0;

            if (!hasExplicitFunctionReturnType(body, lineEnd)) {
                reportAetherRewriteError(path,
                                         lineNumber,
                                         "function",
                                         "functions must declare an explicit return type.",
                                         "write `fn name(args) -> Void { ... }` or replace `Void` with the actual return type.");
                free(translated);
                freePendingContracts(&pending);
                clearFunctionContracts(&fnState);
                clearParBlockState(&parState);
                clearTypeBlockState(&typeState);
                freeAetherBindingTable(&bindingTable);
                freeAetherFunctionTable(&functionTable);
                freeToonLiteralTable(&toonTable);
                freeAetherTupleTable(&tupleTable);
                free(preprocessed);
                free(out.data);
                return NULL;
            }
            if (!extractFunctionSignature(body, lineEnd, &fnName, &returnType)) {
                reportAetherRewriteError(path,
                                         lineNumber,
                                         "function",
                                         "could not parse the function signature.",
                                         "check the parameter list and return type syntax: `fn name(arg: Type) -> ReturnType { ... }`.");
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

            hasTupleReturn = parseTupleTypeList(returnType,
                                               returnType + strlen(returnType),
                                               &tupleItemTypes,
                                               &tupleItemCount);
            free(translated);
            translated = NULL;
            if (hasTupleReturn) {
                char *tuplePostExpr = NULL;
                char tuplePostDetail[256] = {0};

                if (typeState.active) {
                    reportAetherRewriteError(path,
                                             lineNumber,
                                             "feature",
                                             "tuple return types are currently only supported on top-level functions.",
                                             "return a record/object from methods, or move tuple-return logic to a top-level helper function.");
                    freeTupleItemTypes(tupleItemTypes, tupleItemCount);
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
                    freeAetherTupleTable(&tupleTable);
                    free(preprocessed);
                    free(out.data);
                    return NULL;
                }
                existingTupleSig = findAetherTupleSig(&tupleTable, fnName, strlen(fnName));
                if (existingTupleSig && existingTupleSig->tupleTypeName) {
                    snprintf(tupleTypeName,
                             sizeof(tupleTypeName),
                             "%s",
                             existingTupleSig->tupleTypeName);
                } else {
                    nextTupleTypeId++;
                    snprintf(tupleTypeName, sizeof(tupleTypeName), "__aether_tuple_%d", nextTupleTypeId);
                }
                if (pending.postExpr) {
                    FunctionContracts tuplePostState = {0};

                    tuplePostState.tupleTypeName = tupleTypeName;
                    tuplePostState.tupleItemCount = tupleItemCount;
                    tuplePostExpr = rewriteTuplePostExpr(pending.postExpr,
                                                         pending.postExpr + strlen(pending.postExpr),
                                                         &tuplePostState,
                                                         tuplePostDetail,
                                                         sizeof(tuplePostDetail));
                    if (!tuplePostExpr) {
                        reportAetherRewriteError(path,
                                                 lineNumber,
                                                 "contract",
                                                 tuplePostDetail[0]
                                                     ? tuplePostDetail
                                                     : "tuple-return @post rewrite failed.",
                                                 "use positional tuple slots in @post, for example `result.0` and `result.1`.");
                        freeTupleItemTypes(tupleItemTypes, tupleItemCount);
                        free(fnName);
                        free(returnType);
                        freePendingContracts(&pending);
                        clearFunctionContracts(&fnState);
                        clearParBlockState(&parState);
                        clearTypeBlockState(&typeState);
                        freeAetherBindingTable(&bindingTable);
                        freeAetherFunctionTable(&functionTable);
                        freeToonLiteralTable(&toonTable);
                        freeAetherTupleTable(&tupleTable);
                        free(preprocessed);
                        free(out.data);
                        return NULL;
                    }
                    free(pending.postExpr);
                    pending.postExpr = tuplePostExpr;
                }
                translated = translateTupleFnLine(lineStart,
                                                  body,
                                                  lineEnd,
                                                  tupleTypeName,
                                                  tupleItemTypes,
                                                  tupleItemCount);
                if (!translated) {
                    reportAetherRewriteError(path,
                                             lineNumber,
                                             "internal",
                                             "tuple function signature rewrite failed.",
                                             "this is a compiler defect; simplify the tuple-return signature or report the issue.");
                }
                if (!translated ||
                    !setAetherTupleSig(&tupleTable, fnName, tupleTypeName, tupleItemTypes, tupleItemCount) ||
                    !setAetherFunctionReturnType(&functionTable, fnName, "Void")) {
                    freeTupleItemTypes(tupleItemTypes, tupleItemCount);
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
                    freeAetherTupleTable(&tupleTable);
                    free(preprocessed);
                        free(out.data);
                        return NULL;
                    }
            } else {
                translated = translateFnLine(lineStart, body, lineEnd, &typeState);
                if (!translated) {
                    free(fnName);
                    free(returnType);
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    clearParBlockState(&parState);
                    clearTypeBlockState(&typeState);
                    freeAetherBindingTable(&bindingTable);
                    freeAetherFunctionTable(&functionTable);
                    freeToonLiteralTable(&toonTable);
                    freeAetherTupleTable(&tupleTable);
                    free(preprocessed);
                    free(out.data);
                    return NULL;
                }
            }

            if (out.len >= lineOutputStart) {
                out.len = lineOutputStart;
                if (out.data) {
                    out.data[out.len] = '\0';
                }
                outputLineNumber = outputLineBeforeLine;
                if (!bufferAppend(&out, translated) ||
                    !trackRewriteOutputLines(translated, &outputLineNumber, lineNumber)) {
                    freeTupleItemTypes(tupleItemTypes, tupleItemCount);
                    free(returnType);
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    clearParBlockState(&parState);
                    clearTypeBlockState(&typeState);
                    freeAetherBindingTable(&bindingTable);
                    freeAetherFunctionTable(&functionTable);
                    freeToonLiteralTable(&toonTable);
                    freeAetherTupleTable(&tupleTable);
                    free(preprocessed);
                    free(out.data);
                    free(translated);
                    return NULL;
                }
                lineDelta = braceDeltaOverrideSet ? braceDeltaOverride : braceDeltaForLine(translated);
            }

            if ((pending.preExpr || pending.postExpr) && lineDelta > 0) {
                char *methodPreExpr = NULL;
                char *methodPostExpr = NULL;
                char *indent = buildContractIndent(lineStart, body);
                size_t outLenBefore = out.len;
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
                if (typeState.active) {
                    if (pending.preExpr) {
                        methodPreExpr = rewriteMethodScopedExpr(pending.preExpr,
                                                                pending.preExpr + strlen(pending.preExpr),
                                                                &bindingTable,
                                                                &typeState,
                                                                1);
                        if (!methodPreExpr) {
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
                    }
                    if (pending.postExpr) {
                        methodPostExpr = rewriteMethodScopedExpr(pending.postExpr,
                                                                 pending.postExpr + strlen(pending.postExpr),
                                                                 &bindingTable,
                                                                 &typeState,
                                                                 1);
                        if (!methodPostExpr) {
                            free(methodPreExpr);
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
                    }
                }
                if (pending.preExpr) {
                    char *preWithLen = rewriteAetherLenPropertyExpr(methodPreExpr ? methodPreExpr : pending.preExpr,
                                                                    (methodPreExpr ? methodPreExpr : pending.preExpr) +
                                                                        strlen(methodPreExpr ? methodPreExpr : pending.preExpr),
                                                                    &bindingTable,
                                                                    &functionTable,
                                                                    &fieldTable);
                    if (!preWithLen) {
                        free(methodPreExpr);
                        free(methodPostExpr);
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
                    if (methodPreExpr) {
                        free(methodPreExpr);
                    }
                    methodPreExpr = preWithLen;
                }
                if (pending.postExpr) {
                    char *postWithLen = rewriteAetherLenPropertyExpr(methodPostExpr ? methodPostExpr : pending.postExpr,
                                                                     (methodPostExpr ? methodPostExpr : pending.postExpr) +
                                                                         strlen(methodPostExpr ? methodPostExpr : pending.postExpr),
                                                                     &bindingTable,
                                                                     &functionTable,
                                                                     &fieldTable);
                    if (!postWithLen) {
                        free(methodPreExpr);
                        free(methodPostExpr);
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
                    if (methodPostExpr) {
                        free(methodPostExpr);
                    }
                    methodPostExpr = postWithLen;
                }
                if (pending.preExpr &&
                    !appendContractGuard(&out,
                                         indent,
                                         fnName,
                                         "pre",
                                         methodPreExpr ? methodPreExpr : pending.preExpr,
                                         &jsonState,
                                         &toonTable)) {
                    free(methodPreExpr);
                    free(methodPostExpr);
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
                if (pending.preExpr &&
                    !trackRewriteOutputLines(out.data + outLenBefore, &outputLineNumber, lineNumber)) {
                    free(methodPreExpr);
                    free(methodPostExpr);
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
                if (methodPostExpr) {
                    free(pending.postExpr);
                    pending.postExpr = methodPostExpr;
                    methodPostExpr = NULL;
                }
                free(methodPreExpr);
                free(methodPostExpr);
            }

            clearFunctionContracts(&fnState);
            fnState.active = lineDelta > 0;
            fnState.bodyDepth = braceDepth + lineDelta;
            fnState.isVoid = hasTupleReturn || (returnType && strcmp(returnType, "Void") == 0);
            fnState.isMethod = typeState.active || functionHasSelfLikeReceiverParam(body, lineEnd);
            fnState.bindingCountBefore = bindingTable.count;
            fnState.name = fnName;
            fnState.postExpr = pending.postExpr
                                   ? dupRange(pending.postExpr,
                                              pending.postExpr + strlen(pending.postExpr))
                                   : NULL;
            if (hasTupleReturn) {
                fnState.tupleTypeName = dupCString(tupleTypeName);
                if (!fnState.tupleTypeName) {
                    freeTupleItemTypes(tupleItemTypes, tupleItemCount);
                    free(returnType);
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    clearParBlockState(&parState);
                    clearTypeBlockState(&typeState);
                    freeAetherBindingTable(&bindingTable);
                    freeAetherFunctionTable(&functionTable);
                    freeToonLiteralTable(&toonTable);
                    freeAetherTupleTable(&tupleTable);
                    free(preprocessed);
                    free(out.data);
                    return NULL;
                }
                fnState.tupleItemTypes = (char **)calloc(tupleItemCount, sizeof(char *));
                if (!fnState.tupleItemTypes) {
                    freeTupleItemTypes(tupleItemTypes, tupleItemCount);
                    free(returnType);
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    clearParBlockState(&parState);
                    clearTypeBlockState(&typeState);
                    freeAetherBindingTable(&bindingTable);
                    freeAetherFunctionTable(&functionTable);
                    freeToonLiteralTable(&toonTable);
                    freeAetherTupleTable(&tupleTable);
                    free(preprocessed);
                    free(out.data);
                    return NULL;
                }
                fnState.tupleItemCount = tupleItemCount;
                for (size_t tupleIndex = 0; tupleIndex < tupleItemCount; tupleIndex++) {
                    fnState.tupleItemTypes[tupleIndex] = dupCString(tupleItemTypes[tupleIndex]);
                    if (!fnState.tupleItemTypes[tupleIndex]) {
                        freeTupleItemTypes(tupleItemTypes, tupleItemCount);
                        free(returnType);
                        freePendingContracts(&pending);
                        clearFunctionContracts(&fnState);
                        clearParBlockState(&parState);
                        clearTypeBlockState(&typeState);
                        freeAetherBindingTable(&bindingTable);
                        freeAetherFunctionTable(&functionTable);
                        freeToonLiteralTable(&toonTable);
                        freeAetherTupleTable(&tupleTable);
                        free(preprocessed);
                        free(out.data);
                        return NULL;
                    }
                }
            }
            if (fnState.active &&
                !recordAetherFunctionParamBindings(&bindingTable, body, lineEnd)) {
                freeTupleItemTypes(tupleItemTypes, tupleItemCount);
                free(returnType);
                freePendingContracts(&pending);
                clearFunctionContracts(&fnState);
                clearParBlockState(&parState);
                clearTypeBlockState(&typeState);
                freeAetherBindingTable(&bindingTable);
                freeAetherFunctionTable(&functionTable);
                freeToonLiteralTable(&toonTable);
                freeAetherTupleTable(&tupleTable);
                free(preprocessed);
                free(out.data);
                return NULL;
            }
            if (fnState.active && fnState.isMethod && typeState.name) {
                if (!setAetherBindingType(&bindingTable, "self", typeState.name) ||
                    !setAetherBindingType(&bindingTable, "myself", typeState.name)) {
                    freeTupleItemTypes(tupleItemTypes, tupleItemCount);
                    free(returnType);
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    clearParBlockState(&parState);
                    clearTypeBlockState(&typeState);
                    freeAetherBindingTable(&bindingTable);
                    freeAetherFunctionTable(&functionTable);
                    freeAetherFieldTable(&fieldTable);
                    freeToonLiteralTable(&toonTable);
                    freeAetherTupleTable(&tupleTable);
                    free(preprocessed);
                    free(out.data);
                    return NULL;
                }
            }

            freeTupleItemTypes(tupleItemTypes, tupleItemCount);
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
            outputLineNumber++;
            lineEnd++;
            lineNumber++;
        }

        if (startsWithWord(body, lineEnd, "type") && lineDelta > 0) {
            clearTypeBlockState(&typeState);
            typeState.active = 1;
            typeState.bodyDepth = braceDepth + lineDelta;
            typeState.name = extractTypeNameFromLine(body, lineEnd);
        } else if (typeState.active && isTypeFieldDeclarationLine(body, lineEnd)) {
            const char *fieldEnd = body;
            char *fieldName = NULL;
            char *fieldTypeName = NULL;

            while (fieldEnd < lineEnd &&
                   (isalnum((unsigned char)*fieldEnd) || *fieldEnd == '_')) {
                fieldEnd++;
            }
            if (fieldEnd > body &&
                !recordTypeFieldName(&typeState, body, (size_t)(fieldEnd - body))) {
                freePendingContracts(&pending);
                clearFunctionContracts(&fnState);
                clearParBlockState(&parState);
                clearTypeBlockState(&typeState);
                freeAetherBindingTable(&bindingTable);
                freeAetherFunctionTable(&functionTable);
                freeAetherFieldTable(&fieldTable);
                freeToonLiteralTable(&toonTable);
                free(preprocessed);
                free(out.data);
                return NULL;
            }
            if (fieldEnd > body) {
                fieldName = trimmedCopy(body, fieldEnd);
            }
            fieldTypeName = extractTypeFieldTypeName(body, lineEnd);
            if (fieldName && fieldTypeName && typeState.name &&
                !setAetherFieldType(&fieldTable,
                                    typeState.name,
                                    fieldName,
                                    fieldTypeName)) {
                free(fieldName);
                free(fieldTypeName);
                freePendingContracts(&pending);
                clearFunctionContracts(&fnState);
                clearParBlockState(&parState);
                clearTypeBlockState(&typeState);
                freeAetherBindingTable(&bindingTable);
                freeAetherFunctionTable(&functionTable);
                freeAetherFieldTable(&fieldTable);
                freeToonLiteralTable(&toonTable);
                free(preprocessed);
                free(out.data);
                return NULL;
            }
            free(fieldName);
            free(fieldTypeName);
        }
        braceDepth += lineDelta;
        if (fnState.active && braceDepth < fnState.bodyDepth) {
            if (!fnState.isVoid &&
                !fnState.sawValueReturn &&
                fnState.sawFallthroughTopLevelStmt) {
                reportAetherRewriteError(path,
                                         lineNumber,
                                         "function",
                                         "non-Void functions have a fallthrough path with no return value.",
                                         "add `ret value;` on the top-level path that can reach the closing `}`, or declare the function `-> Void` if it only performs side effects.");
                freePendingContracts(&pending);
                clearFunctionContracts(&fnState);
                clearParBlockState(&parState);
                clearObjectInitState(&objectInitState);
                clearTypeBlockState(&typeState);
                freeAetherBindingTable(&bindingTable);
                freeAetherFunctionTable(&functionTable);
                freeAetherFieldTable(&fieldTable);
                freeToonLiteralTable(&toonTable);
                freeAetherTupleTable(&tupleTable);
                free(preprocessed);
                free(out.data);
                return NULL;
            }
            restoreAetherBindingTableCount(&bindingTable, fnState.bindingCountBefore);
            clearFunctionContracts(&fnState);
        }
        if (parState.active && braceDepth < parState.bodyDepth) {
            clearParBlockState(&parState);
        }
        if (objectInitState.active && braceDepth < objectInitState.bodyDepth) {
            clearObjectInitState(&objectInitState);
        }
        if (typeState.active && braceDepth < typeState.bodyDepth) {
            clearTypeBlockState(&typeState);
        }
        cursor = lineEnd;
    }

    freePendingContracts(&pending);
    clearFunctionContracts(&fnState);
    clearParBlockState(&parState);
    clearObjectInitState(&objectInitState);
    clearTypeBlockState(&typeState);
    freeAetherBindingTable(&bindingTable);
    freeAetherFunctionTable(&functionTable);
    freeAetherFieldTable(&fieldTable);
    freeToonLiteralTable(&toonTable);
    freeAetherTupleTable(&tupleTable);
    free(preprocessed);
    return out.data;
}
