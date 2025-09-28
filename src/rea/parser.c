#include "rea/parser.h"
#include "ast/ast.h"
#include "core/types.h"
#include "symbol/symbol.h"
#include "core/utils.h"
#include "Pascal/globals.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>
#include "core/list.h"

// Forward declaration from core/utils.c
Token *newToken(TokenType type, const char *value, int line, int column);
// Provided by front-end stubs for Rea
void insertType(const char* name, AST* typeDef);

typedef struct {
    ReaLexer lexer;
    ReaToken current;
    VarType currentFunctionType;
    bool hadError;
    const char* currentClassName; // non-owning pointer to current class name while parsing class body
    const char* currentParentClassName; // non-owning pointer to current parent class name while in class body
    const char* currentModuleName; // non-owning pointer to current module name while inside module body
    int currentMethodIndex; // index of next method in current class for vtable
    int functionDepth; // nesting level of function/procedure declarations
    bool inModule;      // parsing module body
    bool markExport;    // export modifier seen for next declaration
    char **genericTypeNames;
    int genericTypeCount;
    int genericTypeCapacity;
    int *genericFrameStack;
    int genericFrameDepth;
    int genericFrameCapacity;
} ReaParser;

static void reaAdvance(ReaParser *p) { p->current = reaNextToken(&p->lexer); }

static ReaToken reaPeekToken(ReaParser *p) {
    ReaLexer saved = p->lexer;
    return reaNextToken(&saved);
}

// Strict mode control
static int g_rea_strict_mode = 0;
void reaSetStrictMode(int enable) { g_rea_strict_mode = enable ? 1 : 0; }

// Strict scan for forbidden top-level constructs
static bool strictScanTop(AST* n) {
    if (!n) return false;
    if ((n->type == AST_VARIABLE && n->token && n->token->value &&
         (strcasecmp(n->token->value, "myself") == 0 ||
          strcasecmp(n->token->value, "my") == 0)) ||
        n->type == AST_RETURN) {
        return true;
    }
    if (strictScanTop(n->left)) return true;
    if (strictScanTop(n->right)) return true;
    if (strictScanTop(n->extra)) return true;
    for (int i = 0; i < n->child_count; i++) {
        if (strictScanTop(n->children[i])) return true;
    }
    return false;
}

static void ensureGenericFrameCapacity(ReaParser *p) {
    if (p->genericFrameDepth >= p->genericFrameCapacity) {
        int newCap = p->genericFrameCapacity ? p->genericFrameCapacity * 2 : 8;
        int *resized = (int *)realloc(p->genericFrameStack, (size_t)newCap * sizeof(int));
        if (!resized) {
            fprintf(stderr, "Memory allocation failure expanding generic frame stack.\n");
            EXIT_FAILURE_HANDLER();
        }
        p->genericFrameStack = resized;
        p->genericFrameCapacity = newCap;
    }
}

static bool typeNameAlreadyDefined(const char *name) {
    if (!name) return false;
    for (TypeEntry *entry = type_table; entry; entry = entry->next) {
        if (entry->name && strcasecmp(entry->name, name) == 0) {
            return true;
        }
    }
    return false;
}

static void ensureGenericNameCapacity(ReaParser *p) {
    if (p->genericTypeCount >= p->genericTypeCapacity) {
        int newCap = p->genericTypeCapacity ? p->genericTypeCapacity * 2 : 16;
        char **resized = (char **)realloc(p->genericTypeNames, (size_t)newCap * sizeof(char *));
        if (!resized) {
            fprintf(stderr, "Memory allocation failure expanding generic parameter table.\n");
            EXIT_FAILURE_HANDLER();
        }
        for (int i = p->genericTypeCapacity; i < newCap; i++) {
            resized[i] = NULL;
        }
        p->genericTypeNames = resized;
        p->genericTypeCapacity = newCap;
    }
}

static void pushGenericFrame(ReaParser *p) {
    ensureGenericFrameCapacity(p);
    p->genericFrameStack[p->genericFrameDepth++] = p->genericTypeCount;
}

static void popGenericFrame(ReaParser *p) {
    if (p->genericFrameDepth <= 0) return;
    int frameStart = p->genericFrameStack[--p->genericFrameDepth];
    for (int i = p->genericTypeCount - 1; i >= frameStart; i--) {
        free(p->genericTypeNames[i]);
        p->genericTypeNames[i] = NULL;
    }
    p->genericTypeCount = frameStart;
}

static bool addGenericParam(ReaParser *p, const char *name, int line) {
    if (p->genericFrameDepth <= 0) return false;
    int frameStart = p->genericFrameStack[p->genericFrameDepth - 1];
    for (int i = frameStart; i < p->genericTypeCount; i++) {
        if (p->genericTypeNames[i] && strcasecmp(p->genericTypeNames[i], name) == 0) {
            fprintf(stderr, "L%d: duplicate generic parameter '%s'.\n", line, name);
            p->hadError = true;
            return false;
        }
    }
    ensureGenericNameCapacity(p);
    p->genericTypeNames[p->genericTypeCount] = strdup(name);
    if (!p->genericTypeNames[p->genericTypeCount]) {
        fprintf(stderr, "Memory allocation failure storing generic parameter name.\n");
        EXIT_FAILURE_HANDLER();
    }
    p->genericTypeCount++;
    return true;
}

