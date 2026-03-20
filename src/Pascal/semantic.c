#include "Pascal/semantic.h"

#include "Pascal/globals.h"
#include "Pascal/type_registry.h"
#include "ast/closure_registry.h"
#include "core/utils.h"
#include "symbol/symbol.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static AST *gProgramRoot = NULL;
static ClosureCaptureRegistry gClosureRegistry;
static bool gRegistryInitialized = false;
static AST *desugarNode(AST *node, VarType currentFunctionType);

static int nodeLine(AST *node) {
    if (!node) {
        return 0;
    }
    if (node->token) {
        return node->token->line;
    }
    if (node->left) {
        int line = nodeLine(node->left);
        if (line > 0) {
            return line;
        }
    }
    if (node->right) {
        int line = nodeLine(node->right);
        if (line > 0) {
            return line;
        }
    }
    if (node->extra) {
        int line = nodeLine(node->extra);
        if (line > 0) {
            return line;
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        int line = nodeLine(node->children[i]);
        if (line > 0) {
            return line;
        }
    }
    return 0;
}

static AST *getDeclsCompound(AST *node) {
    if (!node) {
        return NULL;
    }

    AST *block = NULL;
    if (node->type == AST_PROGRAM || node->type == AST_MODULE) {
        block = node->right;
    } else if (node->type == AST_BLOCK) {
        block = node;
    }

    if (!block || block->child_count <= 0 || !block->children) {
        return NULL;
    }

    AST *decls = block->children[0];
    if (decls && decls->type == AST_COMPOUND) {
        return decls;
    }
    return NULL;
}

static AST *cloneTypeForVar(VarType type, AST *typeDef, int line) {
    if (typeDef) {
        return copyAST(typeDef);
    }

    const char *name = NULL;
    switch (type) {
        case TYPE_INT32:        name = "integer"; break;
        case TYPE_INT16:        name = "smallint"; break;
        case TYPE_INT8:         name = "shortint"; break;
        case TYPE_INT64:        name = "int64"; break;
        case TYPE_UINT32:       name = "cardinal"; break;
        case TYPE_UINT64:       name = "uint64"; break;
        case TYPE_FLOAT:        name = "single"; break;
        case TYPE_DOUBLE:       name = "double"; break;
        case TYPE_LONG_DOUBLE:  name = "extended"; break;
        case TYPE_BOOLEAN:      name = "boolean"; break;
        case TYPE_STRING:       name = "string"; break;
        case TYPE_CHAR:         name = "char"; break;
        case TYPE_BYTE:         name = "byte"; break;
        case TYPE_WORD:         name = "word"; break;
        default:
            break;
    }

    if (name) {
        Token *tok = newToken(TOKEN_IDENTIFIER, name, line, 0);
        AST *typeNode = newASTNode(AST_TYPE_IDENTIFIER, tok);
        setTypeAST(typeNode, type);
        return typeNode;
    }

    if (type == TYPE_POINTER) {
        AST *ptrNode = newASTNode(AST_POINTER_TYPE, NULL);
        setTypeAST(ptrNode, TYPE_POINTER);
        Token *tok = newToken(TOKEN_IDENTIFIER, "byte", line, 0);
        AST *base = newASTNode(AST_TYPE_IDENTIFIER, tok);
        setTypeAST(base, TYPE_BYTE);
        setRight(ptrNode, base);
        return ptrNode;
    }

    return NULL;
}

static AST *createBooleanLiteral(bool value, int line) {
    Token *tok = newToken(value ? TOKEN_TRUE : TOKEN_FALSE, value ? "true" : "false", line, 0);
    AST *node = newASTNode(AST_BOOLEAN, tok);
    node->i_val = value ? 1 : 0;
    setTypeAST(node, TYPE_BOOLEAN);
    return node;
}

static AST *createNumberLiteral(long long value, VarType type, int line) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", value);
    Token *tok = newToken(TOKEN_INTEGER_CONST, buf, line, 0);
    AST *node = newASTNode(AST_NUMBER, tok);
    node->i_val = (int)value;
    setTypeAST(node, type);
    return node;
}

static AST *createStringLiteral(const char *value, VarType type, int line) {
    Token *tok = newToken(TOKEN_STRING_CONST, value ? value : "", line, 0);
    AST *node = newASTNode(AST_STRING, tok);
    setTypeAST(node, type);
    return node;
}

