#include "aether/translate.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static char *translateTypedDeclLine(const char *lineStart, const char *body, const char *lineEnd, int isConst) {
    const char *cursor = body + (isConst ? 5 : 3);
    const char *nameStart;
    const char *nameEnd;
    const char *colon;
    const char *afterColon;
    const char *typeEnd;
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

    if (!bufferAppendN(&out, lineStart, (size_t)(body - lineStart))) {
        free(out.data);
        return NULL;
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

static char *translateLine(const char *lineStart, const char *lineEnd) {
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
    if (strncmp(body, "fn ", 3) == 0) {
        return translateFnLine(lineStart, body, lineEnd);
    }
    if (strncmp(body, "let ", 4) == 0 && memchr(body, ':', (size_t)(lineEnd - body))) {
        return translateTypedDeclLine(lineStart, body, lineEnd, 0);
    }
    if (strncmp(body, "const ", 6) == 0 && memchr(body, ':', (size_t)(lineEnd - body))) {
        return translateTypedDeclLine(lineStart, body, lineEnd, 1);
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
    if (strncmp(body, "if ", 3) == 0) {
        return translateConditionLine(lineStart, body, lineEnd, "if");
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
        if (!bufferAppendN(&out, body, 1)) {
            free(out.data);
            return NULL;
        }
        body++;
    }

    return out.data;
}

char *aetherRewriteSource(const char *source, const char *path) {
    const char *cursor = source;
    Buffer out = {0};
    PendingContracts pending = {0};
    FunctionContracts fnState = {0};
    int braceDepth = 0;
    (void)path;

    if (!source) {
        return NULL;
    }

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

        if (fnState.active && fnState.postExpr && braceDepth == fnState.bodyDepth) {
            const char *tail = lineEnd;
            while (tail > body && isspace((unsigned char)tail[-1])) {
                tail--;
            }
            if (tail == body + 1 && body < tail && *body == '}') {
                char *indent = dupRange(lineStart, body);
                if (!indent) {
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    free(out.data);
                    return NULL;
                }
                if (!appendContractGuard(&out, indent, fnState.name, "post", fnState.postExpr)) {
                    free(indent);
                    freePendingContracts(&pending);
                    clearFunctionContracts(&fnState);
                    free(out.data);
                    return NULL;
                }
                free(indent);
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

        if (fnState.active && fnState.postExpr && startsWithWord(body, lineEnd, "ret")) {
            translated = translateReturnWithPost(lineStart, body, lineEnd, &fnState);
        } else {
            translated = translateLine(lineStart, lineEnd);
        }
        if (!translated) {
            freePendingContracts(&pending);
            clearFunctionContracts(&fnState);
            free(out.data);
            return NULL;
        }
        if (!bufferAppend(&out, translated)) {
            free(translated);
            freePendingContracts(&pending);
            clearFunctionContracts(&fnState);
            free(out.data);
            return NULL;
        }
        lineDelta = braceDeltaForLine(translated);

        if (startsWithWord(body, lineEnd, "fn")) {
            char *fnName = NULL;
            char *returnType = NULL;

            if (!extractFunctionSignature(body, lineEnd, &fnName, &returnType)) {
                free(translated);
                freePendingContracts(&pending);
                clearFunctionContracts(&fnState);
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
                free(out.data);
                return NULL;
            }
            lineEnd++;
        }
        braceDepth += lineDelta;
        if (fnState.active && braceDepth < fnState.bodyDepth) {
            clearFunctionContracts(&fnState);
        }
        cursor = lineEnd;
    }

    freePendingContracts(&pending);
    clearFunctionContracts(&fnState);
    return out.data;
}