static bool isGenericTypeParam(const ReaParser *p, const char *name) {
    if (!name) return false;
    for (int i = p->genericTypeCount - 1; i >= 0; i--) {
        if (p->genericTypeNames[i] && strcasecmp(p->genericTypeNames[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static void clearGenericState(ReaParser *p) {
    if (!p) return;
    for (int i = 0; i < p->genericTypeCount; i++) {
        free(p->genericTypeNames[i]);
    }
    free(p->genericTypeNames);
    p->genericTypeNames = NULL;
    p->genericTypeCount = 0;
    p->genericTypeCapacity = 0;
    free(p->genericFrameStack);
    p->genericFrameStack = NULL;
    p->genericFrameDepth = 0;
    p->genericFrameCapacity = 0;
}

static bool tokensStructurallyEqual(Token *a, Token *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->type != b->type) return false;
    if ((a->value == NULL) != (b->value == NULL)) return false;
    if (a->value && b->value && strcmp(a->value, b->value) != 0) return false;
    return true;
}

static bool astStructurallyEqual(AST *a, AST *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->type != b->type) return false;
    if (!tokensStructurallyEqual(a->token, b->token)) return false;
    if (a->child_count != b->child_count) return false;
    if (!astStructurallyEqual(a->left, b->left)) return false;
    if (!astStructurallyEqual(a->right, b->right)) return false;
    if (!astStructurallyEqual(a->extra, b->extra)) return false;
    for (int i = 0; i < a->child_count; i++) {
        if (!astStructurallyEqual(a->children[i], b->children[i])) return false;
    }
    return true;
}

static bool appendFunctionBodyNode(AST ***array, int *count, int *capacity, AST *node) {
    if (!node || !array || !count || !capacity) {
        return true;
    }
    if (*count >= *capacity) {
        int new_capacity = (*capacity < 8) ? 8 : (*capacity * 2);
        AST **resized = (AST **)realloc(*array, (size_t)new_capacity * sizeof(AST *));
        if (!resized) {
            return false;
        }
        *array = resized;
        *capacity = new_capacity;
    }
    (*array)[(*count)++] = node;
    return true;
}

static bool collectFunctionBodyNodesRecursive(AST *node, AST ***array, int *count, int *capacity) {
    if (!node) {
        return true;
    }
    if (!appendFunctionBodyNode(array, count, capacity, node)) {
        return false;
    }
    if (!collectFunctionBodyNodesRecursive(node->left, array, count, capacity)) {
        return false;
    }
    if (!collectFunctionBodyNodesRecursive(node->right, array, count, capacity)) {
        return false;
    }
    if (!collectFunctionBodyNodesRecursive(node->extra, array, count, capacity)) {
        return false;
    }
    for (int i = 0; i < node->child_count; i++) {
        if (!collectFunctionBodyNodesRecursive(node->children[i], array, count, capacity)) {
            return false;
        }
    }
    return true;
}

static bool collectFunctionBodyNodes(AST *body, AST ***array, int *count, int *capacity) {
    return collectFunctionBodyNodesRecursive(body, array, count, capacity);
}

static void computeLineRange(AST *node, int *minLine, int *maxLine) {
    if (!node || !minLine || !maxLine) {
        return;
    }
    if (node->token) {
        int line = node->token->line;
        if (line > 0) {
            if (line < *minLine) *minLine = line;
            if (line > *maxLine) *maxLine = line;
        }
    }
    if (node->left) {
        computeLineRange(node->left, minLine, maxLine);
    }
    if (node->right) {
        computeLineRange(node->right, minLine, maxLine);
    }
    if (node->extra) {
        computeLineRange(node->extra, minLine, maxLine);
    }
    for (int i = 0; i < node->child_count; i++) {
        if (node->children[i]) {
            computeLineRange(node->children[i], minLine, maxLine);
        }
    }
}

typedef struct FunctionBodyRange {
    AST *body;
    int min_line;
    int max_line;
} FunctionBodyRange;

static FunctionBodyRange *findFunctionBodyRangeForNode(AST *node,
                                                       FunctionBodyRange *ranges,
                                                       int range_count) {
    if (!node || !ranges || range_count <= 0) {
        return NULL;
    }
    for (AST *current = node; current; current = current->parent) {
        for (int i = 0; i < range_count; i++) {
            if (ranges[i].body == current) {
                return &ranges[i];
            }
        }
    }
    return NULL;
}

static bool addGlobalName(const char ***array, int *count, int *capacity, const char *name) {
    if (!array || !count || !capacity || !name) {
        return false;
    }
    for (int i = 0; i < *count; i++) {
        if (strcasecmp((*array)[i], name) == 0) {
            return true;
        }
    }
    if (*count >= *capacity) {
        int new_capacity = (*capacity < 8) ? 8 : (*capacity * 2);
        const char **resized = (const char **)realloc(*array, (size_t)new_capacity * sizeof(const char *));
        if (!resized) {
            return false;
        }
        *array = resized;
        *capacity = new_capacity;
    }
    (*array)[(*count)++] = name;
    return true;
}

static void collectGlobalNamesFromDecl(AST *decl, const char ***array, int *count, int *capacity) {
    if (!decl) return;
    switch (decl->type) {
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            for (int i = 0; i < decl->child_count; i++) {
                AST *child = decl->children[i];
                if (!child || !child->token || !child->token->value) continue;
                addGlobalName(array, count, capacity, child->token->value);
            }
            break;
        case AST_TYPE_DECL:
        case AST_FUNCTION_DECL:
        case AST_PROCEDURE_DECL:
            if (decl->token && decl->token->value) {
                addGlobalName(array, count, capacity, decl->token->value);
            }
            break;
        default:
            break;
    }
}

static bool statementReferencesNonGlobal(AST *node, const char **names, int count, AST *parent) {
    if (!node) return false;
    if (node->type == AST_VARIABLE && node->token && node->token->value) {
        bool is_field_selector = parent && parent->type == AST_FIELD_ACCESS && parent->right == node;
        if (!is_field_selector) {
            const char *name = node->token->value;
            bool known = false;
            for (int i = 0; i < count; i++) {
                if (strcasecmp(names[i], name) == 0) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                return true;
            }
        }
    }
    if (statementReferencesNonGlobal(node->left, names, count, node)) return true;
    if (statementReferencesNonGlobal(node->right, names, count, node)) return true;
    if (statementReferencesNonGlobal(node->extra, names, count, node)) return true;
    for (int i = 0; i < node->child_count; i++) {
        if (statementReferencesNonGlobal(node->children[i], names, count, node)) return true;
    }
    return false;
}


/* --------------------------------------------------------------------- */
/*  Helpers for array handling                                           */
/* --------------------------------------------------------------------- */

// Forward declarations from the compiler and core utilities.  These are
// needed so the parser can evaluate constant expressions for array bounds
// without pulling in heavy headers.
Value evaluateCompileTimeValue(AST *node); // from compiler/compiler.c
void freeValue(Value *v);                  // from core/utils.c
void addCompilerConstant(const char* name, const Value* value, int line); // from compiler/compiler.h
static AST *parseExpression(ReaParser *p); // forward for array helpers
static AST *parseWriteArgument(ReaParser *p); // forward
static void transformPrintfArgs(ReaParser *p, AST *call, AST *argList);

// Debug tracing for parser steps (enabled with REA_DEBUG_PARSE=1)
/* parser debug tracing removed */

// Parse a sequence of bracketed index expressions after an expression and
// build an AST_ARRAY_ACCESS node.  For multi-dimensional access like
// `a[1][2]` this gathers both indices into a single AST node.
static AST *parseArrayAccess(ReaParser *p, AST *base) {
    AST *access = newASTNode(AST_ARRAY_ACCESS, NULL);
    setLeft(access, base);
    /*
     * Array accesses cannot be typed without semantic information.
     * Mark the access node's type as unknown so later stages can
     * refine it based on the array's declared element type.
     */
    setTypeAST(access, TYPE_UNKNOWN);
    while (p->current.type == REA_TOKEN_LEFT_BRACKET) {
        reaAdvance(p); // consume '['
        AST *indexExpr = NULL;
        if (p->current.type != REA_TOKEN_RIGHT_BRACKET) {
            indexExpr = parseExpression(p);
        }
        if (p->current.type == REA_TOKEN_RIGHT_BRACKET) {
            reaAdvance(p);
        }
        addChild(access, indexExpr);
    }
    return access;
}

// Parse array type suffixes after a variable name in a declaration, e.g.
// `int a[10];`  Returns a new AST_ARRAY_TYPE node wrapping the provided
// base type.  The resulting node's children are AST_SUBRANGE bounds with
// 0-based indexing, and *vtype_out is set to TYPE_ARRAY.
static AST *parseArrayType(ReaParser *p, AST *baseType, VarType *vtype_out) {
    AST *arrType = newASTNode(AST_ARRAY_TYPE, NULL);
    setTypeAST(arrType, TYPE_ARRAY);
    setRight(arrType, baseType);
    while (p->current.type == REA_TOKEN_LEFT_BRACKET) {
        int line = p->current.line;
        reaAdvance(p); // consume '['
        AST *dimExpr = NULL;
        if (p->current.type != REA_TOKEN_RIGHT_BRACKET) {
            dimExpr = parseExpression(p);
        }
        if (p->current.type == REA_TOKEN_RIGHT_BRACKET) {
            reaAdvance(p);
        }
        // Evaluate dimension to a constant upper bound
        int high = -1;
        if (dimExpr) {
            Value v = evaluateCompileTimeValue(dimExpr);
            if (v.type == TYPE_INT64 || v.type == TYPE_INT32 || v.type == TYPE_INT16 || v.type == TYPE_INT8 || v.type == TYPE_UINT64 || v.type == TYPE_UINT32 || v.type == TYPE_UINT16 || v.type == TYPE_UINT8 || v.type == TYPE_WORD || v.type == TYPE_BYTE || v.type == TYPE_INTEGER) {
                high = (int)v.i_val - 1;
            } else {
                fprintf(stderr, "L%d: Array size must be a constant integer.\n", line);
            }
            freeValue(&v);
            freeAST(dimExpr);
        } else {
            fprintf(stderr, "L%d: Missing array size.\n", line);
        }

        Token *lowTok = newToken(TOKEN_INTEGER_CONST, "0", line, 0);
        AST *lowNode = newASTNode(AST_NUMBER, lowTok);
        setTypeAST(lowNode, TYPE_INT64);
        lowNode->i_val = 0;
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", high);
        Token *highTok = newToken(TOKEN_INTEGER_CONST, buf, line, 0);
        AST *highNode = newASTNode(AST_NUMBER, highTok);
        setTypeAST(highNode, TYPE_INT64);
        highNode->i_val = high;
        AST *range = newASTNode(AST_SUBRANGE, NULL);
        setLeft(range, lowNode);
        setRight(range, highNode);
        setTypeAST(range, TYPE_INT64);
        addChild(arrType, range);
    }
    if (vtype_out) *vtype_out = TYPE_ARRAY;
    return arrType;
}

static AST *parseAssignment(ReaParser *p);
static AST *parseConditional(ReaParser *p);
static AST *parseEquality(ReaParser *p);
static AST *parseComparison(ReaParser *p);
static AST *parseShift(ReaParser *p);
static AST *parseAdditive(ReaParser *p);
static AST *parseTerm(ReaParser *p);
static AST *parseFactor(ReaParser *p);
static AST *parseBitwiseAnd(ReaParser *p);
static AST *parseBitwiseXor(ReaParser *p);
static AST *parseBitwiseOr(ReaParser *p);
static AST *parseLogicalAnd(ReaParser *p);
static AST *parseLogicalOr(ReaParser *p);
static AST *parseStatement(ReaParser *p);
static AST *parseVarDecl(ReaParser *p);
static AST *parseReturn(ReaParser *p);
static AST *parseBreak(ReaParser *p);
static AST *parseJoin(ReaParser *p);
static AST *parseIf(ReaParser *p);
static AST *parseWhile(ReaParser *p);
static AST *parseBlock(ReaParser *p);
static AST *parseImport(ReaParser *p);
static AST *parseModule(ReaParser *p);
static void markExported(AST *node);
static AST *parseFunctionDecl(ReaParser *p, Token *nameTok, AST *typeNode, VarType vtype, int methodIndex, bool pointerWrapped);
static AST *parseWhile(ReaParser *p);
static AST *parseDoWhile(ReaParser *p);
static AST *parseFor(ReaParser *p);
static AST *parseSwitch(ReaParser *p);
static AST *parseConstDecl(ReaParser *p);
static AST *parseTypeAlias(ReaParser *p);
static AST *parseImport(ReaParser *p);
static AST *parsePointerParamType(ReaParser *p);
static AST *parseFunctionPointerParamTypes(ReaParser *p);
static AST *buildProcPointerType(AST *returnType, AST *paramList);
static AST *parsePointerVariableAfterName(ReaParser *p, Token *nameTok, AST *baseType);
static AST *parseCallTypeArgumentList(ReaParser *p);
static bool looksLikeCallTypeArguments(ReaParser *p);
static AST *parseMatch(ReaParser *p);
static AST *parseTry(ReaParser *p);
static AST *parseThrow(ReaParser *p);

// Access to global type table provided by Pascal front end
AST *lookupType(const char* name);

static AST *parseGenericParameterList(ReaParser *p) {
    AST *list = newASTNode(AST_COMPOUND, NULL);
    while (p->current.type != REA_TOKEN_GREATER && p->current.type != REA_TOKEN_EOF) {
        if (p->current.type != REA_TOKEN_IDENTIFIER) {
            fprintf(stderr, "L%d: Expected generic parameter name.\n", p->current.line);
            p->hadError = true;
            freeAST(list);
            return NULL;
        }
        char *paramName = (char *)malloc(p->current.length + 1);
        if (!paramName) {
            fprintf(stderr, "Memory allocation failure while parsing generic parameter.\n");
            EXIT_FAILURE_HANDLER();
        }
        memcpy(paramName, p->current.start, p->current.length);
        paramName[p->current.length] = '\0';
        Token *paramTok = newToken(TOKEN_IDENTIFIER, paramName, p->current.line, 0);
        free(paramName);
        if (!addGenericParam(p, paramTok->value, paramTok->line)) {
            // keep parsing to surface additional errors but avoid duplicating nodes
        }
        AST *paramNode = newASTNode(AST_VARIABLE, paramTok);
        addChild(list, paramNode);
        reaAdvance(p);
        if (p->current.type == REA_TOKEN_COMMA) {
            reaAdvance(p);
            continue;
        }
        break;
    }
    if (p->current.type != REA_TOKEN_GREATER) {
        fprintf(stderr, "L%d: Expected '>' to close generic parameter list.\n", p->current.line);
        p->hadError = true;
        freeAST(list);
        return NULL;
    }
    reaAdvance(p);
    return list;
}

// Helper to rewrite 'continue' statements to 'post; continue' inside for-loop bodies
static AST *rewriteContinueWithPost(AST *node, AST *postStmt) {
    if (!node) return NULL;
    if (node->type == AST_CONTINUE) {
        AST *comp = newASTNode(AST_COMPOUND, NULL);
        addChild(comp, copyAST(postStmt));
        addChild(comp, newASTNode(AST_CONTINUE, NULL));
        return comp;
    }
    // Recurse
    node->left = rewriteContinueWithPost(node->left, postStmt);
    node->right = rewriteContinueWithPost(node->right, postStmt);
    node->extra = rewriteContinueWithPost(node->extra, postStmt);
    for (int i = 0; i < node->child_count; i++) {
        if (node->children[i]) node->children[i] = rewriteContinueWithPost(node->children[i], postStmt);
    }
    return node;
}

// Unescape standard escape sequences within a string literal segment
static char *reaUnescapeString(const char *src, size_t len, size_t *out_len) {
    char *buf = (char *)malloc(len + 1);
    if (!buf) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '\\' && i + 1 < len) {
            char n = src[++i];
            switch (n) {
                case 'n': buf[j++] = '\n'; break;
                case 'r': buf[j++] = '\r'; break;
                case 't': buf[j++] = '\t'; break;
                case '\\': buf[j++] = '\\'; break;
                case 0x27: buf[j++] = 0x27; break;
                case '"': buf[j++] = '"'; break;
                case 'x':
                case 'X': {
                    int val = 0;
                    size_t digits = 0;
                    while (i + 1 < len && digits < 2) {
                        char h = src[i + 1];
                        if ((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F')) {
                            i++;
                            digits++;
                            val = val * 16 + (h >= '0' && h <= '9' ? h - '0' : (h & 0x5f) - 'A' + 10);
                        } else {
                            break;
                        }
                    }
                    if (digits > 0) {
                        buf[j++] = (char)val;
                    } else {
                        buf[j++] = '\\';
                        buf[j++] = n;
                    }
                    break;
                }
                default:
                    if (n >= '0' && n <= '7') {
                        int val = n - '0';
                        size_t digits = 1;
                        while (i + 1 < len && digits < 3 && src[i + 1] >= '0' && src[i + 1] <= '7') {
                            val = (val << 3) + (src[++i] - '0');
                            digits++;
                        }
                        buf[j++] = (char)val;
                    } else {
                        // Preserve unknown escape sequences verbatim
                        buf[j++] = '\\';
                        buf[j++] = n;
                    }
                    break;
            }
        } else {
            buf[j++] = c;
        }
    }
    buf[j] = '\0';
    if (out_len) *out_len = j;
    return buf;
}

// Parses an expression optionally followed by formatting specifiers.
static AST *parseWriteArgument(ReaParser *p) {
    int expr_line = p->current.line;
    AST *expr = parseExpression(p);
    if (!expr) return NULL;
    if (p->current.type == REA_TOKEN_COLON) {
        reaAdvance(p);
        if (p->current.type != REA_TOKEN_NUMBER) return expr;
        char *wlex = (char*)malloc(p->current.length + 1);
        if (!wlex) return expr;
        memcpy(wlex, p->current.start, p->current.length);
        wlex[p->current.length] = '\0';
        int width = atoi(wlex);
        free(wlex);
        reaAdvance(p);
        int prec = -1;
        if (p->current.type == REA_TOKEN_COLON) {
            reaAdvance(p);
            if (p->current.type == REA_TOKEN_NUMBER) {
                char *plex = (char*)malloc(p->current.length + 1);
                if (plex) {
                    memcpy(plex, p->current.start, p->current.length);
                    plex[p->current.length] = '\0';
                    prec = atoi(plex);
                    free(plex);
                }
                reaAdvance(p);
            }
        }
        char fmtbuf[32];
        snprintf(fmtbuf, sizeof(fmtbuf), "%d,%d", width, prec);
        Token *fmtTok = newToken(TOKEN_STRING_CONST, fmtbuf, expr_line, 0);
        AST *fmtNode = newASTNode(AST_FORMATTED_EXPR, fmtTok);
        setLeft(fmtNode, expr);
        setTypeAST(fmtNode, TYPE_UNKNOWN);
        return fmtNode;
    }
    return expr;
}

// Transform printf-style calls into a sequence of write arguments.
static void transformPrintfArgs(ReaParser *p, AST *call, AST *argList) {
    size_t nextArg = 0;
    if (!call || !argList) return;
    if (argList->child_count > 0 && argList->children[0] &&
        argList->children[0]->type == AST_STRING &&
        argList->children[0]->token && argList->children[0]->token->value) {
        AST *fmtNode = argList->children[0];
        char *fmt = fmtNode->token->value;
        int line = fmtNode->token->line;
        size_t flen = strlen(fmt);
        char *seg = (char*)malloc(flen + 1);
        size_t seglen = 0;
        nextArg = 1;
        for (size_t i = 0; i < flen; ++i) {
            if (fmt[i] == '%' && i + 1 < flen) {
                if (fmt[i + 1] == '%') {
                    seg[seglen++] = '%';
                    i++;
                } else {
                    size_t j = i + 1;
                    int width = 0;
                    int precision = -1;
                    while (j < flen && isdigit((unsigned char)fmt[j])) {
                        width = width * 10 + (fmt[j] - '0');
                        j++;
                    }
                    if (j < flen && fmt[j] == '.') {
                        j++;
                        precision = 0;
                        while (j < flen && isdigit((unsigned char)fmt[j])) {
                            precision = precision * 10 + (fmt[j] - '0');
                            j++;
                        }
                    }
                    const char *length_mods = "hlLjzt";
                    while (j < flen && strchr(length_mods, fmt[j]) != NULL) {
                        j++;
                    }
                    const char *specifiers = "cdiuoxXfFeEgGaAspn";
                    if (j < flen && strchr(specifiers, fmt[j]) != NULL) {
                        char spec = fmt[j];
                        const char *supported = "cdiufFeEgGaAsc";
                        if (!strchr(supported, spec)) {
                            fprintf(stderr, "L%d: Unsupported printf format specifier '%c'.\n", line, spec);
                            if (p) p->hadError = true;
                            for (size_t k = i; k <= j && k < flen; ++k) {
                                seg[seglen++] = fmt[k];
                            }
                            i = j; // skip specifier
                        } else if (nextArg < (size_t)argList->child_count) {
                            if (seglen > 0) {
                                seg[seglen] = '\0';
                                Token *segTok = newToken(TOKEN_STRING_CONST, seg, line, 0);
                                AST *segNode = newASTNode(AST_STRING, segTok);
                                segNode->i_val = (int)seglen;
                                setTypeAST(segNode, TYPE_STRING);
                                addChild(call, segNode);
                                seglen = 0;
                            }
                            AST *expr = argList->children[nextArg++];
                            if (width > 0 || precision >= 0) {
                                char fmtbuf[32];
                                snprintf(fmtbuf, sizeof(fmtbuf), "%d,%d", width, precision);
                                Token *fmtTok = newToken(TOKEN_STRING_CONST, fmtbuf, line, 0);
                                AST *fmtExpr = newASTNode(AST_FORMATTED_EXPR, fmtTok);
                                setLeft(fmtExpr, expr);
                                setTypeAST(fmtExpr, TYPE_UNKNOWN);
                                addChild(call, fmtExpr);
                            } else {
                                addChild(call, expr);
                            }
                            i = j; // skip specifier
                        } else {
                            seg[seglen++] = '%';
                        }
                    } else {
                        seg[seglen++] = '%';
                    }
                }
            } else {
                seg[seglen++] = fmt[i];
            }
        }
        if (seglen > 0) {
            seg[seglen] = '\0';
            Token *segTok = newToken(TOKEN_STRING_CONST, seg, line, 0);
            AST *segNode = newASTNode(AST_STRING, segTok);
            segNode->i_val = (int)seglen;
            setTypeAST(segNode, TYPE_STRING);
            addChild(call, segNode);
        }
        free(seg);
        freeAST(fmtNode);
    }
    for (; nextArg < (size_t)argList->child_count; ++nextArg) {
        if (argList->children[nextArg]) addChild(call, argList->children[nextArg]);
    }
    argList->children = NULL;
    argList->child_count = 0;
    argList->child_capacity = 0;
}

static int isTypeKeywordToken(ReaTokenType t) {
    switch (t) {
        case REA_TOKEN_INT:
        case REA_TOKEN_INT64:
        case REA_TOKEN_INT32:
        case REA_TOKEN_INT16:
        case REA_TOKEN_INT8:
        case REA_TOKEN_FLOAT:
        case REA_TOKEN_FLOAT32:
        case REA_TOKEN_LONG_DOUBLE:
        case REA_TOKEN_CHAR:
        case REA_TOKEN_BYTE:
        case REA_TOKEN_BOOL:
            return 1;
        default:
            return 0;
    }
}

static VarType tokenTypeToVarType(ReaTokenType t) {
    switch (t) {
        case REA_TOKEN_INT:
        case REA_TOKEN_INT64: return TYPE_INT64;
        case REA_TOKEN_INT32: return TYPE_INT32;
        case REA_TOKEN_INT16: return TYPE_INT16;
        case REA_TOKEN_INT8:  return TYPE_INT8;
        case REA_TOKEN_FLOAT: return TYPE_DOUBLE;
        case REA_TOKEN_FLOAT32: return TYPE_FLOAT;
        case REA_TOKEN_LONG_DOUBLE: return TYPE_LONG_DOUBLE;
        case REA_TOKEN_CHAR: return TYPE_CHAR;
        case REA_TOKEN_BYTE: return TYPE_BYTE;
        case REA_TOKEN_BOOL: return TYPE_BOOLEAN;
        default: return TYPE_UNKNOWN;
    }
}

static AST *parseFactor(ReaParser *p) {
    // Prefix increment/decrement
    if (p->current.type == REA_TOKEN_PLUS_PLUS || p->current.type == REA_TOKEN_MINUS_MINUS) {
        int line = p->current.line;
        int is_inc = (p->current.type == REA_TOKEN_PLUS_PLUS);
        reaAdvance(p);
        AST *target = parseFactor(p);
        if (!target) return NULL;
        if (!(target->type == AST_VARIABLE || target->type == AST_FIELD_ACCESS || target->type == AST_ARRAY_ACCESS || target->type == AST_DEREFERENCE)) {
            fprintf(stderr, "L%d: Parse error: increment/decrement requires a variable (lvalue).\n", line);
            p->hadError = true;
            return NULL;
        }
        // Build 'target += 1' or 'target -= 1'
        Token *oneTok = newToken(TOKEN_INTEGER_CONST, "1", line, 0);
        AST *oneNode = newASTNode(AST_NUMBER, oneTok);
        setTypeAST(oneNode, TYPE_INT64);
        oneNode->i_val = 1;
        Token *opTok = newToken(is_inc ? TOKEN_PLUS : TOKEN_MINUS, is_inc ? "+" : "-", line, 0);
        AST *assign = newASTNode(AST_ASSIGN, opTok);
        setLeft(assign, target);
        setRight(assign, oneNode);
        setTypeAST(assign, target->var_type);
        return assign;
    }
    if (p->current.type == REA_TOKEN_SPAWN) {
        reaAdvance(p); // consume 'spawn'
        AST *call = parseFactor(p);
        if (!call) return NULL;
        AST *node = newThreadSpawn(call);
        setTypeAST(node, TYPE_INT64);
        return node;
    }
    if (isTypeKeywordToken(p->current.type)) {
        ReaToken t = p->current;
        char *lex = (char*)malloc(t.length + 1);
        if (!lex) return NULL;
        memcpy(lex, t.start, t.length);
        lex[t.length] = '\0';
        Token *tok = newToken(TOKEN_IDENTIFIER, lex, t.line, 0);
        free(lex);
        reaAdvance(p);
        if (p->current.type != REA_TOKEN_LEFT_PAREN) {
            freeToken(tok);
            return NULL;
        }
        reaAdvance(p); // consume '('
        AST *expr = parseExpression(p);
        if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
            reaAdvance(p);
        }
        AST *call = newASTNode(AST_PROCEDURE_CALL, tok);
        if (expr) addChild(call, expr);
        setTypeAST(call, tokenTypeToVarType(t.type));
        return call;
    }
    if (p->current.type == REA_TOKEN_SUPER) {
        // super(...) or super.method(...)
        ReaToken supTok = p->current;
        reaAdvance(p); // consume 'super'
        if (!p->currentParentClassName) {
            // Outside of a subclass context; treat as error-like no-op
            return NULL;
        }
        // Case: super(args)
        if (p->current.type == REA_TOKEN_LEFT_PAREN) {
            reaAdvance(p);
            AST *args = newASTNode(AST_COMPOUND, NULL);
            while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
                AST *arg = parseExpression(p);
                if (!arg) break;
                addChild(args, arg);
                if (p->current.type == REA_TOKEN_COMMA) reaAdvance(p); else break;
            }
            if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);
            // Build call to parent constructor alias: ParentName(myself, ...)
            Token *ctorTok = newToken(TOKEN_IDENTIFIER, p->currentParentClassName, supTok.line, 0);
            AST *call = newASTNode(AST_PROCEDURE_CALL, ctorTok);
            // Prepend implicit 'myself' and flag the node so semantic analysis
            // knows it already includes it (for super constructor calls)
            Token *selfTok = newToken(TOKEN_IDENTIFIER, "myself", supTok.line, 0);
            AST *selfVar = newASTNode(AST_VARIABLE, selfTok);
            setTypeAST(selfVar, TYPE_POINTER);
            addChild(call, selfVar);
            call->i_val = 1; // mark as super call containing implicit 'myself'
            if (args && args->child_count > 0) {
                for (int i = 0; i < args->child_count; i++) {
                    addChild(call, args->children[i]);
                    args->children[i] = NULL;
                }
                args->child_count = 0;
            }
            if (args) freeAST(args);
            setTypeAST(call, TYPE_UNKNOWN);
            return call;
        }
        // Case: super.method(args)
        if (p->current.type == REA_TOKEN_DOT) {
            reaAdvance(p);
            if (p->current.type != REA_TOKEN_IDENTIFIER) return NULL;
            char *mlex = (char*)malloc(p->current.length + 1);
            if (!mlex) return NULL;
            memcpy(mlex, p->current.start, p->current.length);
            mlex[p->current.length] = '\0';
            reaAdvance(p); // consume method name
            // Optional args
            AST *args = NULL;
            if (p->current.type == REA_TOKEN_LEFT_PAREN) {
                reaAdvance(p);
                args = newASTNode(AST_COMPOUND, NULL);
                while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
                    AST *arg = parseExpression(p);
                    if (!arg) break;
                    addChild(args, arg);
                    if (p->current.type == REA_TOKEN_COMMA) reaAdvance(p); else break;
                }
                if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);
            }
            // Build mangled name Parent.Method
            size_t ln = strlen(p->currentParentClassName) + 1 + strlen(mlex) + 1;
            char *mangled = (char*)malloc(ln);
            if (!mangled) { free(mlex); return NULL; }
            snprintf(mangled, ln, "%s.%s", p->currentParentClassName, mlex);
            free(mlex);
            Token *nameTok = newToken(TOKEN_IDENTIFIER, mangled, supTok.line, 0);
            free(mangled);
            AST *call = newASTNode(AST_PROCEDURE_CALL, nameTok);
            // Prepend implicit 'myself' and flag node to avoid duplicate insertion
            Token *selfTok = newToken(TOKEN_IDENTIFIER, "myself", supTok.line, 0);
            AST *selfVar = newASTNode(AST_VARIABLE, selfTok);
            setTypeAST(selfVar, TYPE_POINTER);
            addChild(call, selfVar);
            call->i_val = 1; // mark as super call containing implicit 'myself'
            if (args && args->child_count > 0) {
                for (int i = 0; i < args->child_count; i++) {
                    addChild(call, args->children[i]);
                    args->children[i] = NULL;
                }
                args->child_count = 0;
            }
            if (args) freeAST(args);
            setTypeAST(call, TYPE_UNKNOWN);
            return call;
        }
        return NULL;
    }
    if (p->current.type == REA_TOKEN_NEW) {
        // new ClassName(args) -> AST_NEW(token=ClassName, children=args)
        reaAdvance(p); // consume 'new'
        if (p->current.type != REA_TOKEN_IDENTIFIER) return NULL;
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *clsTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);
        reaAdvance(p); // consume class name
        AST *args = NULL;
        if (p->current.type == REA_TOKEN_LEFT_PAREN) {
            reaAdvance(p);
            args = newASTNode(AST_COMPOUND, NULL);
            while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
                AST *arg = parseExpression(p);
                if (!arg) break;
                addChild(args, arg);
                if (p->current.type == REA_TOKEN_COMMA) reaAdvance(p); else break;
            }
            if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);
        }
        AST *node = newASTNode(AST_NEW, clsTok);
        if (args && args->child_count > 0) {
            node->children = args->children;
            node->child_count = args->child_count;
            node->child_capacity = args->child_capacity;
            for (int i = 0; i < node->child_count; i++) if (node->children[i]) node->children[i]->parent = node;
            args->children = NULL; args->child_count = 0; args->child_capacity = 0;
        }
        if (args) freeAST(args);
        setTypeAST(node, TYPE_POINTER);
        // Support chaining: new Class().method(...) or field access
        while (p->current.type == REA_TOKEN_DOT || p->current.type == REA_TOKEN_LEFT_BRACKET) {
            if (p->current.type == REA_TOKEN_LEFT_BRACKET) {
                node = parseArrayAccess(p, node);
                continue;
            }
            // DOT
            reaAdvance(p);
            if (p->current.type != REA_TOKEN_IDENTIFIER) break;
            char *f = (char*)malloc(p->current.length + 1);
            if (!f) break;
            memcpy(f, p->current.start, p->current.length);
            f[p->current.length] = '\0';
            Token *nameTok = newToken(TOKEN_IDENTIFIER, f, p->current.line, 0);
            free(f);
            reaAdvance(p);
            if (p->current.type == REA_TOKEN_LEFT_PAREN) {
                // method call on freshly constructed receiver
                reaAdvance(p); // consume '('
                AST *margs = newASTNode(AST_COMPOUND, NULL);
                while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
                    AST *arg = parseExpression(p);
                    if (!arg) break;
                    addChild(margs, arg);
                    if (p->current.type == REA_TOKEN_COMMA) reaAdvance(p); else break;
                }
                if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);
                AST *call = newASTNode(AST_PROCEDURE_CALL, nameTok);
                setLeft(call, node);
                addChild(call, node);
                if (margs && margs->child_count > 0) {
                    for (int i = 0; i < margs->child_count; i++) { addChild(call, margs->children[i]); margs->children[i] = NULL; }
                    margs->child_count = 0;
                }
                if (margs) freeAST(margs);
                node = call;
            } else {
                // field access
                AST *fieldVar = newASTNode(AST_VARIABLE, nameTok);
                AST *fa = newASTNode(AST_FIELD_ACCESS, nameTok);
                setLeft(fa, node);
                setRight(fa, fieldVar);
                node = fa;
            }
        }
        return node;
    }
    if (p->current.type == REA_TOKEN_NUMBER) {
        size_t len = p->current.length;
        const char *start = p->current.start;
        TokenType ttype = TOKEN_INTEGER_CONST;
        VarType vtype = TYPE_INT64;

        if (len > 2 && start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
            start += 2;
            len -= 2;
            ttype = TOKEN_HEX_CONST;
        } else {
            for (size_t i = 0; i < len; i++) {
                if (start[i] == '.' || start[i] == 'e' || start[i] == 'E') {
                    ttype = TOKEN_REAL_CONST;
                    vtype = TYPE_DOUBLE;
                    break;
                }
            }
        }

        char *lex = (char *)malloc(len + 1);
        if (!lex) return NULL;
        memcpy(lex, start, len);
        lex[len] = '\0';

        Token *tok = newToken(ttype, lex, p->current.line, 0);
        free(lex);
        AST *node = newASTNode(AST_NUMBER, tok);
        setTypeAST(node, vtype);
        reaAdvance(p);
        return node;
    } else if (p->current.type == REA_TOKEN_STRING) {
        size_t len = p->current.length;
        if (len < 2) return NULL;
        size_t inner_len = len - 2;
        size_t unesc_len = 0;
        char *lex = reaUnescapeString(p->current.start + 1, inner_len, &unesc_len);
        if (!lex) return NULL;
        Token *tok = (Token*)malloc(sizeof(Token));
        if (!tok) { free(lex); return NULL; }
        tok->type = TOKEN_STRING_CONST;
        tok->value = lex; // already NUL-terminated by reaUnescapeString
        tok->length = unesc_len;
        tok->line = p->current.line;
        tok->column = 0;
        AST *node = newASTNode(AST_STRING, tok);
        node->i_val = (int)unesc_len; // track literal length explicitly
        VarType vtype = (p->current.start[0] == '\'' && unesc_len == 1) ? TYPE_CHAR : TYPE_STRING;
        setTypeAST(node, vtype);
        reaAdvance(p);
        return node;
    } else if (p->current.type == REA_TOKEN_TRUE || p->current.type == REA_TOKEN_FALSE) {
        TokenType tt = (p->current.type == REA_TOKEN_TRUE) ? TOKEN_TRUE : TOKEN_FALSE;
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *tok = newToken(tt, lex, p->current.line, 0);
        free(lex);
        AST *node = newASTNode(AST_BOOLEAN, tok);
        setTypeAST(node, TYPE_BOOLEAN);
        node->i_val = (tt == TOKEN_TRUE) ? 1 : 0;
        reaAdvance(p);
        return node;
    } else if (p->current.type == REA_TOKEN_NIL) {
        Token *tok = newToken(TOKEN_NIL, "nil", p->current.line, 0);
        AST *node = newASTNode(AST_NIL, tok);
        setTypeAST(node, TYPE_NIL);
        freeToken(tok);
        reaAdvance(p);
        return node;
    } else if (p->current.type == REA_TOKEN_IDENTIFIER || p->current.type == REA_TOKEN_MYSELF) {
        size_t alloc_len = p->current.length;
        if (p->current.type == REA_TOKEN_MYSELF && alloc_len < 7) {
            alloc_len = 7; // ensure space for canonical "myself" lexeme
        }
        char *lex = (char *)malloc(alloc_len + 1);
        if (!lex) return NULL;
        if (p->current.type == REA_TOKEN_MYSELF) {
            strcpy(lex, "myself");
        } else {
            memcpy(lex, p->current.start, p->current.length);
            lex[p->current.length] = '\0';
        }
        bool isWriteBuiltin = (strcasecmp(lex, "writeln") == 0 || strcasecmp(lex, "write") == 0);
        bool isPrintf = (strcasecmp(lex, "printf") == 0);
        Token *tok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);
        reaAdvance(p); // consume identifier

        AST *call_args = NULL;
        AST *callTypeArgs = NULL;
        if (p->current.type == REA_TOKEN_LESS && looksLikeCallTypeArguments(p)) {
            reaAdvance(p); // consume '<'
            callTypeArgs = parseCallTypeArgumentList(p);
            if (!callTypeArgs && p->hadError) {
                freeToken(tok);
                return NULL;
            }
        }
        if (p->current.type == REA_TOKEN_LEFT_PAREN) {
            reaAdvance(p); // consume '('
            call_args = newASTNode(AST_COMPOUND, NULL);
            while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
                AST *arg = isWriteBuiltin ? parseWriteArgument(p) : parseExpression(p);
                if (!arg) break;
                addChild(call_args, arg);
                if (p->current.type == REA_TOKEN_COMMA) {
                    reaAdvance(p);
                } else {
                    break;
                }
            }
            if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
                reaAdvance(p);
            }
            AST *call = NULL;
            if (isPrintf) {
                call = newASTNode(AST_WRITE, NULL);
                transformPrintfArgs(p, call, call_args);
                if (call_args) freeAST(call_args);
                freeToken(tok);
                if (callTypeArgs) freeAST(callTypeArgs);
            } else {
                if (tok && tok->value && strcasecmp(tok->value, "writeln") == 0) {
                    call = newASTNode(AST_WRITELN, NULL);
                    freeToken(tok);
                } else if (tok && tok->value && strcasecmp(tok->value, "write") == 0) {
                    call = newASTNode(AST_WRITE, NULL);
                    freeToken(tok);
                } else {
                    call = newASTNode(AST_PROCEDURE_CALL, tok);
                }
                if (call_args && call_args->child_count > 0) {
                    call->children = call_args->children;
                    call->child_count = call_args->child_count;
                    call->child_capacity = call_args->child_capacity;
                    for (int i = 0; i < call->child_count; i++) {
                        if (call->children[i]) call->children[i]->parent = call;
                    }
                    call_args->children = NULL;
                    call_args->child_count = 0;
                    call_args->child_capacity = 0;
                }
                if (call_args) freeAST(call_args);
                if (callTypeArgs && call) {
                    call->extra = callTypeArgs;
                    callTypeArgs->parent = call;
                } else if (callTypeArgs) {
                    freeAST(callTypeArgs);
                }
            }
            setTypeAST(call, TYPE_UNKNOWN);
            // Support member access chaining after call (dots or brackets)
            AST *node = call;
            while (p->current.type == REA_TOKEN_DOT || p->current.type == REA_TOKEN_LEFT_BRACKET) {
                if (p->current.type == REA_TOKEN_LEFT_BRACKET) {
                    node = parseArrayAccess(p, node);
                    continue;
                }
                reaAdvance(p);
                if (p->current.type != REA_TOKEN_IDENTIFIER) break;
                // Capture method/field name
                char *f = (char*)malloc(p->current.length + 1);
                if (!f) break;
                memcpy(f, p->current.start, p->current.length);
                f[p->current.length] = '\0';
                Token *nameTok = newToken(TOKEN_IDENTIFIER, f, p->current.line, 0);
                free(f);
                reaAdvance(p);
                // If this is a qualified method call like receiver.method(...), build a call
                if (p->current.type == REA_TOKEN_LEFT_PAREN) {
                    // Parse call args
                    reaAdvance(p); // consume '('
                    AST *args = newASTNode(AST_COMPOUND, NULL);
                    while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
                        AST *arg = parseExpression(p);
                        if (!arg) break;
                        addChild(args, arg);
                        if (p->current.type == REA_TOKEN_COMMA) reaAdvance(p); else break;
                    }
                    if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);
                    // Build call node and prepend receiver as first arg
                    // Potentially mangle if receiver is 'myself'/`my` or freshly constructed 'new Class'
                    const char* cls = NULL;
                    if (node->type == AST_VARIABLE && node->token && node->token->value &&
                        (strcasecmp(node->token->value, "myself") == 0 ||
                         strcasecmp(node->token->value, "my") == 0)) {
                        cls = p->currentClassName;
                    } else if (node->type == AST_NEW && node->token && node->token->value) {
                        cls = node->token->value;
                    }
                    if (cls) {
                        size_t ln = strlen(cls) + 1 + strlen(nameTok->value) + 1;
                        char *m = (char*)malloc(ln);
                        if (m) {
                            snprintf(m, ln, "%s.%s", cls, nameTok->value);
                            free(nameTok->value);
                            nameTok->value = m;
                        }
                    }
                    AST *call = newASTNode(AST_PROCEDURE_CALL, nameTok);
                    // Receiver as qualifier and also as first argument
                    setLeft(call, node);
                    addChild(call, node);
                    if (args && args->child_count > 0) {
                        for (int i = 0; i < args->child_count; i++) {
                            addChild(call, args->children[i]);
                            args->children[i] = NULL;
                        }
                        args->child_count = 0;
                    }
                    if (args) freeAST(args);
                    node = call;
                } else {
                    // Simple field access
                    AST *fieldVar = newASTNode(AST_VARIABLE, nameTok);
                    AST *fa = newASTNode(AST_FIELD_ACCESS, nameTok);
                    setLeft(fa, node);
                    setRight(fa, fieldVar);
                    node = fa;
                }
            }
            return node;
        } else {
            AST *node = newASTNode(AST_VARIABLE, tok);
            setTypeAST(node, TYPE_UNKNOWN);
            if (callTypeArgs) {
                freeAST(callTypeArgs);
            }
            while (p->current.type == REA_TOKEN_DOT || p->current.type == REA_TOKEN_LEFT_BRACKET) {
                if (p->current.type == REA_TOKEN_LEFT_BRACKET) {
                    node = parseArrayAccess(p, node);
                    continue;
                }
                reaAdvance(p);
                if (p->current.type != REA_TOKEN_IDENTIFIER) break;
                char *f = (char*)malloc(p->current.length + 1);
                if (!f) break;
                memcpy(f, p->current.start, p->current.length);
                f[p->current.length] = '\0';
                Token *fieldTok = newToken(TOKEN_IDENTIFIER, f, p->current.line, 0);
                free(f);
                reaAdvance(p);
                if (p->current.type == REA_TOKEN_LEFT_PAREN) {
                    // Method call on arbitrary receiver: parse args and build call node with receiver first
                    reaAdvance(p); // consume '('
                    AST *args = newASTNode(AST_COMPOUND, NULL);
                    while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
                        AST *arg = parseExpression(p);
                        if (!arg) break;
                        addChild(args, arg);
                        if (p->current.type == REA_TOKEN_COMMA) reaAdvance(p); else break;
                    }
                    if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);
                    AST *call = newASTNode(AST_PROCEDURE_CALL, fieldTok);
                    setLeft(call, node);
                    addChild(call, node);
                    if (args && args->child_count > 0) {
                        for (int i = 0; i < args->child_count; i++) {
                            addChild(call, args->children[i]);
                            args->children[i] = NULL;
                        }
                        args->child_count = 0;
                    }
                    if (args) freeAST(args);
                    node = call;
                } else {
                    AST *fieldVar = newASTNode(AST_VARIABLE, fieldTok);
                    AST *fa = newASTNode(AST_FIELD_ACCESS, fieldTok);
                    setLeft(fa, node);
                    setRight(fa, fieldVar);
                    node = fa;
                }
            }
            return node;
        }
    } else if (p->current.type == REA_TOKEN_MINUS) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseFactor(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_MINUS, "-", op.line, 0);
        AST *node = newASTNode(AST_UNARY_OP, tok);
        setLeft(node, right);
        setTypeAST(node, right->var_type);
        return node;
    } else if (p->current.type == REA_TOKEN_BANG) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseFactor(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_NOT, "!", op.line, 0);
        AST *node = newASTNode(AST_UNARY_OP, tok);
        setLeft(node, right);
        setTypeAST(node, TYPE_BOOLEAN);
        return node;
    } else if (p->current.type == REA_TOKEN_LEFT_PAREN) {
        reaAdvance(p);
        AST *expr = parseExpression(p);
        if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
            reaAdvance(p);
        }
        return expr;
    }
    // Postfix increment/decrement: handled after primary
    // Parse primary failed
    return NULL;
}