static AST *createNilLiteral(VarType type, int line) {
    Token *tok = newToken(TOKEN_NIL, "nil", line, 0);
    AST *node = newASTNode(AST_NIL, tok);
    setTypeAST(node, type);
    return node;
}

static AST *createVarRef(const char *name, VarType type, AST *typeDef, int line) {
    Token *tok = newToken(TOKEN_IDENTIFIER, name, line, 0);
    AST *var = newASTNode(AST_VARIABLE, tok);
    setTypeAST(var, type);
    if (typeDef) {
        var->type_def = copyAST(typeDef);
    }
    return var;
}

static AST *createAssignment(AST *lhs, AST *rhs, int line) {
    Token *tok = newToken(TOKEN_ASSIGN, ":=", line, 0);
    AST *assign = newASTNode(AST_ASSIGN, tok);
    setLeft(assign, lhs);
    setRight(assign, rhs);
    setTypeAST(assign, lhs ? lhs->var_type : TYPE_VOID);
    return assign;
}

static AST *createPendingBreakNode(int line) {
    AST *ifBody = newASTNode(AST_COMPOUND, NULL);
    addChild(ifBody, newASTNode(AST_BREAK, NULL));

    AST *ifNode = newASTNode(AST_IF, NULL);
    setLeft(ifNode, createVarRef("__pas_exc_pending", TYPE_BOOLEAN, NULL, line));
    setRight(ifNode, ifBody);
    return ifNode;
}

static AST *createThrowBreakBlock(int line) {
    AST *block = newASTNode(AST_COMPOUND, NULL);
    addChild(block, createAssignment(createVarRef("__pas_exc_pending", TYPE_BOOLEAN, NULL, line),
                                     createBooleanLiteral(true, line),
                                     line));
    addChild(block, newASTNode(AST_BREAK, NULL));
    return block;
}

static void appendStatementsFromBlock(AST *target, AST *block) {
    if (!target || !block) {
        return;
    }

    if (block->type == AST_COMPOUND) {
        for (int i = 0; i < block->child_count; i++) {
            AST *child = block->children[i];
            if (!child) {
                continue;
            }
            block->children[i] = NULL;
            addChild(target, child);
        }
        block->child_count = 0;
        freeAST(block);
    } else {
        addChild(target, block);
    }
}

static bool astContainsExceptions(AST *node) {
    if (!node) {
        return false;
    }
    if (node->type == AST_TRY || node->type == AST_THROW) {
        return true;
    }
    if (astContainsExceptions(node->left)) {
        return true;
    }
    if (astContainsExceptions(node->right)) {
        return true;
    }
    if (astContainsExceptions(node->extra)) {
        return true;
    }
    for (int i = 0; i < node->child_count; i++) {
        if (astContainsExceptions(node->children[i])) {
            return true;
        }
    }
    return false;
}

static AST *createDefaultReturnValue(VarType type, AST *typeDef, int line) {
    switch (type) {
        case TYPE_BOOLEAN:
            return createBooleanLiteral(false, line);
        case TYPE_INT8:
        case TYPE_UINT8:
        case TYPE_INT16:
        case TYPE_UINT16:
        case TYPE_INT32:
        case TYPE_UINT32:
        case TYPE_INT64:
        case TYPE_UINT64:
        case TYPE_BYTE:
        case TYPE_WORD:
        case TYPE_DOUBLE:
        case TYPE_LONG_DOUBLE:
        case TYPE_FLOAT:
        case TYPE_ENUM:
        case TYPE_CHAR:
            return createNumberLiteral(0, type, line);
        case TYPE_STRING:
            return createStringLiteral("", TYPE_STRING, line);
        case TYPE_VOID:
            return NULL;
        default:
            return createNilLiteral(typeDef ? typeDef->var_type : type, line);
    }
}