static TokenType mapOp(ReaTokenType t) {
    switch (t) {
        case REA_TOKEN_PLUS: return TOKEN_PLUS;
        case REA_TOKEN_MINUS: return TOKEN_MINUS;
        case REA_TOKEN_STAR: return TOKEN_MUL;
        case REA_TOKEN_SLASH: return TOKEN_SLASH;
        case REA_TOKEN_PERCENT: return TOKEN_MOD;
        case REA_TOKEN_EQUAL_EQUAL: return TOKEN_EQUAL;
        case REA_TOKEN_BANG_EQUAL: return TOKEN_NOT_EQUAL;
        case REA_TOKEN_GREATER: return TOKEN_GREATER;
        case REA_TOKEN_GREATER_EQUAL: return TOKEN_GREATER_EQUAL;
        case REA_TOKEN_LESS: return TOKEN_LESS;
        case REA_TOKEN_LESS_EQUAL: return TOKEN_LESS_EQUAL;
        default: return TOKEN_UNKNOWN;
    }
}

static const char *opLexeme(TokenType t) {
    switch (t) {
        case TOKEN_PLUS: return "+";
        case TOKEN_MINUS: return "-";
        case TOKEN_MUL: return "*";
        case TOKEN_SLASH: return "/";
        case TOKEN_INT_DIV: return "/";
        case TOKEN_MOD: return "%";
        case TOKEN_EQUAL: return "==";
        case TOKEN_NOT_EQUAL: return "!=";
        case TOKEN_GREATER: return ">";
        case TOKEN_GREATER_EQUAL: return ">=";
        case TOKEN_LESS: return "<";
        case TOKEN_LESS_EQUAL: return "<=";
        case TOKEN_ASSIGN: return "=";
        default: return "";
    }
}

static AST *parseTerm(ReaParser *p) {
    AST *node = parseFactor(p);
    // Handle postfix ++/-- on the parsed primary/lvalue
    if (node && (node->type == AST_VARIABLE || node->type == AST_FIELD_ACCESS || node->type == AST_ARRAY_ACCESS)) {
        if (p->current.type == REA_TOKEN_PLUS_PLUS || p->current.type == REA_TOKEN_MINUS_MINUS) {
            int line = p->current.line;
            int is_inc = (p->current.type == REA_TOKEN_PLUS_PLUS);
            reaAdvance(p);
            // Build ( (node += 1) - 1 )  or ( (node -= 1) + 1 ) to preserve old value
            Token *oneTok = newToken(TOKEN_INTEGER_CONST, "1", line, 0);
            AST *oneNode = newASTNode(AST_NUMBER, oneTok);
            setTypeAST(oneNode, TYPE_INT64);
            oneNode->i_val = 1;
            Token *opTok = newToken(is_inc ? TOKEN_PLUS : TOKEN_MINUS, is_inc ? "+" : "-", line, 0);
            AST *assign = newASTNode(AST_ASSIGN, opTok);
            setLeft(assign, node);
            setRight(assign, oneNode);
            setTypeAST(assign, node->var_type);

            Token *wrapTok = newToken(is_inc ? TOKEN_MINUS : TOKEN_PLUS, is_inc ? "-" : "+", line, 0);
            AST *wrap = newASTNode(AST_BINARY_OP, wrapTok);
            setLeft(wrap, assign);
            // right is constant 1 again
            Token *oneTok2 = newToken(TOKEN_INTEGER_CONST, "1", line, 0);
            AST *oneNode2 = newASTNode(AST_NUMBER, oneTok2);
            setTypeAST(oneNode2, TYPE_INT64);
            oneNode2->i_val = 1;
            setRight(wrap, oneNode2);
            setTypeAST(wrap, node->var_type);
            node = wrap;
        }
    }
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_STAR ||
           p->current.type == REA_TOKEN_SLASH ||
           p->current.type == REA_TOKEN_PERCENT) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseFactor(p);
        if (!right) return NULL;
        VarType lt = node->var_type;
        VarType rt = right->var_type;
        TokenType tt = mapOp(op.type);
        if (tt == TOKEN_SLASH && isIntlikeType(lt) && isIntlikeType(rt)) {
            tt = TOKEN_INT_DIV;
        }
        Token *tok = newToken(tt, opLexeme(tt), op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        VarType res;
        if (tt == TOKEN_SLASH || lt == TYPE_DOUBLE || rt == TYPE_DOUBLE) {
            res = TYPE_DOUBLE;
        } else if (lt == TYPE_INT64 || rt == TYPE_INT64) {
            res = TYPE_INT64;
        } else {
            res = TYPE_INT32;
        }
        setTypeAST(bin, res);
        node = bin;
    }
    return node;
}

static AST *parseAdditive(ReaParser *p) {
    AST *node = parseTerm(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_PLUS || p->current.type == REA_TOKEN_MINUS) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseTerm(p);
        if (!right) return NULL;
        TokenType tt = mapOp(op.type);
        Token *tok = newToken(tt, opLexeme(tt), op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        VarType lt = node->var_type;
        VarType rt = right->var_type;
        VarType res;
        if (tt == TOKEN_PLUS && (lt == TYPE_STRING || rt == TYPE_STRING || lt == TYPE_CHAR || rt == TYPE_CHAR)) {
            res = TYPE_STRING;
        } else if (lt == TYPE_DOUBLE || rt == TYPE_DOUBLE) {
            res = TYPE_DOUBLE;
        } else if (lt == TYPE_INT64 || rt == TYPE_INT64) {
            res = TYPE_INT64;
        } else {
            res = TYPE_INT32;
        }
        setTypeAST(bin, res);
        node = bin;
    }
    return node;
}

static AST *parseShift(ReaParser *p) {
    AST *node = parseAdditive(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_SHIFT_LEFT ||
           p->current.type == REA_TOKEN_SHIFT_RIGHT) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseAdditive(p);
        if (!right) return NULL;
        TokenType tt = (op.type == REA_TOKEN_SHIFT_LEFT) ? TOKEN_SHL : TOKEN_SHR;
        Token *tok = newToken(tt, tt == TOKEN_SHL ? "<<" : ">>", op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        VarType lt = node->var_type;
        VarType rt = right->var_type;
        VarType res = (lt == TYPE_INT64 || rt == TYPE_INT64) ? TYPE_INT64 : TYPE_INT32;
        setTypeAST(bin, res);
        node = bin;
    }
    return node;
}

static AST *parseComparison(ReaParser *p) {
    AST *node = parseShift(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_GREATER || p->current.type == REA_TOKEN_GREATER_EQUAL ||
           p->current.type == REA_TOKEN_LESS || p->current.type == REA_TOKEN_LESS_EQUAL) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseShift(p);
        if (!right) return NULL;
        TokenType tt = mapOp(op.type);
        Token *tok = newToken(tt, opLexeme(tt), op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        setTypeAST(bin, TYPE_BOOLEAN);
        node = bin;
    }
    return node;
}

static AST *parseEquality(ReaParser *p) {
    AST *node = parseComparison(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_EQUAL_EQUAL || p->current.type == REA_TOKEN_BANG_EQUAL) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseComparison(p);
        if (!right) return NULL;
        TokenType tt = mapOp(op.type);
        Token *tok = newToken(tt, opLexeme(tt), op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        setTypeAST(bin, TYPE_BOOLEAN);
        node = bin;
    }
    return node;
}

static AST *parseBitwiseAnd(ReaParser *p) {
    AST *node = parseEquality(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_AND) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseEquality(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_AND, "&", op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        VarType lt = node->var_type;
        VarType rt = right->var_type;
        VarType res = (lt == TYPE_INT64 || rt == TYPE_INT64) ? TYPE_INT64 : TYPE_INT32;
        setTypeAST(bin, res);
        node = bin;
    }
    return node;
}

static AST *parseBitwiseXor(ReaParser *p) {
    AST *node = parseBitwiseAnd(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_XOR) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseBitwiseAnd(p);
        if (!right) return NULL;
        const char *lexeme = (strncmp(op.start, "xor", op.length) == 0) ? "xor" : "^";
        Token *tok = newToken(TOKEN_XOR, lexeme, op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        VarType lt = node->var_type;
        VarType rt = right->var_type;
        VarType res;
        if (lt == TYPE_BOOLEAN && rt == TYPE_BOOLEAN) {
            res = TYPE_BOOLEAN;
        } else if (lt == TYPE_INT64 || rt == TYPE_INT64) {
            res = TYPE_INT64;
        } else {
            res = TYPE_INT32;
        }
        setTypeAST(bin, res);
        node = bin;
    }
    return node;
}

static VarType promoteConditionalNumericType(VarType a, VarType b) {
    static const VarType order[] = {
        TYPE_LONG_DOUBLE,
        TYPE_DOUBLE,
        TYPE_FLOAT,
        TYPE_INT64,
        TYPE_UINT64,
        TYPE_INT32,
        TYPE_UINT32,
        TYPE_INT16,
        TYPE_UINT16,
        TYPE_INT8,
        TYPE_UINT8,
        TYPE_WORD,
        TYPE_BYTE
    };
    size_t count = sizeof(order) / sizeof(order[0]);
    for (size_t i = 0; i < count; i++) {
        VarType t = order[i];
        if (a == t || b == t) {
            return t;
        }
    }
    return TYPE_UNKNOWN;
}

static VarType resolveConditionalType(AST *thenExpr, AST *elseExpr, AST **typeDefSource) {
    VarType thenType = thenExpr ? thenExpr->var_type : TYPE_UNKNOWN;
    VarType elseType = elseExpr ? elseExpr->var_type : TYPE_UNKNOWN;

    if (thenType == elseType) {
        if (typeDefSource) {
            if (thenExpr && thenExpr->type_def) {
                *typeDefSource = thenExpr;
            } else if (elseExpr && elseExpr->type_def) {
                *typeDefSource = elseExpr;
            }
        }
        return thenType;
    }

    if (thenType == TYPE_UNKNOWN) {
        if (typeDefSource && elseExpr && elseExpr->type_def) {
            *typeDefSource = elseExpr;
        }
        return elseType;
    }
    if (elseType == TYPE_UNKNOWN) {
        if (typeDefSource && thenExpr && thenExpr->type_def) {
            *typeDefSource = thenExpr;
        }
        return thenType;
    }

    if ((thenType == TYPE_POINTER && elseType == TYPE_NIL) ||
        (thenType == TYPE_NIL && elseType == TYPE_POINTER)) {
        if (typeDefSource) {
            AST *source = (thenType == TYPE_POINTER) ? thenExpr : elseExpr;
            if (source && source->type_def) {
                *typeDefSource = source;
            }
        }
        return TYPE_POINTER;
    }

    if (thenType == TYPE_POINTER && elseType == TYPE_POINTER) {
        if (typeDefSource) {
            if (thenExpr && thenExpr->type_def) {
                *typeDefSource = thenExpr;
            } else if (elseExpr && elseExpr->type_def) {
                *typeDefSource = elseExpr;
            }
        }
        return TYPE_POINTER;
    }

    if (thenType == TYPE_STRING || elseType == TYPE_STRING ||
        (thenType == TYPE_CHAR && elseType == TYPE_STRING) ||
        (thenType == TYPE_STRING && elseType == TYPE_CHAR)) {
        return TYPE_STRING;
    }

    if (thenType == TYPE_CHAR && elseType == TYPE_CHAR) {
        return TYPE_CHAR;
    }

    if (thenType == TYPE_BOOLEAN && elseType == TYPE_BOOLEAN) {
        return TYPE_BOOLEAN;
    }

    VarType numeric = promoteConditionalNumericType(thenType, elseType);
    if (numeric != TYPE_UNKNOWN) {
        return numeric;
    }

    return thenType;
}

static AST *parseConditional(ReaParser *p) {
    AST *condition = parseLogicalOr(p);
    if (!condition) {
        return NULL;
    }

    if (p->current.type != REA_TOKEN_QUESTION) {
        return condition;
    }

    ReaToken question = p->current;
    reaAdvance(p); // consume '?'

    AST *thenBranch = parseAssignment(p);
    if (!thenBranch) {
        return NULL;
    }

    if (p->current.type != REA_TOKEN_COLON) {
        fprintf(stderr, "L%d: Expected ':' in conditional expression.\n", p->current.line);
        p->hadError = true;
        return NULL;
    }
    reaAdvance(p); // consume ':'

    AST *elseBranch = parseAssignment(p);
    if (!elseBranch) {
        return NULL;
    }

    Token *tok = newToken(TOKEN_IF, "?", question.line, 0);
    AST *node = newASTNode(AST_TERNARY, tok);
    setLeft(node, condition);
    setRight(node, thenBranch);
    setExtra(node, elseBranch);

    AST *typeDefSource = NULL;
    VarType resolved = resolveConditionalType(thenBranch, elseBranch, &typeDefSource);
    setTypeAST(node, resolved);
    if (typeDefSource && typeDefSource->type_def) {
        node->type_def = copyAST(typeDefSource->type_def);
    } else if (resolved == TYPE_POINTER) {
        if (thenBranch && thenBranch->type_def) {
            node->type_def = copyAST(thenBranch->type_def);
        } else if (elseBranch && elseBranch->type_def) {
            node->type_def = copyAST(elseBranch->type_def);
        }
    }

    return node;
}

static AST *parseAssignment(ReaParser *p) {
    // Highest precedence: handle assignment right-associatively
    AST *left = parseConditional(p);
    if (!left) return NULL;
    if ((left->type == AST_VARIABLE || left->type == AST_FIELD_ACCESS || left->type == AST_ARRAY_ACCESS) &&
        (p->current.type == REA_TOKEN_EQUAL || p->current.type == REA_TOKEN_PLUS_EQUAL || p->current.type == REA_TOKEN_MINUS_EQUAL)) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *value = parseAssignment(p);
        if (!value) return NULL;
        TokenType assignType = TOKEN_ASSIGN;
        if (op.type == REA_TOKEN_PLUS_EQUAL) assignType = TOKEN_PLUS;
        else if (op.type == REA_TOKEN_MINUS_EQUAL) assignType = TOKEN_MINUS;

        Token *assignTok = newToken(assignType, opLexeme(assignType), op.line, 0);
        AST *node = newASTNode(AST_ASSIGN, assignTok);
        setLeft(node, left);
        setRight(node, value);
        setTypeAST(node, left->var_type);
        return node;
    }
    return left;
}

static AST *parseExpression(ReaParser *p) {
    return parseAssignment(p);
}

static AST *parseBitwiseOr(ReaParser *p) {
    AST *node = parseBitwiseXor(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_OR) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseBitwiseXor(p);
        if (!right) return NULL;
        const char *lexeme = (strncmp(op.start, "or", op.length) == 0) ? "or" : "|";
        Token *tok = newToken(TOKEN_OR, lexeme, op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        VarType lt = node->var_type;
        VarType rt = right->var_type;
        VarType res;
        if (lt == TYPE_BOOLEAN && rt == TYPE_BOOLEAN) {
            res = TYPE_BOOLEAN;
        } else if (lt == TYPE_INT64 || rt == TYPE_INT64) {
            res = TYPE_INT64;
        } else {
            res = TYPE_INT32;
        }
        setTypeAST(bin, res);
        node = bin;
    }
    return node;
}

static AST *parseLogicalAnd(ReaParser *p) {
    AST *node = parseBitwiseOr(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_AND_AND) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseBitwiseOr(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_AND, "&&", op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        setTypeAST(bin, TYPE_BOOLEAN);
        node = bin;
    }
    return node;
}

static AST *parseLogicalOr(ReaParser *p) {
    AST *node = parseLogicalAnd(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_OR_OR) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseLogicalAnd(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_OR, "||", op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        setTypeAST(bin, TYPE_BOOLEAN);
        node = bin;
    }
    return node;
}

static VarType mapType(ReaTokenType t) {
    switch (t) {
        case REA_TOKEN_INT:
        case REA_TOKEN_INT64: return TYPE_INT64;
        case REA_TOKEN_INT32: return TYPE_INT32;
        case REA_TOKEN_INT16: return TYPE_INT16;
        case REA_TOKEN_INT8:  return TYPE_INT8;
        case REA_TOKEN_FLOAT: return TYPE_DOUBLE;
        case REA_TOKEN_FLOAT32: return TYPE_FLOAT;
        case REA_TOKEN_LONG_DOUBLE: return TYPE_LONG_DOUBLE;
        case REA_TOKEN_CHAR: return TYPE_CHAR;
        case REA_TOKEN_BYTE: return TYPE_BYTE;
        case REA_TOKEN_STR: return TYPE_STRING;
        case REA_TOKEN_TEXT: return TYPE_FILE;
        case REA_TOKEN_MSTREAM: return TYPE_MEMORYSTREAM;
        case REA_TOKEN_BOOL: return TYPE_BOOLEAN;
        case REA_TOKEN_VOID: return TYPE_VOID;
        default: return TYPE_VOID;
    }
}

static const char *typeName(ReaTokenType t) {
    switch (t) {
        case REA_TOKEN_INT: return "int";
        case REA_TOKEN_INT64: return "int64";
        case REA_TOKEN_INT32: return "int32";
        case REA_TOKEN_INT16: return "int16";
        case REA_TOKEN_INT8: return "int8";
        case REA_TOKEN_FLOAT: return "float";
        case REA_TOKEN_FLOAT32: return "float32";
        case REA_TOKEN_LONG_DOUBLE: return "longdouble";
        case REA_TOKEN_CHAR: return "char";
        case REA_TOKEN_BYTE: return "byte";
        case REA_TOKEN_STR: return "str";
        case REA_TOKEN_TEXT: return "text";
        case REA_TOKEN_MSTREAM: return "mstream";
        case REA_TOKEN_BOOL: return "bool";
        case REA_TOKEN_VOID: return "void";
        default: return "";
    }
}

static void parseTypeArgumentsIfPresent(ReaParser *p, AST *typeRef) {
    if (!typeRef || typeRef->type != AST_TYPE_REFERENCE) return;
    if (p->current.type != REA_TOKEN_LESS) return;
    reaAdvance(p); // consume '<'
    while (p->current.type != REA_TOKEN_GREATER && p->current.type != REA_TOKEN_EOF) {
        AST *argType = parsePointerParamType(p);
        if (!argType) {
            return;
        }
        addChild(typeRef, argType);
        if (p->current.type == REA_TOKEN_COMMA) {
            reaAdvance(p);
            continue;
        }
        break;
    }
    if (p->current.type != REA_TOKEN_GREATER) {
        fprintf(stderr, "L%d: Expected '>' to close type argument list.\n", p->current.line);
        p->hadError = true;
        return;
    }
    reaAdvance(p);
}

static AST *parseCallTypeArgumentList(ReaParser *p) {
    AST *list = newASTNode(AST_COMPOUND, NULL);
    while (p->current.type != REA_TOKEN_GREATER && p->current.type != REA_TOKEN_EOF) {
        AST *argType = parsePointerParamType(p);
        if (!argType) {
            freeAST(list);
            return NULL;
        }
        addChild(list, argType);
        if (p->current.type == REA_TOKEN_COMMA) {
            reaAdvance(p);
            continue;
        }
        break;
    }
    if (p->current.type != REA_TOKEN_GREATER) {
        fprintf(stderr, "L%d: Expected '>' to close call type argument list.\n", p->current.line);
        p->hadError = true;
        freeAST(list);
        return NULL;
    }
    reaAdvance(p);
    return list;
}

static bool tokenIsTypeKeyword(ReaTokenType t) {
    switch (t) {
        case REA_TOKEN_INT:
        case REA_TOKEN_INT64:
        case REA_TOKEN_INT32:
        case REA_TOKEN_INT16:
        case REA_TOKEN_INT8:
        case REA_TOKEN_FLOAT:
        case REA_TOKEN_FLOAT32:
        case REA_TOKEN_LONG_DOUBLE:
        case REA_TOKEN_CHAR:
        case REA_TOKEN_BYTE:
        case REA_TOKEN_STR:
        case REA_TOKEN_TEXT:
        case REA_TOKEN_MSTREAM:
        case REA_TOKEN_BOOL:
        case REA_TOKEN_VOID:
            return true;
        default:
            return false;
    }
}

static bool looksLikeCallTypeArguments(ReaParser *p) {
    if (p->current.type != REA_TOKEN_LESS) {
        return false;
    }
    ReaLexer saved = p->lexer;
    ReaToken next = reaNextToken(&saved);
    if (next.type == REA_TOKEN_GREATER) {
        // Allow empty type argument list, though unusual
    } else if (next.type != REA_TOKEN_IDENTIFIER && !tokenIsTypeKeyword(next.type)) {
        return false;
    }
    int depth = 1;
    ReaToken tok = next;
    while (depth > 0) {
        tok = reaNextToken(&saved);
        if (tok.type == REA_TOKEN_EOF) {
            return false;
        }
        if (tok.type == REA_TOKEN_LESS) {
            depth++;
        } else if (tok.type == REA_TOKEN_GREATER) {
            depth--;
        }
    }
    ReaToken after = reaNextToken(&saved);
    return after.type == REA_TOKEN_LEFT_PAREN;
}

static AST *parsePointerParamType(ReaParser *p) {
    if (p->current.type == REA_TOKEN_EOF) {
        return NULL;
    }

    ReaToken tok = p->current;
    VarType mapped = mapType(tok.type);

    if (tok.type == REA_TOKEN_VOID) {
        const char *name = typeName(tok.type);
        Token *typeTok = newToken(TOKEN_IDENTIFIER, name, tok.line, 0);
        AST *typeNode = newASTNode(AST_TYPE_IDENTIFIER, typeTok);
        setTypeAST(typeNode, TYPE_VOID);
        reaAdvance(p);
        return typeNode;
    }

    if (mapped != TYPE_VOID) {
        const char *name = typeName(tok.type);
        Token *typeTok = newToken(TOKEN_IDENTIFIER, name, tok.line, 0);
        AST *typeNode = newASTNode(AST_TYPE_IDENTIFIER, typeTok);
        setTypeAST(typeNode, mapped);
        reaAdvance(p);
        return typeNode;
    }

    if (tok.type == REA_TOKEN_IDENTIFIER) {
        char *lex = (char *)malloc(tok.length + 1);
        if (!lex) return NULL;
        memcpy(lex, tok.start, tok.length);
        lex[tok.length] = '\0';
        Token *typeTok = newToken(TOKEN_IDENTIFIER, lex, tok.line, 0);
        free(lex);
        if (isGenericTypeParam(p, typeTok->value)) {
            AST *genericNode = newASTNode(AST_TYPE_REFERENCE, typeTok);
            setTypeAST(genericNode, TYPE_UNKNOWN);
            reaAdvance(p);
            if (p->current.type == REA_TOKEN_LESS) {
                fprintf(stderr, "L%d: Generic parameter '%s' cannot specify type arguments.\n",
                        tok.line, typeTok->value);
                p->hadError = true;
            }
            return genericNode;
        }
        AST *refNode = newASTNode(AST_TYPE_REFERENCE, typeTok);
        setTypeAST(refNode, TYPE_RECORD);
        reaAdvance(p);
        parseTypeArgumentsIfPresent(p, refNode);
        return refNode;
    }

    fprintf(stderr, "L%d: expected type name in function pointer signature.\n", tok.line);
    p->hadError = true;
    return NULL;
}

static AST *parseFunctionPointerParamTypes(ReaParser *p) {
    if (p->current.type != REA_TOKEN_LEFT_PAREN) {
        fprintf(stderr, "L%d: expected '(' to begin function pointer signature.\n", p->current.line);
        p->hadError = true;
        return NULL;
    }

    reaAdvance(p); // consume '('
    AST *params = newASTNode(AST_COMPOUND, NULL);

    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        reaAdvance(p);
        return params;
    }

    if (p->current.type == REA_TOKEN_VOID) {
        int voidLine = p->current.line;
        reaAdvance(p);
        if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
            reaAdvance(p);
            return params; // no parameters
        }
        const char *name = typeName(REA_TOKEN_VOID);
        Token *typeTok = newToken(TOKEN_IDENTIFIER, name, voidLine, 0);
        AST *voidNode = newASTNode(AST_TYPE_IDENTIFIER, typeTok);
        setTypeAST(voidNode, TYPE_VOID);
        addChild(params, voidNode);
        if (p->current.type == REA_TOKEN_COMMA) {
            reaAdvance(p);
        }
    }

    while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
        AST *typeNode = parsePointerParamType(p);
        if (!typeNode) {
            freeAST(params);
            return NULL;
        }
        addChild(params, typeNode);
        if (p->current.type == REA_TOKEN_COMMA) {
            reaAdvance(p);
            continue;
        }
        break;
    }

    if (p->current.type != REA_TOKEN_RIGHT_PAREN) {
        fprintf(stderr, "L%d: expected ')' to close function pointer signature.\n", p->current.line);
        p->hadError = true;
        freeAST(params);
        return NULL;
    }
    reaAdvance(p);
    return params;
}