static void rewriteThrowsForTry(AST *node) {
    if (!node) {
        return;
    }

    if (node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL || node->type == AST_TRY) {
        return;
    }

    if (node->left) {
        if (node->left->type == AST_THROW) {
            AST *throwNode = node->left;
            int line = throwNode->i_val;
            node->left = NULL;
            freeAST(throwNode);
            setLeft(node, createThrowBreakBlock(line));
        } else {
            rewriteThrowsForTry(node->left);
        }
    }

    if (node->right) {
        if (node->right->type == AST_THROW) {
            AST *throwNode = node->right;
            int line = throwNode->i_val;
            node->right = NULL;
            freeAST(throwNode);
            setRight(node, createThrowBreakBlock(line));
        } else {
            rewriteThrowsForTry(node->right);
        }
    }

    if (node->extra) {
        if (node->extra->type == AST_THROW) {
            AST *throwNode = node->extra;
            int line = throwNode->i_val;
            node->extra = NULL;
            freeAST(throwNode);
            setExtra(node, createThrowBreakBlock(line));
        } else {
            rewriteThrowsForTry(node->extra);
        }
    }

    for (int i = 0; i < node->child_count; i++) {
        AST *child = node->children[i];
        if (!child) {
            continue;
        }
        if (child->type == AST_THROW) {
            int line = child->i_val;
            freeAST(child);
            node->children[i] = createThrowBreakBlock(line);
            node->children[i]->parent = node;
        } else {
            rewriteThrowsForTry(child);
        }
    }
}

static void appendGuardedTryStatements(AST *target, AST *block) {
    if (!target || !block) {
        return;
    }

    if (block->type == AST_COMPOUND) {
        for (int i = 0; i < block->child_count; i++) {
            AST *child = block->children[i];
            if (!child) {
                continue;
            }
            block->children[i] = NULL;
            addChild(target, child);
            addChild(target, createPendingBreakNode(nodeLine(child)));
        }
        block->child_count = 0;
        freeAST(block);
        return;
    }

    addChild(target, block);
    addChild(target, createPendingBreakNode(nodeLine(block)));
}

static AST *desugarTryNode(AST *node, VarType currentFunctionType) {
    AST *tryBlock = node->left;
    AST *catchNode = node->right;
    AST *catchBody = NULL;
    int labelLine = 0;

    node->left = NULL;
    node->right = NULL;

    if (tryBlock) {
        labelLine = nodeLine(tryBlock);
        rewriteThrowsForTry(tryBlock);
        tryBlock = desugarNode(tryBlock, currentFunctionType);
    }

    if (catchNode) {
        catchBody = catchNode->right;
        catchNode->right = NULL;
        freeAST(catchNode);
    }
    if (catchBody) {
        catchBody = desugarNode(catchBody, currentFunctionType);
    }

    AST *result = newASTNode(AST_COMPOUND, NULL);
    addChild(result, createAssignment(createVarRef("__pas_exc_pending", TYPE_BOOLEAN, NULL, labelLine),
                                      createBooleanLiteral(false, labelLine),
                                      labelLine));

    AST *loopBody = newASTNode(AST_COMPOUND, NULL);
    appendGuardedTryStatements(loopBody, tryBlock);
    AST *repeatNode = newASTNode(AST_REPEAT, NULL);
    setLeft(repeatNode, loopBody);
    setRight(repeatNode, createBooleanLiteral(true, labelLine));
    addChild(result, repeatNode);

    AST *ifBody = newASTNode(AST_COMPOUND, NULL);
    addChild(ifBody, createAssignment(createVarRef("__pas_exc_pending", TYPE_BOOLEAN, NULL, labelLine),
                                      createBooleanLiteral(false, labelLine),
                                      labelLine));
    appendStatementsFromBlock(ifBody, catchBody);

    AST *catchIf = newASTNode(AST_IF, NULL);
    setLeft(catchIf, createVarRef("__pas_exc_pending", TYPE_BOOLEAN, NULL, labelLine));
    setRight(catchIf, ifBody);
    addChild(result, catchIf);

    free(node);
    return result;
}

static AST *desugarThrowNode(AST *node, VarType currentFunctionType) {
    if (!node) {
        return NULL;
    }

    int line = node->i_val;
    AST *expr = node->left;
    node->left = NULL;
    if (expr) {
        freeAST(expr);
    }

    AST *result = newASTNode(AST_COMPOUND, NULL);
    addChild(result, createAssignment(createVarRef("__pas_exc_pending", TYPE_BOOLEAN, NULL, line),
                                      createBooleanLiteral(true, line),
                                      line));

    Token *retTok = newToken(TOKEN_RETURN, "return", line, 0);
    AST *ret = newASTNode(AST_RETURN, retTok);
    AST *retValue = createDefaultReturnValue(currentFunctionType, NULL, line);
    setLeft(ret, retValue);
    setTypeAST(ret, currentFunctionType);
    addChild(result, ret);

    free(node);
    return result;
}