static AST *buildProcPointerType(AST *returnType, AST *paramList) {
    AST *procType = newASTNode(AST_PROC_PTR_TYPE, NULL);
    setTypeAST(procType, TYPE_POINTER);

    if (paramList) {
        if (paramList->child_count > 0) {
            procType->children = paramList->children;
            procType->child_count = paramList->child_count;
            procType->child_capacity = paramList->child_capacity;
            for (int i = 0; i < procType->child_count; i++) {
                if (procType->children[i]) {
                    procType->children[i]->parent = procType;
                }
            }
            paramList->children = NULL;
            paramList->child_count = 0;
            paramList->child_capacity = 0;
        }
        freeAST(paramList);
    }

    if (returnType) {
        
        setRight(procType, returnType);
    }
    return procType;
}

static AST *parsePointerVariableAfterName(ReaParser *p, Token *nameTok, AST *baseType) {
    if (p->current.type != REA_TOKEN_RIGHT_PAREN) {
        fprintf(stderr, "L%d: expected ')' after function pointer declarator.\n", p->current.line);
        p->hadError = true;
        return NULL;
    }
    reaAdvance(p); // consume ')'

    AST *paramTypes = parseFunctionPointerParamTypes(p);
    if (!paramTypes) {
        if (!p->hadError) {
            p->hadError = true;
        }
        return NULL;
    }

    AST *pointerType = buildProcPointerType(baseType, paramTypes);
    AST *varDecl = newASTNode(AST_VAR_DECL, NULL);
    AST *varNode = newASTNode(AST_VARIABLE, nameTok);
    setTypeAST(varNode, TYPE_POINTER);
    addChild(varDecl, varNode);
    setRight(varDecl, pointerType);
    setTypeAST(varDecl, TYPE_POINTER);

    if (p->current.type == REA_TOKEN_EQUAL) {
        reaAdvance(p);
        AST *init = parseExpression(p);
        setLeft(varDecl, init);
    }

    return varDecl;
}

static AST *parseVarDecl(ReaParser *p) {
    // Allow constructor shorthand: inside a class, the constructor may omit
    // the return type and start directly with the class name, e.g.:
    //   ClassName(arg1, arg2) { ... }
    // Detect this before normal type parsing by peeking at the next
    // non-whitespace character. If it is an opening parenthesis and the
    // identifier matches the current class name, treat it as a constructor
    // with an implicit void return type.
    if (p->current.type == REA_TOKEN_IDENTIFIER && p->currentClassName) {
        size_t len = p->current.length;
        if (strlen(p->currentClassName) == len &&
            strncasecmp(p->current.start, p->currentClassName, len) == 0) {
            size_t look = p->lexer.pos;
            const char *src = p->lexer.source;
            while (src[look] == ' ' || src[look] == '\t' ||
                   src[look] == '\r' || src[look] == '\n') {
                look++;
            }
            if (src[look] == '(') {
                char *lex = (char *)malloc(len + 1);
                if (!lex) return NULL;
                memcpy(lex, p->current.start, len);
                lex[len] = '\0';
                Token *nameTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
                free(lex);
                reaAdvance(p); // consume constructor name
                return parseFunctionDecl(p, nameTok, NULL, TYPE_VOID, -1, false);
            }
        }
    }

    ReaTokenType typeTok = p->current.type;
    VarType vtype = mapType(typeTok);

    AST *typeNode = NULL;
    if (typeTok == REA_TOKEN_VOID) {
        // Procedures: void name(...)
        reaAdvance(p); // consume 'void'
        if (p->current.type != REA_TOKEN_IDENTIFIER) return NULL;
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *nameTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);
        reaAdvance(p); // consume identifier
        if (p->current.type == REA_TOKEN_LEFT_PAREN) {
            int idx = p->currentClassName ? p->currentMethodIndex++ : -1;
            return parseFunctionDecl(p, nameTok, NULL, TYPE_VOID, idx, false);
        } else {
            fprintf(stderr, "L%d: Variable cannot have type void.\n", p->current.line);
            p->hadError = true;
            return NULL;
        }
    } else if (vtype != TYPE_VOID) {
        // Built-in type keyword
        const char *tname = typeName(typeTok);
        Token *typeToken = newToken(TOKEN_IDENTIFIER, tname, p->current.line, 0);
        typeNode = newASTNode(AST_TYPE_IDENTIFIER, typeToken);
        setTypeAST(typeNode, vtype);
        reaAdvance(p); // consume type keyword
    } else if (p->current.type == REA_TOKEN_IDENTIFIER) {
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *typeRefTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);

        bool isGeneric = isGenericTypeParam(p, typeRefTok->value);
        AST *resolvedType = isGeneric ? NULL : lookupType(typeRefTok->value);
        bool treatAsPointer = false;
        VarType resolvedVarType = TYPE_UNKNOWN;
        if (resolvedType) {
            resolvedVarType = resolvedType->var_type;
            if (resolvedType->type == AST_RECORD_TYPE ||
                resolvedVarType == TYPE_RECORD ||
                resolvedVarType == TYPE_POINTER) {
                treatAsPointer = true;
            } else {
                treatAsPointer = false;
            }
        } else if (!isGeneric) {
            resolvedVarType = TYPE_UNKNOWN;
        }

        if (treatAsPointer) {
            AST *refNode = newASTNode(AST_TYPE_REFERENCE, typeRefTok);
            setTypeAST(refNode, TYPE_RECORD);
            AST *ptrNode = newASTNode(AST_POINTER_TYPE, NULL);
            setTypeAST(ptrNode, TYPE_POINTER);
            setRight(ptrNode, refNode);
            typeNode = ptrNode;
            vtype = TYPE_POINTER;
        } else {
            AST *refNode = newASTNode(AST_TYPE_REFERENCE, typeRefTok);
            if (resolvedVarType == TYPE_VOID) resolvedVarType = TYPE_UNKNOWN;
            setTypeAST(refNode, resolvedVarType);
            typeNode = refNode;
            vtype = resolvedVarType;
        }

        if (resolvedType && !resolvedType->token) {
            freeAST(resolvedType);
        }

        reaAdvance(p); // consume type identifier
    } else {
        return NULL;
    }

    if (p->current.type == REA_TOKEN_LESS) {
        if (typeNode && typeNode->type == AST_TYPE_REFERENCE) {
            parseTypeArgumentsIfPresent(p, typeNode);
        } else if (typeNode && typeNode->type == AST_POINTER_TYPE && typeNode->right) {
            parseTypeArgumentsIfPresent(p, typeNode->right);
        }
    }

    if (p->current.type != REA_TOKEN_IDENTIFIER && p->current.type != REA_TOKEN_LEFT_PAREN) return NULL;

    AST *baseType = copyAST(typeNode); // copy uses original token pointers; keep until end
    AST *compound = newASTNode(AST_COMPOUND, NULL);
    if (compound) {
        compound->i_val = 1; // mark as declaration group wrapper
    }
    // Mark as global scope only when parsing at the top level
    compound->is_global_scope = (p->functionDepth == 0 && p->currentClassName == NULL);

    bool first = true;
    while (1) {
        bool handledPointer = false;
        if (p->current.type == REA_TOKEN_LEFT_PAREN) {
            ReaLexer savedLexer = p->lexer;
            ReaToken savedToken = p->current;
            reaAdvance(p); // consume '('
            if (p->current.type == REA_TOKEN_STAR) {
                reaAdvance(p); // consume '*'
                if (p->current.type == REA_TOKEN_IDENTIFIER) {
                    char *nameLex = (char *)malloc(p->current.length + 1);
                    if (!nameLex) { freeAST(compound); return NULL; }
                    memcpy(nameLex, p->current.start, p->current.length);
                    nameLex[p->current.length] = '\0';
                    Token *nameTok = newToken(TOKEN_IDENTIFIER, nameLex, p->current.line, 0);
                    free(nameLex);
                    reaAdvance(p); // consume identifier

                    AST *baseForPointer = first ? typeNode : copyAST(baseType);
                    if (!baseForPointer) {
                        freeAST(compound);
                        return NULL;
                    }

                    if (p->current.type == REA_TOKEN_LEFT_PAREN) {
                        int idxPtr = p->currentClassName ? p->currentMethodIndex++ : -1;
                        AST *func = parseFunctionDecl(p, nameTok, baseForPointer, vtype, idxPtr, true);
                        freeAST(compound);
                        return func;
                    }

                    AST *varDecl = parsePointerVariableAfterName(p, nameTok, baseForPointer);
                    if (!varDecl) {
                        freeAST(compound);
                        return NULL;
                    }
                    addChild(compound, varDecl);
                    handledPointer = true;
                    first = false;
                } else {
                    p->lexer = savedLexer;
                    p->current = savedToken;
                }
            } else {
                p->lexer = savedLexer;
                p->current = savedToken;
            }

            if (handledPointer) {
                if (p->current.type == REA_TOKEN_COMMA) {
                    reaAdvance(p);
                    continue;
                }
                break;
            }
        }

        if (p->current.type != REA_TOKEN_IDENTIFIER) break;

        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) { freeAST(compound); return NULL; }
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *nameTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);

        reaAdvance(p); // consume identifier

        VarType vtype_local = vtype;
        AST *varType = first ? typeNode : copyAST(baseType);
        if (p->current.type == REA_TOKEN_LEFT_BRACKET) {
            varType = parseArrayType(p, varType, &vtype_local);
        }

        if (first && (p->current.type == REA_TOKEN_LEFT_PAREN || p->current.type == REA_TOKEN_LESS)) {
            int idx2 = p->currentClassName ? p->currentMethodIndex++ : -1;
            freeAST(compound);
            return parseFunctionDecl(p, nameTok, varType, vtype_local, idx2, false);
        }

        AST *var = newASTNode(AST_VARIABLE, nameTok);
        setTypeAST(var, vtype_local);

        AST *init = NULL;
        if (p->current.type == REA_TOKEN_EQUAL) {
            reaAdvance(p);
            init = parseExpression(p);
        }

        AST *decl = newASTNode(AST_VAR_DECL, NULL);
        addChild(decl, var);
        setLeft(decl, init);
        setRight(decl, varType);
        setTypeAST(decl, vtype_local);
        addChild(compound, decl);

        if (p->current.type == REA_TOKEN_COMMA) {
            reaAdvance(p);
            if (p->current.type == REA_TOKEN_EOF) break;
            first = false;
            continue;
        }
        break;
    }

    if (p->current.type == REA_TOKEN_SEMICOLON) {
        reaAdvance(p);
    }

    // baseType shares token memory with typeNode; do not free to avoid double free
    if (compound->child_count == 1) {
        AST *only = compound->children[0];
        compound->children[0] = NULL;
        freeAST(compound);
        return only;
    }
    return compound;
}

static AST *parseFunctionDecl(ReaParser *p, Token *nameTok, AST *typeNode, VarType vtype, int methodIndex, bool pointerWrapped) {
    
    VarType prevType = p->currentFunctionType;
    int prevDepth = p->functionDepth;
    p->currentFunctionType = vtype;
    p->functionDepth++;

    // If inside a class, mangle function name to ClassName.method
    if (p->currentClassName && nameTok && nameTok->value) {
        size_t ln = strlen(p->currentClassName) + 1 + strlen(nameTok->value) + 1;
        char *m = (char*)malloc(ln);
        if (m) {
            snprintf(m, ln, "%s.%s", p->currentClassName, nameTok->value);
            free(nameTok->value);
            nameTok->value = m;
            nameTok->length = strlen(m); // keep token length in sync with new name
        }
    }

    AST *genericParams = NULL;
    bool genericFramePushed = false;
    if (p->current.type == REA_TOKEN_LESS) {
        genericFramePushed = true;
        pushGenericFrame(p);
        reaAdvance(p); // consume '<'
        genericParams = parseGenericParameterList(p);
        if (!genericParams) {
            popGenericFrame(p);
            p->currentFunctionType = prevType;
            p->functionDepth = prevDepth;
            return NULL;
        }
    }

    // Parse parameter list
    reaAdvance(p); // consume '('
    AST *params = newASTNode(AST_COMPOUND, NULL);
    // Inject implicit 'myself' parameter as first parameter when inside a class
    if (p->currentClassName) {
        Token *ptypeTok = newToken(TOKEN_IDENTIFIER, p->currentClassName, p->current.line, 0);
        AST *refNode = newASTNode(AST_TYPE_REFERENCE, ptypeTok);
        setTypeAST(refNode, TYPE_RECORD);
        AST *ptrNode = newASTNode(AST_POINTER_TYPE, NULL);
        setTypeAST(ptrNode, TYPE_POINTER);
        setRight(ptrNode, refNode);
        Token *selfTok = newToken(TOKEN_IDENTIFIER, "myself", p->current.line, 0);
        AST *selfVar = newASTNode(AST_VARIABLE, selfTok);
        setTypeAST(selfVar, TYPE_POINTER);
        AST *selfDecl = newASTNode(AST_VAR_DECL, NULL);
        addChild(selfDecl, selfVar);
        setRight(selfDecl, ptrNode);
        setTypeAST(selfDecl, TYPE_POINTER);
        addChild(params, selfDecl);
    }
    while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
        AST *ptypeNode = NULL;
        VarType pvtype = TYPE_VOID;
        if (p->current.type == REA_TOKEN_IDENTIFIER) {
            char *lex = (char *)malloc(p->current.length + 1);
            if (!lex) break;
            memcpy(lex, p->current.start, p->current.length);
            lex[p->current.length] = '\0';
            Token *typeRefTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
            free(lex);
            if (isGenericTypeParam(p, typeRefTok->value)) {
                AST *refNode = newASTNode(AST_TYPE_REFERENCE, typeRefTok);
                setTypeAST(refNode, TYPE_UNKNOWN);
                ptypeNode = refNode;
                pvtype = TYPE_UNKNOWN;
                reaAdvance(p); // consume type identifier
                if (p->current.type == REA_TOKEN_LESS) {
                    fprintf(stderr, "L%d: Generic parameter '%s' cannot specify type arguments.\n",
                            typeRefTok->line, typeRefTok->value);
                    p->hadError = true;
                }
            } else {
                AST *refNode = newASTNode(AST_TYPE_REFERENCE, typeRefTok);
                setTypeAST(refNode, TYPE_RECORD);
                reaAdvance(p); // consume type identifier
                parseTypeArgumentsIfPresent(p, refNode);
                AST *ptrNode = newASTNode(AST_POINTER_TYPE, NULL);
                setTypeAST(ptrNode, TYPE_POINTER);
                setRight(ptrNode, refNode);
                ptypeNode = ptrNode;
                pvtype = TYPE_POINTER;
            }
        } else {
            ReaTokenType paramTypeTok = p->current.type;
            pvtype = mapType(paramTypeTok);
            const char *ptname = typeName(paramTypeTok);
            Token *ptypeTok = newToken(TOKEN_IDENTIFIER, ptname, p->current.line, 0);
            ptypeNode = newASTNode(AST_TYPE_IDENTIFIER, ptypeTok);
            setTypeAST(ptypeNode, pvtype);
            reaAdvance(p); // consume param type
        }

        if (p->current.type != REA_TOKEN_IDENTIFIER) break;
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) break;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *paramNameTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);
        AST *paramVar = newASTNode(AST_VARIABLE, paramNameTok);
        setTypeAST(paramVar, pvtype);
        reaAdvance(p); // consume param name

        bool duplicateParam = false;
        for (int pi = 0; pi < params->child_count && !duplicateParam; pi++) {
            AST *existingDecl = params->children[pi];
            if (!existingDecl || existingDecl->type != AST_VAR_DECL) continue;
            for (int pj = 0; pj < existingDecl->child_count; pj++) {
                AST *existingVar = existingDecl->children[pj];
                if (!existingVar || !existingVar->token || !existingVar->token->value) continue;
                if (strcasecmp(existingVar->token->value, paramNameTok->value) == 0) {
                    fprintf(stderr, "L%d: duplicate parameter '%s' in function declaration.\n",
                            paramNameTok->line, paramNameTok->value);
                    p->hadError = true;
                    duplicateParam = true;
                    break;
                }
            }
        }

        AST *paramDecl = newASTNode(AST_VAR_DECL, NULL);
        addChild(paramDecl, paramVar);
        setRight(paramDecl, ptypeNode);
        setTypeAST(paramDecl, pvtype);

        if (p->current.type == REA_TOKEN_EQUAL) {
            reaAdvance(p); // consume '='
            AST *defaultExpr = parseExpression(p);
            if (defaultExpr) {
                setLeft(paramDecl, defaultExpr);
            } else {
                p->hadError = true;
            }
        }

        addChild(params, paramDecl);

        if (p->current.type == REA_TOKEN_COMMA) {
            reaAdvance(p);
        } else {
            break;
        }
    }
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        reaAdvance(p);
    }

    if (pointerWrapped) {
        if (p->current.type != REA_TOKEN_RIGHT_PAREN) {
            fprintf(stderr, "L%d: expected ')' after function pointer parameters.\n", p->current.line);
            p->hadError = true;
            freeAST(params);
            return NULL;
        }
        reaAdvance(p); // consume pointer-closing ')'
        AST *pointerParams = parseFunctionPointerParamTypes(p);
        if (!pointerParams) {
            if (!p->hadError) {
                p->hadError = true;
            }
            freeAST(params);
            return NULL;
        }
        typeNode = buildProcPointerType(typeNode, pointerParams);
        vtype = TYPE_POINTER;
    }

    // Parse function body. Previously the parser split variable declarations
    // from executable statements and wrapped them into an `AST_BLOCK` with two
    // children (declarations and statements).  This caused any variable
    // initializer with side effects—such as `spawn worker()`—to be emitted
    // before preceding statements when compiled, leading to confusing behavior
    // and runtime errors.  Instead, parse the body as a simple compound block
    // preserving the original statement order so initializers execute where
    // they appear in source.
    HashTable *outer_proc_table = current_procedure_table;
    HashTable *local_proc_table = NULL;
    AST *block = NULL;
    bool has_body = false;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        local_proc_table = pushProcedureTable();
        block = parseBlock(p);  // consumes braces and returns AST_COMPOUND
        has_body = true;
        if (local_proc_table) {
            popProcedureTable(false);
        }
    } else if (p->current.type == REA_TOKEN_SEMICOLON) {
        reaAdvance(p); // consume forward declaration terminator
    }

    AST *func = (vtype == TYPE_VOID) ? newASTNode(AST_PROCEDURE_DECL, nameTok)
                                     : newASTNode(AST_FUNCTION_DECL, nameTok);
    if (methodIndex >= 0) {
        func->is_virtual = true;
        func->i_val = methodIndex;
    }
    if (params->child_count > 0) {
        func->children = params->children;
        func->child_count = params->child_count;
        func->child_capacity = params->child_capacity;
        for (int i = 0; i < func->child_count; i++) {
            if (func->children[i]) func->children[i]->parent = func;
        }
        params->children = NULL;
        params->child_count = 0;
        params->child_capacity = 0;
    }
    freeAST(params);
    if (vtype == TYPE_VOID) {
        // Procedures carry body in right; no return type node
        setRight(func, block);
    } else {
        setRight(func, typeNode);
        setExtra(func, block);
    }
    if (genericParams) {
        setLeft(func, genericParams);
    }
    if (genericFramePushed) {
        popGenericFrame(p);
    }

    if (local_proc_table) {
        func->symbol_table = (Symbol*)local_proc_table;
    }
    setTypeAST(func, vtype);
    

    

    // Register function in procedure table (reuse existing entry for forward declarations)
    char lower_name[MAX_SYMBOL_LENGTH];
    strncpy(lower_name, nameTok->value, sizeof(lower_name) - 1);
    lower_name[sizeof(lower_name) - 1] = '\0';
    for (int i = 0; lower_name[i]; i++) lower_name[i] = (char)tolower((unsigned char)lower_name[i]);

    HashTable *target_table = outer_proc_table ? outer_proc_table : procedure_table;

    char symbol_lookup_name[MAX_SYMBOL_LENGTH * 2 + 2];
    if (target_table == procedure_table && p->inModule && p->currentModuleName && *p->currentModuleName) {
        snprintf(symbol_lookup_name, sizeof(symbol_lookup_name), "%s.%s", p->currentModuleName, lower_name);
        symbol_lookup_name[sizeof(symbol_lookup_name) - 1] = '\0';
        toLowerString(symbol_lookup_name);
    } else {
        strncpy(symbol_lookup_name, lower_name, sizeof(symbol_lookup_name) - 1);
        symbol_lookup_name[sizeof(symbol_lookup_name) - 1] = '\0';
    }

    Symbol *sym = target_table ? hashTableLookup(target_table, symbol_lookup_name) : NULL;
    if (sym && sym->is_alias && sym->real_symbol) {
        sym = sym->real_symbol;
    }

    bool sym_is_new = false;
    if (!sym) {
        sym = (Symbol*)calloc(1, sizeof(Symbol));
        if (sym) {
            sym->name = strdup(symbol_lookup_name);
            sym_is_new = true;
            if (target_table) {
                hashTableInsert(target_table, sym);
            }
        }
    }
    if (sym) {
        sym->type = vtype;
        if (sym->type_def) {
            freeAST(sym->type_def);
        }
        sym->type_def = copyAST(func);
        if (!has_body) {
            sym->is_defined = false;
        }
    }

    // If inside a class, also add a bare-name alias so 'obj.method(...)' can resolve.
    if (p->currentClassName && sym && sym_is_new && sym->name) {
        const char* dot = strrchr(sym->name, '.');
        const char* bare = NULL;
        if (dot && *(dot + 1)) bare = dot + 1;
        if (bare) {
            Symbol* alias2 = (Symbol*)calloc(1, sizeof(Symbol));
            if (alias2) {
                alias2->name = strdup(bare);
                alias2->is_alias = true;
                alias2->real_symbol = sym;
                alias2->type = vtype;
                alias2->type_def = copyAST(sym->type_def);
                if (target_table) {
                    hashTableInsert(target_table, alias2);
                }
            }
        }
    }

    // If this is a constructor (method name equals class name), add alias 'ClassName' -> real symbol
    if (p->currentClassName && sym_is_new && nameTok && nameTok->value) {
        const char* dot = strchr(nameTok->value, '.');
        if (dot) {
            size_t cls_len = (size_t)(dot - nameTok->value);
            if (strlen(p->currentClassName) == cls_len && strncasecmp(nameTok->value, p->currentClassName, cls_len) == 0) {
                const char* after = dot + 1;
                if (strncasecmp(after, p->currentClassName, cls_len) == 0 && after[cls_len] == '\0') {
                    Symbol* alias = (Symbol*)calloc(1, sizeof(Symbol));
                    if (alias) {
                        alias->name = strdup(p->currentClassName);
                        if (alias->name) {
                            for (int i = 0; alias->name[i]; i++) alias->name[i] = (char)tolower((unsigned char)alias->name[i]);
                        }
                        alias->is_alias = true;
                        alias->real_symbol = sym;
                        alias->type = vtype;
                        alias->type_def = sym ? copyAST(sym->type_def) : NULL;
                        if (target_table) {
                            hashTableInsert(target_table, alias);
                        }
                    }
                }
            }
        }
    }

    p->currentFunctionType = prevType;
    p->functionDepth = prevDepth;
    return func;
}

static AST *parseBlock(ReaParser *p) {
    if (p->current.type != REA_TOKEN_LEFT_BRACE) return NULL;
    reaAdvance(p); // consume '{'
    AST *block = newASTNode(AST_COMPOUND, NULL);
    
    while (p->current.type != REA_TOKEN_RIGHT_BRACE && p->current.type != REA_TOKEN_EOF) {
        AST *stmt = parseStatement(p);
        if (!stmt) break;
        addChild(block, stmt);
    }
    if (p->current.type == REA_TOKEN_RIGHT_BRACE) {
        reaAdvance(p);
    }
    
    return block;
}

static AST *parseReturn(ReaParser *p) {
    ReaToken ret = p->current;
    reaAdvance(p); // consume 'return'
    AST *value = NULL;
    if (p->current.type != REA_TOKEN_SEMICOLON) {
        value = parseExpression(p);
    } else if (p->currentFunctionType != TYPE_VOID) {
        fprintf(stderr, "L%d: return requires a value.\n", ret.line);
        p->hadError = true;
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        reaAdvance(p);
    }
    if (p->hadError) return NULL;
    Token *retTok = newToken(TOKEN_RETURN, "return", ret.line, 0);
    AST *node = newASTNode(AST_RETURN, retTok);
    setLeft(node, value);
    if (value) setTypeAST(node, value->var_type); else setTypeAST(node, TYPE_VOID);
    return node;
}

static AST *parseBreak(ReaParser *p) {
    reaAdvance(p); // consume 'break'
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        reaAdvance(p);
    }
    return newASTNode(AST_BREAK, NULL);
}

static AST *parseJoin(ReaParser *p) {
    reaAdvance(p); // consume 'join'
    AST *expr = parseExpression(p);
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        reaAdvance(p);
    }
    AST *node = newThreadJoin(expr);
    return node;
}

static AST *parseIf(ReaParser *p) {
    reaAdvance(p); // consume 'if'
    if (p->current.type == REA_TOKEN_LEFT_PAREN) {
        reaAdvance(p);
    }
    AST *condition = parseExpression(p);
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        reaAdvance(p);
    }

    AST *thenBranch = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        thenBranch = parseBlock(p);
    } else {
        thenBranch = parseStatement(p);
    }

    AST *elseBranch = NULL;
    if (p->current.type == REA_TOKEN_ELSE) {
        reaAdvance(p);
        if (p->current.type == REA_TOKEN_LEFT_BRACE) {
            elseBranch = parseBlock(p);
        } else {
            elseBranch = parseStatement(p);
        }
    }

    AST *node = newASTNode(AST_IF, NULL);
    setLeft(node, condition);
    setRight(node, thenBranch);
    setExtra(node, elseBranch);
    return node;
}

static AST *parseWhile(ReaParser *p) {
    reaAdvance(p); // consume 'while'
    if (p->current.type == REA_TOKEN_LEFT_PAREN) reaAdvance(p);
    AST *condition = parseExpression(p);
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);
    AST *body = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) body = parseBlock(p);
    else body = parseStatement(p);

    AST *node = newASTNode(AST_WHILE, NULL);
    setLeft(node, condition);
    setRight(node, body);
    return node;
}

static AST *parseDoWhile(ReaParser *p) {
    reaAdvance(p); // consume 'do'
    AST *body = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) body = parseBlock(p);
    else body = parseStatement(p);
    if (p->current.type == REA_TOKEN_WHILE) reaAdvance(p);
    if (p->current.type == REA_TOKEN_LEFT_PAREN) reaAdvance(p);
    AST *cond = parseExpression(p);
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);
    if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);
    // Translate do { body } while (cond);  ==>  REPEAT body UNTIL !cond
    Token *notTok = newToken(TOKEN_NOT, "!", cond ? (cond->token ? cond->token->line : 0) : 0, 0);
    AST *notCond = newASTNode(AST_UNARY_OP, notTok);
    setLeft(notCond, cond);
    setTypeAST(notCond, TYPE_BOOLEAN);
    AST *node = newASTNode(AST_REPEAT, NULL);
    setLeft(node, body);
    setRight(node, notCond);
    return node;
}

static AST *parseFor(ReaParser *p) {
    reaAdvance(p); // consume 'for'
    if (p->current.type == REA_TOKEN_LEFT_PAREN) reaAdvance(p);
    // init (may be var decl or expr) and ends with ';'
    AST *init = NULL;
    if (p->current.type == REA_TOKEN_INT || p->current.type == REA_TOKEN_INT64 ||
        p->current.type == REA_TOKEN_INT32 || p->current.type == REA_TOKEN_INT16 ||
        p->current.type == REA_TOKEN_INT8 || p->current.type == REA_TOKEN_FLOAT ||
        p->current.type == REA_TOKEN_FLOAT32 || p->current.type == REA_TOKEN_LONG_DOUBLE ||
        p->current.type == REA_TOKEN_CHAR || p->current.type == REA_TOKEN_BYTE ||
        p->current.type == REA_TOKEN_STR || p->current.type == REA_TOKEN_TEXT ||
        p->current.type == REA_TOKEN_MSTREAM || p->current.type == REA_TOKEN_BOOL) {
        init = parseVarDecl(p);
    } else if (p->current.type != REA_TOKEN_SEMICOLON) {
        AST *ie = parseExpression(p);
        if (ie) {
            AST *stmt = newASTNode(AST_EXPR_STMT, ie->token);
            setLeft(stmt, ie);
            init = stmt;
        }
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);

    // condition (optional)
    AST *cond = NULL;
    if (p->current.type != REA_TOKEN_SEMICOLON) {
        cond = parseExpression(p);
    } else {
        Token *tt = newToken(TOKEN_TRUE, "true", p->current.line, 0);
        AST *b = newASTNode(AST_BOOLEAN, tt);
        b->i_val = 1;
        setTypeAST(b, TYPE_BOOLEAN);
        cond = b;
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);

    // post (optional) until ')'
    AST *post = NULL;
    if (p->current.type != REA_TOKEN_RIGHT_PAREN) {
        AST *pe = parseExpression(p);
        if (pe) {
            AST *stmt = newASTNode(AST_EXPR_STMT, pe->token);
            setLeft(stmt, pe);
            post = stmt;
        }
    }
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);

    AST *body = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) body = parseBlock(p);
    else body = parseStatement(p);

    if (post) {
        body = rewriteContinueWithPost(body, post);
    }

    AST *whileBody = NULL;
    if (post) {
        whileBody = newASTNode(AST_COMPOUND, NULL);
        addChild(whileBody, body);
        addChild(whileBody, post);
    } else {
        whileBody = body;
    }
    AST *whileNode = newASTNode(AST_WHILE, NULL);
    setLeft(whileNode, cond);
    setRight(whileNode, whileBody);

    if (init) {
        AST *outer = newASTNode(AST_COMPOUND, NULL);
        addChild(outer, init);
        addChild(outer, whileNode);
        return outer;
    } else {
        return whileNode;
    }
}

static AST *parseSwitch(ReaParser *p) {
    reaAdvance(p); // consume 'switch'
    if (p->current.type == REA_TOKEN_LEFT_PAREN) reaAdvance(p);
    AST *expr = parseExpression(p);
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);
    if (p->current.type != REA_TOKEN_LEFT_BRACE) return NULL;
    reaAdvance(p); // consume '{'
    AST *node = newASTNode(AST_CASE, NULL);
    setLeft(node, expr);
    while (p->current.type != REA_TOKEN_RIGHT_BRACE && p->current.type != REA_TOKEN_EOF) {
        if (p->current.type == REA_TOKEN_CASE) {
            reaAdvance(p);
            AST *labels = newASTNode(AST_COMPOUND, NULL);
            while (p->current.type != REA_TOKEN_COLON && p->current.type != REA_TOKEN_EOF) {
                AST *lbl = parseExpression(p);
                if (!lbl) break;
                addChild(labels, lbl);
                if (p->current.type == REA_TOKEN_COMMA) { reaAdvance(p); continue; }
                else break;
            }
            if (p->current.type == REA_TOKEN_COLON) reaAdvance(p);
            AST *body = newASTNode(AST_COMPOUND, NULL);
            while (p->current.type != REA_TOKEN_RIGHT_BRACE &&
                   p->current.type != REA_TOKEN_CASE &&
                   p->current.type != REA_TOKEN_DEFAULT &&
                   p->current.type != REA_TOKEN_EOF) {
                if (p->current.type == REA_TOKEN_BREAK) {
                    reaAdvance(p);
                    if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);
                    break;
                }
                AST *s = parseStatement(p);
                if (!s) break;
                addChild(body, s);
            }
            AST *branch = newASTNode(AST_CASE_BRANCH, NULL);
            if (labels->child_count == 1) {
                AST *only = labels->children[0];
                only->parent = NULL;
                free(labels->children);
                free(labels);
                setLeft(branch, only);
            } else {
                setLeft(branch, labels);
            }
            setRight(branch, body);
            addChild(node, branch);
        } else if (p->current.type == REA_TOKEN_DEFAULT) {
            reaAdvance(p);
            if (p->current.type == REA_TOKEN_COLON) reaAdvance(p);
            AST *body = newASTNode(AST_COMPOUND, NULL);
            while (p->current.type != REA_TOKEN_RIGHT_BRACE &&
                   p->current.type != REA_TOKEN_CASE &&
                   p->current.type != REA_TOKEN_DEFAULT &&
                   p->current.type != REA_TOKEN_EOF) {
                if (p->current.type == REA_TOKEN_BREAK) {
                    reaAdvance(p);
                    if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);
                    break;
                }
                AST *s = parseStatement(p);
                if (!s) break;
                addChild(body, s);
            }
            setExtra(node, body);
        } else {
            reaAdvance(p);
        }
    }
    if (p->current.type == REA_TOKEN_RIGHT_BRACE) reaAdvance(p);
    return node;
}

static AST *ensureCompound(AST *node) {
    if (!node) return NULL;
    if (node->type == AST_COMPOUND) {
        return node;
    }
    AST *block = newASTNode(AST_COMPOUND, NULL);
    addChild(block, node);
    return block;
}

static AST *parseMatchPattern(ReaParser *p);
static AST *parseMatchCase(ReaParser *p);

static AST *parseTuplePattern(ReaParser *p) {
    int startLine = p->current.line;
    reaAdvance(p); // consume '('
    AST *list = newASTNode(AST_LIST, NULL);
    const char **names = NULL;
    int nameCount = 0;
    int nameCap = 0;
    while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
        if (p->current.type == REA_TOKEN_IDENTIFIER) {
            char *lex = (char *)malloc(p->current.length + 1);
            if (!lex) {
                fprintf(stderr, "Memory allocation failure parsing tuple pattern.\n");
                EXIT_FAILURE_HANDLER();
            }
            memcpy(lex, p->current.start, p->current.length);
            lex[p->current.length] = '\0';
            Token *tok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
            free(lex);
            AST *binding = newASTNode(AST_PATTERN_BINDING, tok);
            addChild(list, binding);
            for (int i = 0; i < nameCount; i++) {
                if (strcasecmp(names[i], tok->value) == 0) {
                    fprintf(stderr, "L%d: duplicate pattern binding '%s'.\n", tok->line, tok->value);
                    p->hadError = true;
                    break;
                }
            }
            if (nameCount >= nameCap) {
                int newCap = nameCap ? nameCap * 2 : 4;
                const char **resized = (const char **)realloc(names, (size_t)newCap * sizeof(const char *));
                if (!resized) {
                    fprintf(stderr, "Memory allocation failure recording tuple pattern names.\n");
                    EXIT_FAILURE_HANDLER();
                }
                names = resized;
                nameCap = newCap;
            }
            names[nameCount++] = tok->value;
            reaAdvance(p);
        } else {
            AST *expr = parseExpression(p);
            if (expr) {
                addChild(list, expr);
            } else {
                break;
            }
        }
        if (p->current.type == REA_TOKEN_COMMA) {
            reaAdvance(p);
            continue;
        }
        break;
    }
    if (p->current.type != REA_TOKEN_RIGHT_PAREN) {
        fprintf(stderr, "L%d: expected ')' to close tuple pattern.\n", startLine);
        p->hadError = true;
    } else {
        reaAdvance(p);
    }
    free(names);
    return list;
}

static AST *parseMatchPattern(ReaParser *p) {
    if (p->current.type == REA_TOKEN_LEFT_PAREN) {
        return parseTuplePattern(p);
    }
    if (p->current.type == REA_TOKEN_IDENTIFIER) {
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) {
            fprintf(stderr, "Memory allocation failure parsing pattern binding.\n");
            EXIT_FAILURE_HANDLER();
        }
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *tok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);
        AST *binding = newASTNode(AST_PATTERN_BINDING, tok);
        reaAdvance(p);
        return binding;
    }
    return parseExpression(p);
}

static AST *parseMatchCase(ReaParser *p) {
    reaAdvance(p); // consume 'case'
    AST *pattern = parseMatchPattern(p);
    AST *guard = NULL;
    if (p->current.type == REA_TOKEN_IF) {
        reaAdvance(p);
        guard = parseExpression(p);
    }
    if (p->current.type != REA_TOKEN_ARROW) {
        fprintf(stderr, "L%d: expected '->' after match pattern.\n", p->current.line);
        p->hadError = true;
    } else {
        reaAdvance(p);
    }
    AST *body = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        body = parseBlock(p);
    } else {
        body = parseStatement(p);
    }
    body = ensureCompound(body);
    AST *branch = newASTNode(AST_MATCH_BRANCH, NULL);
    setLeft(branch, pattern);
    setExtra(branch, guard);
    setRight(branch, body);
    return branch;
}