static AST *desugarNode(AST *node, VarType currentFunctionType) {
    if (!node) {
        return NULL;
    }

    if (node->type == AST_TRY) {
        return desugarTryNode(node, currentFunctionType);
    }

    if (node->type == AST_THROW) {
        return desugarThrowNode(node, currentFunctionType);
    }

    if (node->type == AST_FUNCTION_DECL) {
        if (node->left) {
            AST *newLeft = desugarNode(node->left, currentFunctionType);
            if (newLeft != node->left) {
                setLeft(node, newLeft);
            }
        }
        if (node->right) {
            AST *newRight = desugarNode(node->right, currentFunctionType);
            if (newRight != node->right) {
                setRight(node, newRight);
            }
        }
        if (node->extra) {
            AST *newExtra = desugarNode(node->extra, node->var_type);
            if (newExtra != node->extra) {
                setExtra(node, newExtra);
            }
        }
    } else if (node->type == AST_PROCEDURE_DECL) {
        if (node->left) {
            AST *newLeft = desugarNode(node->left, currentFunctionType);
            if (newLeft != node->left) {
                setLeft(node, newLeft);
            }
        }
        if (node->right) {
            AST *newRight = desugarNode(node->right, TYPE_VOID);
            if (newRight != node->right) {
                setRight(node, newRight);
            }
        }
        if (node->extra) {
            AST *newExtra = desugarNode(node->extra, currentFunctionType);
            if (newExtra != node->extra) {
                setExtra(node, newExtra);
            }
        }
    } else {
        if (node->left) {
            AST *newLeft = desugarNode(node->left, currentFunctionType);
            if (newLeft != node->left) {
                setLeft(node, newLeft);
            }
        }
        if (node->right) {
            AST *newRight = desugarNode(node->right, currentFunctionType);
            if (newRight != node->right) {
                setRight(node, newRight);
            }
        }
        if (node->extra) {
            AST *newExtra = desugarNode(node->extra, currentFunctionType);
            if (newExtra != node->extra) {
                setExtra(node, newExtra);
            }
        }
    }

    for (int i = 0; i < node->child_count; i++) {
        AST *child = node->children[i];
        if (!child) {
            continue;
        }
        AST *newChild = desugarNode(child, currentFunctionType);
        if (newChild != child) {
            node->children[i] = newChild;
            if (newChild) {
                newChild->parent = node;
            }
        }
    }

    return node;
}

static void ensureExceptionGlobals(AST *root) {
    if (!root || !astContainsExceptions(root)) {
        return;
    }

    AST *decls = getDeclsCompound(root);
    if (!decls) {
        return;
    }

    bool hasPending = false;
    for (int i = 0; i < decls->child_count; i++) {
        AST *child = decls->children[i];
        if (!child || child->type != AST_VAR_DECL) {
            continue;
        }
        for (int j = 0; j < child->child_count; j++) {
            AST *varNode = child->children[j];
            if (!varNode || !varNode->token || !varNode->token->value) {
                continue;
            }
            if (strcasecmp(varNode->token->value, "__pas_exc_pending") == 0) {
                hasPending = true;
            }
        }
    }

    if (!hasPending) {
        AST *pendingDecl = newASTNode(AST_VAR_DECL, NULL);
        Token *pendingTok = newToken(TOKEN_IDENTIFIER, "__pas_exc_pending", 0, 0);
        AST *pendingVar = newASTNode(AST_VARIABLE, pendingTok);
        setTypeAST(pendingVar, TYPE_BOOLEAN);
        addChild(pendingDecl, pendingVar);
        setRight(pendingDecl, cloneTypeForVar(TYPE_BOOLEAN, NULL, 0));
        setTypeAST(pendingDecl, TYPE_BOOLEAN);
        setLeft(pendingDecl, createBooleanLiteral(false, 0));
        addChild(decls, pendingDecl);
    }
}

static AST *resolveTypeAliasLocal(AST *type_node) {
    if (!type_node) {
        return NULL;
    }

    while (type_node) {
        if ((type_node->type == AST_TYPE_REFERENCE || type_node->type == AST_VARIABLE) &&
            type_node->token && type_node->token->value) {
            AST *resolved = lookupType(type_node->token->value);
            if (!resolved || resolved == type_node) {
                break;
            }
            type_node = resolved;
            continue;
        }

        if (type_node->type == AST_TYPE_DECL && type_node->left) {
            type_node = type_node->left;
            continue;
        }

        break;
    }

    return type_node;
}

static bool astIsInterface(AST *node) {
    AST *resolved = resolveTypeAliasLocal(node);
    return resolved && resolved->type == AST_INTERFACE;
}

static bool astIsRecord(AST *node) {
    AST *resolved = resolveTypeAliasLocal(node);
    return resolved && resolved->type == AST_RECORD_TYPE;
}

static AST *findRecordMethod(AST *recordType, const char *name) {
    if (!recordType || recordType->type != AST_RECORD_TYPE || !name) {
        return NULL;
    }

    for (int i = 0; i < recordType->child_count; i++) {
        AST *child = recordType->children[i];
        if (!child) {
            continue;
        }
        if ((child->type == AST_PROCEDURE_DECL || child->type == AST_FUNCTION_DECL) &&
            child->token && child->token->value &&
            strcasecmp(child->token->value, name) == 0) {
            return child;
        }
    }

    return NULL;
}

static void markRecordAgainstInterface(AST *recordType, AST *interfaceType);

static void markRecordAgainstInterfaceList(AST *recordType, AST *baseList) {
    if (!baseList) {
        return;
    }

    if (baseList->type == AST_LIST) {
        for (int i = 0; i < baseList->child_count; i++) {
            markRecordAgainstInterface(recordType, baseList->children[i]);
        }
    } else {
        markRecordAgainstInterface(recordType, baseList);
    }
}

static void markRecordAgainstInterface(AST *recordType, AST *interfaceType) {
    AST *resolvedInterface = resolveTypeAliasLocal(interfaceType);
    if (!resolvedInterface || resolvedInterface->type != AST_INTERFACE) {
        return;
    }

    if (resolvedInterface->extra) {
        markRecordAgainstInterfaceList(recordType, resolvedInterface->extra);
    }

    for (int i = 0; i < resolvedInterface->child_count; i++) {
        AST *method = resolvedInterface->children[i];
        if (!method || (method->type != AST_PROCEDURE_DECL && method->type != AST_FUNCTION_DECL)) {
            continue;
        }
        if (!method->token || !method->token->value) {
            continue;
        }
        AST *recordMethod = findRecordMethod(recordType, method->token->value);
        if (recordMethod) {
            recordMethod->is_virtual = true;
        }
    }
}

static void markVirtualMethodsForInterfaces(void) {
    for (TypeEntry *recordEntry = type_table; recordEntry; recordEntry = recordEntry->next) {
        AST *recordNode = resolveTypeAliasLocal(recordEntry->typeAST);
        if (!astIsRecord(recordNode)) {
            continue;
        }

        for (TypeEntry *ifaceEntry = type_table; ifaceEntry; ifaceEntry = ifaceEntry->next) {
            AST *ifaceNode = ifaceEntry->typeAST;
            if (!astIsInterface(ifaceNode)) {
                continue;
            }
            markRecordAgainstInterface(recordNode, ifaceNode);
        }
    }
}

typedef struct {
    AST *declaration;
    bool is_by_ref;
} CaptureInfo;

typedef struct {
    CaptureInfo *items;
    size_t count;
    size_t capacity;
} CaptureCollection;

#define MAX_CLOSURE_CAPTURES (sizeof(((Symbol *)0)->upvalues) / sizeof(((Symbol *)0)->upvalues[0]))

static void captureCollectionInit(CaptureCollection *collection) {
    if (!collection) {
        return;
    }
    collection->items = NULL;
    collection->count = 0;
    collection->capacity = 0;
}

static void captureCollectionFree(CaptureCollection *collection) {
    if (!collection) {
        return;
    }
    free(collection->items);
    collection->items = NULL;
    collection->count = 0;
    collection->capacity = 0;
}

static bool captureCollectionAdd(CaptureCollection *collection, AST *decl, bool is_by_ref) {
    if (!collection || !decl) {
        return false;
    }

    for (size_t i = 0; i < collection->count; i++) {
        if (collection->items[i].declaration == decl) {
            if (is_by_ref) {
                collection->items[i].is_by_ref = true;
            }
            return true;
        }
    }

    if (collection->count >= MAX_CLOSURE_CAPTURES) {
        return false;
    }

    if (collection->count == collection->capacity) {
        size_t new_capacity = collection->capacity ? collection->capacity * 2 : 8;
        if (new_capacity > MAX_CLOSURE_CAPTURES) {
            new_capacity = MAX_CLOSURE_CAPTURES;
        }
        CaptureInfo *resized = (CaptureInfo *)realloc(collection->items, new_capacity * sizeof(CaptureInfo));
        if (!resized) {
            return false;
        }
        collection->items = resized;
        collection->capacity = new_capacity;
    }

    collection->items[collection->count].declaration = decl;
    collection->items[collection->count].is_by_ref = is_by_ref;
    collection->count++;
    return true;
}