static AST *parseMatch(ReaParser *p) {
    reaAdvance(p); // consume 'match'
    AST *expr = parseExpression(p);
    if (!expr) {
        return NULL;
    }
    if (p->current.type != REA_TOKEN_LEFT_BRACE) {
        fprintf(stderr, "L%d: expected '{' to begin match body.\n", p->current.line);
        p->hadError = true;
        return expr;
    }
    reaAdvance(p); // consume '{'
    AST *node = newASTNode(AST_MATCH, NULL);
    setLeft(node, expr);
    bool sawDefault = false;
    while (p->current.type != REA_TOKEN_RIGHT_BRACE && p->current.type != REA_TOKEN_EOF) {
        if (p->current.type == REA_TOKEN_CASE) {
            AST *branch = parseMatchCase(p);
            if (branch) {
                addChild(node, branch);
            }
            continue;
        }
        if (p->current.type == REA_TOKEN_DEFAULT) {
            if (sawDefault) {
                fprintf(stderr, "L%d: multiple default branches in match statement.\n", p->current.line);
                p->hadError = true;
            }
            reaAdvance(p);
            if (p->current.type != REA_TOKEN_ARROW) {
                fprintf(stderr, "L%d: expected '->' after default label.\n", p->current.line);
                p->hadError = true;
            } else {
                reaAdvance(p);
            }
            AST *body = NULL;
            if (p->current.type == REA_TOKEN_LEFT_BRACE) {
                body = parseBlock(p);
            } else {
                body = parseStatement(p);
            }
            setExtra(node, ensureCompound(body));
            sawDefault = true;
            continue;
        }
        // Skip unexpected tokens to avoid infinite loop.
        fprintf(stderr, "L%d: unexpected token in match body.\n", p->current.line);
        p->hadError = true;
        reaAdvance(p);
    }
    if (p->current.type != REA_TOKEN_RIGHT_BRACE) {
        fprintf(stderr, "L%d: expected '}' to close match statement.\n", p->current.line);
        p->hadError = true;
    } else {
        reaAdvance(p);
    }
    return node;
}

static AST *parseThrow(ReaParser *p) {
    int line = p->current.line;
    reaAdvance(p); // consume 'throw'
    AST *expr = parseExpression(p);
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        reaAdvance(p);
    }
    AST *node = newASTNode(AST_THROW, NULL);
    setLeft(node, expr);
    if (expr) {
        setTypeAST(node, expr->var_type);
    } else {
        setTypeAST(node, TYPE_VOID);
    }
    node->i_val = line;
    return node;
}

static AST *parseTry(ReaParser *p) {
    reaAdvance(p); // consume 'try'
    if (p->current.type != REA_TOKEN_LEFT_BRACE) {
        fprintf(stderr, "L%d: expected '{' after try keyword.\n", p->current.line);
        p->hadError = true;
        return NULL;
    }
    AST *tryBlock = parseBlock(p);
    if (p->current.type != REA_TOKEN_CATCH) {
        fprintf(stderr, "L%d: expected catch clause for try block.\n", p->current.line);
        p->hadError = true;
        return tryBlock;
    }
    reaAdvance(p); // consume 'catch'
    if (p->current.type != REA_TOKEN_LEFT_PAREN) {
        fprintf(stderr, "L%d: expected '(' after catch.\n", p->current.line);
        p->hadError = true;
        return tryBlock;
    }
    reaAdvance(p);
    AST *typeNode = parsePointerParamType(p);
    if (!typeNode) {
        // Default to int64 when parsing fails so later stages can continue.
        Token *tok = newToken(TOKEN_IDENTIFIER, "int", p->current.line, 0);
        typeNode = newASTNode(AST_TYPE_IDENTIFIER, tok);
        setTypeAST(typeNode, TYPE_INT64);
    }
    if (p->current.type != REA_TOKEN_IDENTIFIER) {
        fprintf(stderr, "L%d: expected identifier for catch variable.\n", p->current.line);
        p->hadError = true;
        return tryBlock;
    }
    char *lex = (char *)malloc(p->current.length + 1);
    if (!lex) {
        fprintf(stderr, "Memory allocation failure parsing catch variable.\n");
        EXIT_FAILURE_HANDLER();
    }
    memcpy(lex, p->current.start, p->current.length);
    lex[p->current.length] = '\0';
    Token *nameTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
    free(lex);
    reaAdvance(p);
    if (p->current.type != REA_TOKEN_RIGHT_PAREN) {
        fprintf(stderr, "L%d: expected ')' after catch binding.\n", p->current.line);
        p->hadError = true;
    } else {
        reaAdvance(p);
    }
    AST *catchBody = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        catchBody = parseBlock(p);
    } else {
        catchBody = parseStatement(p);
    }
    catchBody = ensureCompound(catchBody);
    AST *varNode = newASTNode(AST_VARIABLE, nameTok);
    setTypeAST(varNode, typeNode ? typeNode->var_type : TYPE_INT64);
    AST *catchDecl = newASTNode(AST_VAR_DECL, NULL);
    addChild(catchDecl, varNode);
    setRight(catchDecl, typeNode);
    setTypeAST(catchDecl, typeNode ? typeNode->var_type : TYPE_INT64);
    AST *catchNode = newASTNode(AST_CATCH, NULL);
    setLeft(catchNode, catchDecl);
    setRight(catchNode, catchBody);
    AST *tryNode = newASTNode(AST_TRY, NULL);
    setLeft(tryNode, tryBlock);
    setRight(tryNode, catchNode);
    return tryNode;
}

static AST *parseConstDecl(ReaParser *p) {
    reaAdvance(p); // consume 'const'
    AST *typeNode = NULL;
    VarType vtype = TYPE_UNKNOWN;
    if (p->current.type != REA_TOKEN_IDENTIFIER) {
        ReaTokenType typeTok = p->current.type;
        vtype = mapType(typeTok);
        const char *tname = typeName(typeTok);
        Token *ttok = newToken(TOKEN_IDENTIFIER, tname, p->current.line, 0);
        typeNode = newASTNode(AST_TYPE_IDENTIFIER, ttok);
        setTypeAST(typeNode, vtype);
        reaAdvance(p); // consume type keyword
    }
    if (p->current.type != REA_TOKEN_IDENTIFIER) return NULL;
    char *lex = (char *)malloc(p->current.length + 1);
    if (!lex) return NULL;
    memcpy(lex, p->current.start, p->current.length);
    lex[p->current.length] = '\0';
    Token *nameTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
    free(lex);
    reaAdvance(p); // consume name
    if (p->current.type != REA_TOKEN_EQUAL) return NULL;
    reaAdvance(p);
    AST *value = parseExpression(p);
    if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);
    AST *node = newASTNode(AST_CONST_DECL, nameTok);
    setLeft(node, value);
    if (typeNode) setRight(node, typeNode);
    if (value) setTypeAST(node, value->var_type);
    if (value) {
        Value v = evaluateCompileTimeValue(value);
        if (v.type != TYPE_VOID && v.type != TYPE_UNKNOWN) {
            if (p->functionDepth == 0) {
                addCompilerConstant(nameTok->value, &v, nameTok->line);
            }
            if (!typeNode) setTypeAST(node, v.type);
        }
        freeValue(&v);
    }
    return node;
}

static AST *parseTypeAlias(ReaParser *p) {
    int line = p->current.line;
    reaAdvance(p); // consume 'type'

    if (p->current.type != REA_TOKEN_ALIAS) {
        fprintf(stderr, "L%d: Expected 'alias' after 'type'.\n", line);
        p->hadError = true;
        return NULL;
    }
    reaAdvance(p); // consume 'alias'

    if (p->current.type != REA_TOKEN_IDENTIFIER) {
        int line = p->current.line;
        const char *lex = p->current.start;
        int len = (int)p->current.length;
        if (!lex || len <= 0) {
            fprintf(stderr, "L%d: type name is reserved.\n", line);
        } else {
            fprintf(stderr, "L%d: type name '%.*s' is reserved.\n", line, len, lex);
        }
        p->hadError = true;
        return NULL;
    }

    char *aliasName = (char *)malloc(p->current.length + 1);
    if (!aliasName) {
        fprintf(stderr, "Memory allocation failure while parsing type alias name.\n");
        p->hadError = true;
        return NULL;
    }
    memcpy(aliasName, p->current.start, p->current.length);
    aliasName[p->current.length] = '\0';
    Token *aliasTok = newToken(TOKEN_IDENTIFIER, aliasName, p->current.line, 0);
    free(aliasName);
    reaAdvance(p); // consume alias identifier

    AST *typeParams = NULL;
    bool framePushed = false;

    if (typeNameAlreadyDefined(aliasTok->value)) {
        fprintf(stderr, "L%d: type alias '%s' already defined.\n", aliasTok->line, aliasTok->value);
        p->hadError = true;
    } else {
        AST *existing = lookupType(aliasTok->value);
        if (existing) {
            if (!existing->token) {
                fprintf(stderr, "L%d: type name '%s' is reserved.\n", aliasTok->line, aliasTok->value);
            } else {
                fprintf(stderr, "L%d: type alias '%s' already defined.\n", aliasTok->line, aliasTok->value);
            }
            p->hadError = true;
            if (!existing->token) {
                freeAST(existing);
            }
        }
    }

    if (p->current.type == REA_TOKEN_LESS) {
        framePushed = true;
        pushGenericFrame(p);
        reaAdvance(p); // consume '<'
        typeParams = parseGenericParameterList(p);
        if (!typeParams) {
            popGenericFrame(p);
            return NULL;
        }
    }

    if (p->current.type != REA_TOKEN_EQUAL) {
        fprintf(stderr, "L%d: Expected '=' in type alias declaration.\n", p->current.line);
        p->hadError = true;
        if (framePushed) {
            popGenericFrame(p);
        }
        if (typeParams) freeAST(typeParams);
        return NULL;
    }
    reaAdvance(p); // consume '='

    AST *aliasType = parsePointerParamType(p);
    if (!aliasType) {
        if (!p->hadError) {
            fprintf(stderr, "L%d: Expected type after '=' in type alias.\n", p->current.line);
            p->hadError = true;
        }
        if (framePushed) {
            popGenericFrame(p);
        }
        if (typeParams) freeAST(typeParams);
        return NULL;
    }

    if (p->current.type == REA_TOKEN_SEMICOLON) {
        reaAdvance(p);
    }

    AST *typeDecl = newASTNode(AST_TYPE_DECL, aliasTok);
    setLeft(typeDecl, aliasType);
    setTypeAST(typeDecl, aliasType->var_type);
    if (typeParams && typeParams->child_count > 0) {
        typeDecl->extra = typeParams;
        typeParams->parent = typeDecl;
    } else if (typeParams) {
        freeAST(typeParams);
    }

    if (framePushed) {
        popGenericFrame(p);
    }

    if (!p->hadError) {
        insertType(aliasTok->value, aliasType);
    }
    return typeDecl;
}

static bool tokenIsKeyword(const ReaToken *tok, const char *keyword) {
    if (!tok || tok->type != REA_TOKEN_IDENTIFIER) return false;
    size_t len = strlen(keyword);
    if ((size_t)tok->length != len) return false;
    return strncasecmp(tok->start, keyword, len) == 0;
}

static AST *parseImport(ReaParser *p) {
    // Grammar:
    //   #import (Identifier | StringLiteral) [as Identifier] (',' ...)* ';'
    reaAdvance(p); // consume '#import'
    AST *uses = newASTNode(AST_USES_CLAUSE, NULL);

    bool parsed_any = false;
    while (p->current.type != REA_TOKEN_EOF) {
        char *path = NULL;
        int path_line = p->current.line;

        if (p->current.type == REA_TOKEN_IDENTIFIER) {
            path = (char*)malloc(p->current.length + 1);
            if (!path) break;
            memcpy(path, p->current.start, p->current.length);
            path[p->current.length] = '\0';
            reaAdvance(p);
        } else if (p->current.type == REA_TOKEN_STRING) {
            size_t len = p->current.length;
            if (len >= 2) {
                path = (char*)malloc(len - 1);
                if (!path) break;
                memcpy(path, p->current.start + 1, len - 2);
                path[len - 2] = '\0';
            }
            reaAdvance(p);
        } else {
            break;
        }

        char *alias = NULL;
        if (tokenIsKeyword(&p->current, "as")) {
            reaAdvance(p); // consume 'as'
            if (p->current.type != REA_TOKEN_IDENTIFIER) {
                fprintf(stderr, "L%d: Expected alias identifier after 'as'.\n", p->current.line);
                p->hadError = true;
            } else {
                alias = (char*)malloc(p->current.length + 1);
                if (!alias) {
                    free(path);
                    break;
                }
                memcpy(alias, p->current.start, p->current.length);
                alias[p->current.length] = '\0';
                reaAdvance(p);
            }
        }

        if (path) {
            Token *pathTok = newToken(TOKEN_STRING_CONST, path, path_line, 0);
            AST *importNode = newASTNode(AST_IMPORT, pathTok);
            if (alias) {
                Token *aliasTok = newToken(TOKEN_IDENTIFIER, alias, path_line, 0);
                AST *aliasNode = newASTNode(AST_VARIABLE, aliasTok);
                setLeft(importNode, aliasNode);
            }
            addChild(uses, importNode);
            free(path);
            if (alias) free(alias);
            parsed_any = true;
        }

        if (p->current.type == REA_TOKEN_COMMA) {
            reaAdvance(p);
            continue;
        }
        break;
    }

    if (p->current.type == REA_TOKEN_SEMICOLON) {
        reaAdvance(p);
    }

    if (!parsed_any) {
        fprintf(stderr, "L%d: Malformed #import directive.\n", p->current.line);
        p->hadError = true;
    }

    return uses;
}

static bool isExportableType(ASTNodeType type) {
    switch (type) {
        case AST_VAR_DECL:
        case AST_CONST_DECL:
        case AST_TYPE_DECL:
        case AST_FUNCTION_DECL:
        case AST_PROCEDURE_DECL:
            return true;
        default:
            return false;
    }
}

static void markExported(AST *node) {
    if (!node) return;
    if (node->type == AST_COMPOUND || node->type == AST_BLOCK) {
        if (node->left) markExported(node->left);
        if (node->right) markExported(node->right);
        if (node->extra) markExported(node->extra);
        for (int i = 0; i < node->child_count; i++) {
            markExported(node->children[i]);
        }
        return;
    }
    if (isExportableType(node->type)) {
        node->is_exported = true;
    }
}

static void appendModuleNode(AST *decls, AST *stmts, AST *node) {
    if (!node) return;
    if (node->type == AST_COMPOUND && node->is_global_scope) {
        for (int i = 0; i < node->child_count; i++) {
            AST *child = node->children[i];
            if (!child) continue;
            if (isExportableType(child->type) || child->type == AST_USES_CLAUSE || child->type == AST_IMPORT) {
                addChild(decls, child);
            } else {
                addChild(stmts, child);
            }
            node->children[i] = NULL;
        }
        freeAST(node);
        return;
    }

    if (isExportableType(node->type) || node->type == AST_USES_CLAUSE || node->type == AST_IMPORT || node->type == AST_MODULE) {
        addChild(decls, node);
    } else {
        addChild(stmts, node);
    }
}

static AST *parseModule(ReaParser *p) {
    int module_line = p->current.line;
    reaAdvance(p); // consume 'module'

    if (p->current.type != REA_TOKEN_IDENTIFIER) {
        fprintf(stderr, "L%d: Expected module name after 'module'.\n", p->current.line);
        p->hadError = true;
        return NULL;
    }

    char *name = (char*)malloc(p->current.length + 1);
    if (!name) return NULL;
    memcpy(name, p->current.start, p->current.length);
    name[p->current.length] = '\0';
    Token *nameTok = newToken(TOKEN_IDENTIFIER, name, module_line, 0);
    free(name);
    reaAdvance(p); // consume module name

    if (p->current.type != REA_TOKEN_LEFT_BRACE) {
        fprintf(stderr, "L%d: Expected '{' to begin module body.\n", p->current.line);
        p->hadError = true;
        return NULL;
    }
    reaAdvance(p); // consume '{'

    bool prevInModule = p->inModule;
    bool prevMark = p->markExport;
    const char* prevModuleName = p->currentModuleName;
    p->inModule = true;
    p->markExport = false;
    p->currentModuleName = nameTok->value;

    AST *moduleNode = newASTNode(AST_MODULE, nameTok);
    AST *block = newASTNode(AST_BLOCK, NULL);
    AST *decls = newASTNode(AST_COMPOUND, NULL);
    AST *stmts = newASTNode(AST_COMPOUND, NULL);
    decls->is_global_scope = true;
    stmts->is_global_scope = true;
    addChild(block, decls);
    addChild(block, stmts);
    setRight(moduleNode, block);

    while (p->current.type != REA_TOKEN_RIGHT_BRACE && p->current.type != REA_TOKEN_EOF) {
        AST *stmt = parseStatement(p);
        if (!stmt) {
            if (p->current.type == REA_TOKEN_RIGHT_BRACE || p->current.type == REA_TOKEN_EOF) {
                break;
            }
            p->hadError = true;
            break;
        }
        appendModuleNode(decls, stmts, stmt);
        while (p->current.type == REA_TOKEN_SEMICOLON) {
            reaAdvance(p);
        }
    }

    if (p->current.type == REA_TOKEN_RIGHT_BRACE) {
        reaAdvance(p);
    } else {
        fprintf(stderr, "L%d: Expected '}' to close module body.\n", p->current.line);
        p->hadError = true;
    }

    if (p->current.type == REA_TOKEN_SEMICOLON) {
        reaAdvance(p);
    }

    p->inModule = prevInModule;
    p->markExport = prevMark;
    p->currentModuleName = prevModuleName;
    return moduleNode;
}