static void ensureRegistry(void) {
    if (!gRegistryInitialized) {
        closureRegistryInit(&gClosureRegistry);
        gRegistryInitialized = true;
    }
    closureRegistryReset(&gClosureRegistry);
}

static AST *findEnclosingFunction(AST *node) {
    if (!node) {
        return NULL;
    }
    AST *curr = node->parent;
    while (curr) {
        if (curr->type == AST_FUNCTION_DECL || curr->type == AST_PROCEDURE_DECL) {
            return curr;
        }
        curr = curr->parent;
    }
    return NULL;
}

static AST *getRoutineBody(AST *routine) {
    if (!routine) {
        return NULL;
    }
    if (routine->type == AST_FUNCTION_DECL) {
        return routine->extra;
    }
    if (routine->type == AST_PROCEDURE_DECL) {
        return routine->right;
    }
    return NULL;
}

static void collectCapturesVisitor(AST *node, AST *func, CaptureCollection *captures) {
    if (!node || !func || !captures) {
        return;
    }
    if (node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) {
        return;
    }

    if (node->type == AST_VARIABLE && node->token && node->token->value) {
        const char *name = node->token->value;
        AST *decl = findStaticDeclarationInAST(name, node, gProgramRoot);
        if (decl && decl->type == AST_VAR_DECL) {
            AST *owner = findEnclosingFunction(decl);
            if (owner && owner != func) {
                bool is_by_ref = decl->by_ref != 0;
                captureCollectionAdd(captures, decl, is_by_ref);
            }
        }
    }

    if (node->left) {
        collectCapturesVisitor(node->left, func, captures);
    }
    if (node->right) {
        collectCapturesVisitor(node->right, func, captures);
    }
    if (node->extra) {
        collectCapturesVisitor(node->extra, func, captures);
    }
    for (int i = 0; i < node->child_count; i++) {
        collectCapturesVisitor(node->children[i], func, captures);
    }
}

static bool collectFunctionCaptureDescriptors(AST *func,
                                              ClosureCaptureDescriptor **out_descriptors,
                                              size_t *out_count) {
    if (out_descriptors) {
        *out_descriptors = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!func) {
        return false;
    }

    AST *body = getRoutineBody(func);
    if (!body) {
        return true;
    }

    CaptureCollection captures;
    captureCollectionInit(&captures);
    collectCapturesVisitor(body, func, &captures);

    if (captures.count == 0) {
        captureCollectionFree(&captures);
        return true;
    }

    size_t count = captures.count;
    ClosureCaptureDescriptor *descriptors = (ClosureCaptureDescriptor *)calloc(count, sizeof(ClosureCaptureDescriptor));
    if (!descriptors) {
        captureCollectionFree(&captures);
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        descriptors[i].slot_index = (uint8_t)i;
        descriptors[i].is_by_ref = captures.items[i].is_by_ref;
    }

    captureCollectionFree(&captures);

    if (out_descriptors) {
        *out_descriptors = descriptors;
    } else {
        free(descriptors);
    }
    if (out_count) {
        *out_count = count;
    }
    return true;
}

static void applyCaptureLayoutToSymbol(Symbol *sym,
                                       const ClosureCaptureDescriptor *descriptors,
                                       size_t descriptor_count) {
    if (!sym) {
        return;
    }
    size_t limit = descriptor_count;
    if (limit > MAX_CLOSURE_CAPTURES) {
        limit = MAX_CLOSURE_CAPTURES;
    }
    sym->upvalue_count = (uint8_t)limit;
    for (size_t i = 0; i < limit; i++) {
        uint8_t slot = descriptors ? descriptors[i].slot_index : (uint8_t)i;
        sym->upvalues[i].index = slot;
        sym->upvalues[i].isLocal = false;
        sym->upvalues[i].is_ref = descriptors ? descriptors[i].is_by_ref : false;
    }
    sym->closure_captures = limit > 0;
}