static AST *parseStatement(ReaParser *p) {
    if (p->current.type == REA_TOKEN_EXPORT) {
        int line = p->current.line;
        reaAdvance(p); // consume 'export'
        if (!p->inModule) {
            fprintf(stderr, "L%d: 'export' is only valid inside a module.\n", line);
            p->hadError = true;
        }
        bool prevMark = p->markExport;
        p->markExport = true;
        AST *decl = parseStatement(p);
        p->markExport = prevMark;
        if (decl && p->inModule) {
            markExported(decl);
        }
        return decl;
    }

    if (p->current.type == REA_TOKEN_MODULE) {
        return parseModule(p);
    }

    if (p->current.type == REA_TOKEN_TYPE) {
        return parseTypeAlias(p);
    }

    if (p->current.type == REA_TOKEN_CLASS) {
        // class Name { field declarations ; ... }
        
        reaAdvance(p); // consume 'class'
        if (p->current.type != REA_TOKEN_IDENTIFIER) return NULL;
        // class name token
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *classNameTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);
        reaAdvance(p); // consume class name

        // Optional 'extends' <Identifier>
        AST *parentRef = NULL;
        if (p->current.type == REA_TOKEN_EXTENDS) {
            reaAdvance(p); // consume extends
            if (p->current.type == REA_TOKEN_IDENTIFIER) {
                char *plex = (char *)malloc(p->current.length + 1);
                if (plex) {
                    memcpy(plex, p->current.start, p->current.length);
                    plex[p->current.length] = '\0';
                    Token *ptypeTok = newToken(TOKEN_IDENTIFIER, plex, p->current.line, 0);
                    free(plex);
                    parentRef = newASTNode(AST_TYPE_REFERENCE, ptypeTok);
                }
                reaAdvance(p);
            }
        }

        // Parse class body
        AST *recordAst = newASTNode(AST_RECORD_TYPE, NULL);
        if (parentRef) setExtra(recordAst, parentRef);
        // Inject hidden vtable pointer field as first record member
        Token *vtTok = newToken(TOKEN_IDENTIFIER, "__vtable", classNameTok->line, 0);
        AST *vtVar = newASTNode(AST_VARIABLE, vtTok);
        setTypeAST(vtVar, TYPE_POINTER);

        AST *vtType = newASTNode(AST_POINTER_TYPE, NULL);
        setTypeAST(vtType, TYPE_POINTER);

        AST *vtDecl = newASTNode(AST_VAR_DECL, NULL);
        addChild(vtDecl, vtVar);
        setRight(vtDecl, vtType);
        setTypeAST(vtDecl, TYPE_POINTER);
        addChild(recordAst, vtDecl);
        AST *methods = newASTNode(AST_COMPOUND, NULL);
        const char* prevClass = p->currentClassName;
        const char* prevParent = p->currentParentClassName;
        int prevIndex = p->currentMethodIndex;
        p->currentClassName = classNameTok->value;
        p->currentParentClassName = parentRef && parentRef->token ? parentRef->token->value : NULL;
        p->currentMethodIndex = 0;
        if (p->current.type == REA_TOKEN_LEFT_BRACE) {
            reaAdvance(p); // consume '{'
            
            while (p->current.type != REA_TOKEN_RIGHT_BRACE && p->current.type != REA_TOKEN_EOF) {
                if (p->current.type == REA_TOKEN_CONST) {
                    AST *c = parseConstDecl(p);
                    if (c) {
                        addChild(recordAst, c);
                    } else {
                        reaAdvance(p);
                    }
                } else if (p->current.type == REA_TOKEN_INT || p->current.type == REA_TOKEN_INT64 ||
                           p->current.type == REA_TOKEN_INT32 || p->current.type == REA_TOKEN_INT16 ||
                           p->current.type == REA_TOKEN_INT8 || p->current.type == REA_TOKEN_FLOAT ||
                           p->current.type == REA_TOKEN_FLOAT32 || p->current.type == REA_TOKEN_LONG_DOUBLE ||
                           p->current.type == REA_TOKEN_CHAR || p->current.type == REA_TOKEN_BYTE ||
                           p->current.type == REA_TOKEN_STR || p->current.type == REA_TOKEN_TEXT ||
                           p->current.type == REA_TOKEN_MSTREAM || p->current.type == REA_TOKEN_BOOL ||
                           p->current.type == REA_TOKEN_VOID ||
                           p->current.type == REA_TOKEN_IDENTIFIER) {
                    AST *decl = parseVarDecl(p);
                    if (decl) {
                        if (decl->type == AST_COMPOUND) {
                            for (int di = 0; di < decl->child_count; di++) {
                                AST *d = decl->children[di];
                                if (!d) continue;
                                if (d->type == AST_FUNCTION_DECL || d->type == AST_PROCEDURE_DECL) {
                                    addChild(methods, d);
                                } else {
                                    addChild(recordAst, d);
                                }
                                decl->children[di] = NULL;
                            }
                            freeAST(decl);
                        } else if (decl->type == AST_FUNCTION_DECL) {
                            addChild(methods, decl);
                        } else if (decl->type == AST_PROCEDURE_DECL) {
                            addChild(methods, decl);
                        } else {
                            addChild(recordAst, decl);
                        }
                    } else {
                        reaAdvance(p);
                    }
                } else {
                    reaAdvance(p);
                }
                // optional semicolons are consumed by parseVarDecl; any extras will be skipped
            }
            if (p->current.type == REA_TOKEN_RIGHT_BRACE) reaAdvance(p);
        }
        
        p->currentClassName = prevClass;
        p->currentParentClassName = prevParent;
        p->currentMethodIndex = prevIndex;

        // Build TYPE_DECL(Name = <recordAst>) and register type
        AST *typeDecl = newASTNode(AST_TYPE_DECL, classNameTok);
        setLeft(typeDecl, recordAst);

        // Register the type into the global type table
        if (classNameTok && classNameTok->value) {
            insertType(classNameTok->value, recordAst);
        }
        // Return both type and methods in a compound so top-level gets both
        AST *bundle = newASTNode(AST_COMPOUND, NULL);
        bundle->is_global_scope = true; // mark for top-level flattening
        addChild(bundle, typeDecl);
        // append methods
        if (methods && methods->child_count > 0) {
            for (int mi = 0; mi < methods->child_count; mi++) {
                addChild(bundle, methods->children[mi]);
                methods->children[mi] = NULL;
            }
            methods->child_count = 0;
        }
        freeAST(methods);
        return bundle;
    }
    if (p->current.type == REA_TOKEN_WHILE) {
        return parseWhile(p);
    }
    if (p->current.type == REA_TOKEN_BREAK) {
        return parseBreak(p);
    }
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        return parseBlock(p);
    }
    if (p->current.type == REA_TOKEN_IF) {
        return parseIf(p);
    }
    if (p->current.type == REA_TOKEN_WHILE) {
        return parseWhile(p);
    }
    if (p->current.type == REA_TOKEN_DO) {
        return parseDoWhile(p);
    }
    if (p->current.type == REA_TOKEN_FOR) {
        return parseFor(p);
    }
    if (p->current.type == REA_TOKEN_SWITCH) {
        return parseSwitch(p);
    }
    if (p->current.type == REA_TOKEN_MATCH) {
        return parseMatch(p);
    }
    if (p->current.type == REA_TOKEN_TRY) {
        return parseTry(p);
    }
    if (p->current.type == REA_TOKEN_THROW) {
        return parseThrow(p);
    }
    if (p->current.type == REA_TOKEN_CONTINUE) {
        reaAdvance(p);
        if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);
        return newASTNode(AST_CONTINUE, NULL);
    }
    if (p->current.type == REA_TOKEN_BREAK) {
        return parseBreak(p);
    }
    if (p->current.type == REA_TOKEN_CONST) {
        return parseConstDecl(p);
    }
    if (p->current.type == REA_TOKEN_IMPORT) {
        return parseImport(p);
    }
    if (p->current.type == REA_TOKEN_JOIN) {
        return parseJoin(p);
    }
    if (p->current.type == REA_TOKEN_RETURN) {
        return parseReturn(p);
    }
    if (p->current.type == REA_TOKEN_INT || p->current.type == REA_TOKEN_INT64 ||
        p->current.type == REA_TOKEN_INT32 || p->current.type == REA_TOKEN_INT16 ||
        p->current.type == REA_TOKEN_INT8 || p->current.type == REA_TOKEN_FLOAT ||
        p->current.type == REA_TOKEN_FLOAT32 || p->current.type == REA_TOKEN_LONG_DOUBLE ||
        p->current.type == REA_TOKEN_CHAR || p->current.type == REA_TOKEN_BYTE ||
        p->current.type == REA_TOKEN_STR || p->current.type == REA_TOKEN_TEXT ||
        p->current.type == REA_TOKEN_MSTREAM || p->current.type == REA_TOKEN_BOOL ||
        p->current.type == REA_TOKEN_VOID) {
        return parseVarDecl(p);
    }
    // Allow declarations that start with user-defined type identifiers
    if (p->current.type == REA_TOKEN_IDENTIFIER) {
        // Peek the identifier text and check type table
        char namebuf[256];
        size_t n = p->current.length < sizeof(namebuf)-1 ? p->current.length : sizeof(namebuf)-1;
        memcpy(namebuf, p->current.start, n);
        namebuf[n] = '\0';
        AST *tdef = lookupType(namebuf);
        if (tdef) {
            if (!tdef->token) {
                freeAST(tdef);
            }
            return parseVarDecl(p);
        }
        ReaToken peek = reaPeekToken(p);
        if (peek.type == REA_TOKEN_IDENTIFIER || peek.type == REA_TOKEN_LESS) {
            return parseVarDecl(p);
        }
    }
    AST *expr = parseExpression(p);
    if (!expr) return NULL;
    if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);
    if (expr->type == AST_ASSIGN) {
        return expr; // assignments act as statements directly
    }
    AST *stmt = newASTNode(AST_EXPR_STMT, expr->token);
    setLeft(stmt, expr);
    return stmt;
}

AST *parseRea(const char *source) {
    ReaParser p;
    reaInitLexer(&p.lexer, source);
    p.currentFunctionType = TYPE_VOID;
    p.hadError = false;
    p.currentClassName = NULL;
    p.currentParentClassName = NULL;
    p.currentModuleName = NULL;
    p.currentMethodIndex = 0;
    p.functionDepth = 0;
    p.inModule = false;
    p.markExport = false;
    p.genericTypeNames = NULL;
    p.genericTypeCount = 0;
    p.genericTypeCapacity = 0;
    p.genericFrameStack = NULL;
    p.genericFrameDepth = 0;
    p.genericFrameCapacity = 0;
    reaAdvance(&p);

    AST *program = newASTNode(AST_PROGRAM, NULL);
    AST *block = newASTNode(AST_BLOCK, NULL);
    setRight(program, block);

    AST *decls = newASTNode(AST_COMPOUND, NULL);
    AST *stmts = newASTNode(AST_COMPOUND, NULL);
    addChild(block, decls);
    addChild(block, stmts);

    while (p.current.type != REA_TOKEN_EOF && !p.hadError) {
        AST *stmt = parseStatement(&p);
        if (!stmt) {
            // Be forgiving at top level: skip stray right braces instead of erroring.
            if (p.current.type == REA_TOKEN_RIGHT_BRACE) {
                reaAdvance(&p);
                continue;
            }
            fprintf(stderr,
                    "Unexpected token %s '%.*s' at line %d\n",
                    reaTokenTypeToString(p.current.type),
                    (int)p.current.length,
                    p.current.start,
                    p.current.line);
            p.hadError = true;
            break;
        }

        if (stmt->type == AST_COMPOUND && stmt->is_global_scope) {
            for (int i = 0; i < stmt->child_count; i++) {
                AST *child = stmt->children[i];
                if (!child) continue;
                if (child->type == AST_VAR_DECL || child->type == AST_FUNCTION_DECL || child->type == AST_PROCEDURE_DECL ||
                    child->type == AST_TYPE_DECL || child->type == AST_CONST_DECL) {
                    addChild(decls, child);
                    
                } else {
                    addChild(stmts, child);
                    
                }
                stmt->children[i] = NULL;
            }
            freeAST(stmt);
        } else if (stmt->type == AST_VAR_DECL || stmt->type == AST_FUNCTION_DECL || stmt->type == AST_PROCEDURE_DECL ||
                   stmt->type == AST_TYPE_DECL || stmt->type == AST_CONST_DECL || stmt->type == AST_MODULE ||
                   stmt->type == AST_USES_CLAUSE) {
            addChild(decls, stmt);
            
        } else {
            addChild(stmts, stmt);
            
        }
    }

    AST **function_body_nodes = NULL;
    int function_body_node_count = 0;
    int function_body_node_capacity = 0;
    FunctionBodyRange *function_body_ranges = NULL;
    int function_body_range_count = 0;
    int function_body_range_capacity = 0;
    const char **global_names = NULL;
    int global_name_count = 0;
    int global_name_capacity = 0;
    bool collected_function_nodes = true;
    for (int i = 0; i < decls->child_count && collected_function_nodes; i++) {
        AST *d = decls->children[i];
        if (!d) continue;
        AST *body = NULL;
        if (d->type == AST_FUNCTION_DECL) {
            body = d->extra;
        } else if (d->type == AST_PROCEDURE_DECL) {
            body = d->right;
        }
        if (!body) continue;
        if (!collectFunctionBodyNodes(body,
                                      &function_body_nodes,
                                      &function_body_node_count,
                                      &function_body_node_capacity)) {
            collected_function_nodes = false;
        }
        int min_line = INT_MAX;
        int max_line = 0;
        computeLineRange(body, &min_line, &max_line);
        if (max_line > 0 && min_line <= max_line) {
            if (function_body_range_count >= function_body_range_capacity) {
                int new_capacity = function_body_range_capacity < 8 ? 8 : function_body_range_capacity * 2;
                FunctionBodyRange *resized = (FunctionBodyRange *)realloc(function_body_ranges,
                                                                          (size_t)new_capacity * sizeof(FunctionBodyRange));
                if (resized) {
                    function_body_ranges = resized;
                    function_body_range_capacity = new_capacity;
                }
            }
            if (function_body_range_count < function_body_range_capacity) {
                function_body_ranges[function_body_range_count].body = body;
                function_body_ranges[function_body_range_count].min_line = min_line;
                function_body_ranges[function_body_range_count].max_line = max_line;
                function_body_range_count++;
            }
        }
    }

    for (int i = 0; i < decls->child_count; i++) {
        collectGlobalNamesFromDecl(decls->children[i], &global_names, &global_name_count, &global_name_capacity);
    }

    if ((function_body_nodes && function_body_node_count > 0) || function_body_range_count > 0) {
        int write_idx = 0;
        for (int i = 0; i < stmts->child_count; i++) {
            AST *s = stmts->children[i];
            if (!s) continue;
            bool duplicate = false;
            bool pointer_match = false;
            bool reattached_to_body = false;
            int stmt_min = INT_MAX;
            int stmt_max = 0;
            computeLineRange(s, &stmt_min, &stmt_max);
            int references_non_global = -1;
            if (function_body_nodes && function_body_node_count > 0) {
                for (int j = 0; j < function_body_node_count; j++) {
                    AST *body_node = function_body_nodes[j];
                    if (!body_node) continue;
                    if (!astStructurallyEqual(s, body_node)) {
                        continue;
                    }
                    FunctionBodyRange *owner = findFunctionBodyRangeForNode(body_node,
                                                                            function_body_ranges,
                                                                            function_body_range_count);
                    bool same_span = false;
                    if (owner && stmt_max > 0 && stmt_min <= stmt_max) {
                        if (stmt_max >= owner->min_line && stmt_min <= owner->max_line) {
                            same_span = true;
                        }
                    }
                    bool uses_non_global = false;
                    if (!same_span && global_name_count > 0 && stmt_max > 0) {
                        if (references_non_global == -1) {
                            references_non_global = statementReferencesNonGlobal(s,
                                                                                global_names,
                                                                                global_name_count,
                                                                                NULL)
                                                       ? 1 : 0;
                        }
                        uses_non_global = (references_non_global == 1);
                    }
                    if (!same_span && !uses_non_global) {
                        continue;
                    }
                    duplicate = true;
                    if (s == body_node) {
                        pointer_match = true;
                    }
                    if (uses_non_global && owner && owner->body && s->parent != owner->body) {
                        addChild(owner->body, s);
                        reattached_to_body = true;
                    }
                    break;
                }
            }
            if (!duplicate && function_body_range_count > 0) {
                if (stmt_max > 0 && stmt_min <= stmt_max) {
                    for (int j = 0; j < function_body_range_count; j++) {
                        FunctionBodyRange *info = &function_body_ranges[j];
                        if (stmt_max >= info->min_line && stmt_min <= info->max_line) {
                            duplicate = true;
                            if (info->body && s->parent != info->body) {
                                addChild(info->body, s);
                                reattached_to_body = true;
                            }
                            break;
                        }
                    }
                }
            }
            if (!duplicate && global_name_count > 0 && stmt_max > 0) {
                bool uses_non_global = false;
                if (references_non_global != -1) {
                    uses_non_global = (references_non_global == 1);
                } else {
                    uses_non_global = statementReferencesNonGlobal(s, global_names, global_name_count, NULL);
                    references_non_global = uses_non_global ? 1 : 0;
                }
                if (uses_non_global) {
                    FunctionBodyRange *target = NULL;
                    for (int j = function_body_range_count - 1; j >= 0; j--) {
                        FunctionBodyRange *info = &function_body_ranges[j];
                        if (stmt_min >= info->min_line && stmt_min <= info->max_line) {
                            target = info;
                            break;
                        }
                        if (stmt_min > info->max_line) {
                            target = info;
                            break;
                        }
                    }
                    if (target && target->body && s->parent != target->body) {
                        addChild(target->body, s);
                        reattached_to_body = true;
                        duplicate = true;
                    }
                }
            }
            if (duplicate) {
                if (!pointer_match && !reattached_to_body) {
                    freeAST(s);
                }
                continue;
            }
            stmts->children[write_idx++] = s;
        }
        for (int i = write_idx; i < stmts->child_count; i++) {
            stmts->children[i] = NULL;
        }
        stmts->child_count = write_idx;
    }
    free(function_body_nodes);
    free(function_body_ranges);
    free(global_names);

    // Optional strict validation for top-level structure issues (e.g., accidental use of 'myself' at top level).
    if (!p.hadError && g_rea_strict_mode) {
        for (int i = 0; i < stmts->child_count && !p.hadError; i++) {
            AST* s = stmts->children[i];
            if (strictScanTop(s)) {
                int l = s && s->token ? s->token->line : 0;
                fprintf(stderr, "L%d: Strict mode error: disallowed construct at top level (e.g., 'myself' or 'return').\n", l);
                p.hadError = true;
                break;
            }
        }
    }

    clearGenericState(&p);

    if (p.hadError) {
        freeAST(program);
        return NULL;
    }

    // If a function or procedure named 'main' is declared and there are no
    // top-level statements, insert an implicit call to `main` so the VM
    // executes user code on program start.
    bool has_main = false;
    bool has_app = false;
    for (int i = 0; i < decls->child_count; i++) {
        AST *d = decls->children[i];
        if (!d) continue;

        if ((d->type == AST_FUNCTION_DECL || d->type == AST_PROCEDURE_DECL) &&
            d->token && d->token->value &&
            strcasecmp(d->token->value, "main") == 0) {
            has_main = true;
        }

        if (d->type == AST_VAR_DECL && d->child_count > 0 &&
            d->children[0] && d->children[0]->token && d->children[0]->token->value &&
            strcasecmp(d->children[0]->token->value, "app") == 0) {
            has_app = true;
        }
    }
    if (stmts->child_count == 0) {
        if (has_main) {
            Token *mainTok = newToken(TOKEN_IDENTIFIER, "main", 0, 0);
            AST *call = newASTNode(AST_PROCEDURE_CALL, mainTok);
            AST *stmt = newASTNode(AST_EXPR_STMT, call->token);
            setLeft(stmt, call);
            addChild(stmts, stmt);
        } else if (has_app) {
            Token *appTok = newToken(TOKEN_IDENTIFIER, "app", 0, 0);
            AST *appVar = newASTNode(AST_VARIABLE, appTok);
            setTypeAST(appVar, TYPE_UNKNOWN);
            Token *runTok = newToken(TOKEN_IDENTIFIER, "run", 0, 0);
            AST *call = newASTNode(AST_PROCEDURE_CALL, runTok);
            setLeft(call, appVar);
            addChild(call, appVar);
            AST *stmt = newASTNode(AST_EXPR_STMT, runTok);
            setLeft(stmt, call);
            addChild(stmts, stmt);
        }
    }

    return program;
}