static Symbol *symbolForRoutine(AST *routine) {
    if (!routine || !routine->token || !routine->token->value) {
        return NULL;
    }
    char lowered[MAX_SYMBOL_LENGTH];
    strncpy(lowered, routine->token->value, MAX_SYMBOL_LENGTH - 1);
    lowered[MAX_SYMBOL_LENGTH - 1] = '\0';
    toLowerString(lowered);
    Symbol *sym = lookupProcedure(lowered);
    if (!sym) {
        sym = lookupGlobalSymbol(lowered);
    }
    return sym;
}

static void analyzeClosureCaptures(AST *node) {
    if (!node) {
        return;
    }
    if (node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) {
        ClosureCaptureDescriptor *descriptors = NULL;
        size_t descriptor_count = 0;
        collectFunctionCaptureDescriptors(node, &descriptors, &descriptor_count);
        bool captures = descriptor_count > 0;
        closureRegistryRecord(&gClosureRegistry, node, captures, descriptors, descriptor_count, false);
        Symbol *sym = symbolForRoutine(node);
        if (sym) {
            if (captures) {
                applyCaptureLayoutToSymbol(sym, descriptors, descriptor_count);
            } else {
                sym->closure_captures = false;
                sym->upvalue_count = 0;
            }
            sym->closure_escapes = false;
        }
        free(descriptors);
    }
    if (node->left) {
        analyzeClosureCaptures(node->left);
    }
    if (node->right) {
        analyzeClosureCaptures(node->right);
    }
    if (node->extra) {
        analyzeClosureCaptures(node->extra);
    }
    for (int i = 0; i < node->child_count; i++) {
        analyzeClosureCaptures(node->children[i]);
    }
}

static void noteClosureEscape(AST *decl) {
    if (!decl) {
        return;
    }
    bool captures = closureRegistryCaptures(&gClosureRegistry, decl);
    closureRegistryRecord(&gClosureRegistry, decl, captures, NULL, 0, true);
    Symbol *sym = symbolForRoutine(decl);
    if (!sym) {
        return;
    }
    sym->closure_escapes = true;
    if (captures && sym->upvalue_count == 0) {
        size_t descriptor_count = 0;
        const ClosureCaptureDescriptor *descriptors =
            closureRegistryGetDescriptors(&gClosureRegistry, decl, &descriptor_count);
        if (descriptors && descriptor_count > 0) {
            applyCaptureLayoutToSymbol(sym, descriptors, descriptor_count);
        }
    }
}

static void checkClosureEscapes(AST *node) {
    if (!node) {
        return;
    }

    if (node->type == AST_VARIABLE && node->token && node->token->value) {
        const char *name = node->token->value;
        AST *decl = findStaticDeclarationInAST(name, node, gProgramRoot);
        if (decl && (decl->type == AST_FUNCTION_DECL || decl->type == AST_PROCEDURE_DECL)) {
            if (closureRegistryCaptures(&gClosureRegistry, decl)) {
                AST *parent = node->parent;
                bool partOfCall = false;
                bool assigningFunctionResult = false;
                if (parent && parent->type == AST_PROCEDURE_CALL && parent->token && parent->token->value &&
                    node->token && node->token->value &&
                    strcasecmp(parent->token->value, node->token->value) == 0) {
                    partOfCall = true;
                }
                if (!partOfCall && parent && parent->type == AST_ASSIGN && parent->left == node) {
                    AST *enclosing = findEnclosingFunction(node);
                    if (enclosing == decl) {
                        assigningFunctionResult = true;
                    }
                }
                if (!partOfCall && !assigningFunctionResult) {
                    noteClosureEscape(decl);
                }
            }
        }
    }

    if (node->left) {
        checkClosureEscapes(node->left);
    }
    if (node->right) {
        checkClosureEscapes(node->right);
    }
    if (node->extra) {
        checkClosureEscapes(node->extra);
    }
    for (int i = 0; i < node->child_count; i++) {
        checkClosureEscapes(node->children[i]);
    }
}

void pascalPerformSemanticAnalysis(AST *root) {
    if (!root) {
        return;
    }

    ensureRegistry();
    ensureExceptionGlobals(root);
    gProgramRoot = desugarNode(root, TYPE_VOID);

    markVirtualMethodsForInterfaces();

    analyzeClosureCaptures(gProgramRoot);
    checkClosureEscapes(gProgramRoot);

    if (gRegistryInitialized) {
        closureRegistryDestroy(&gClosureRegistry);
        gRegistryInitialized = false;
    }
    gProgramRoot = NULL;
}
