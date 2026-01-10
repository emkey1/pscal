#include "core/types.h"
#include "core/utils.h"
#include "core/list.h"
#include "ast.h"
#include "Pascal/globals.h"
#include "symbol/symbol.h"
#include "Pascal/parser.h"
#include "backend_ast/builtin.h"
#include <string.h>

bool isNodeInTypeTable(AST* nodeToFind) {
    if (!nodeToFind || !type_table) return false; // No node or no table
    TypeEntry *entry = type_table;
    while (entry) {
        if (entry->typeAST == nodeToFind) {
            #ifdef DEBUG
            fprintf(stderr, "[DEBUG_FREE_CHECK] Node %p (Type: %s) found in type_table (Entry: %s).\n",
                    (void*)nodeToFind, astTypeToString(nodeToFind->type), entry->name);
            #endif
            return true; // Found the exact node pointer
        }
        entry = entry->next;
    }
    return false; // Not found
}

// Resolve type references to their concrete definitions.
static AST* resolveTypeAlias(AST* type_node) {
    while (type_node && type_node->type == AST_TYPE_REFERENCE &&
           type_node->token && type_node->token->value) {
        AST* looked = lookupType(type_node->token->value);
        if (!looked || looked == type_node) break;
        type_node = looked;
    }
    return type_node;
}

static Symbol *lookupProcedureInAncestors(const char *loweredName, AST *scope) {
    for (AST *curr = scope; curr; curr = curr->parent) {
        if (!curr->symbol_table) {
            continue;
        }

        Symbol *sym = NULL;
        if (curr->type == AST_UNIT) {
            for (Symbol *unitSym = curr->symbol_table; unitSym; unitSym = unitSym->next) {
                if (!unitSym->name) {
                    continue;
                }
                char lowered_unit_name[MAX_SYMBOL_LENGTH];
                strncpy(lowered_unit_name, unitSym->name, sizeof(lowered_unit_name) - 1);
                lowered_unit_name[sizeof(lowered_unit_name) - 1] = '\0';
                toLowerString(lowered_unit_name);
                if (strcmp(lowered_unit_name, loweredName) == 0) {
                    sym = unitSym;
                    break;
                }
            }
        } else {
            HashTable *table = (HashTable *)curr->symbol_table;
            sym = hashTableLookup(table, loweredName);
        }

        if (sym) {
            return resolveSymbolAlias(sym);
        }
    }
    return NULL;
}

Symbol *resolveProcedureSymbolInScope(const char *name, AST *referenceNode, AST *globalProgramNode) {
    if (!name) {
        return NULL;
    }

    char lowered[MAX_SYMBOL_LENGTH];
    strncpy(lowered, name, sizeof(lowered) - 1);
    lowered[sizeof(lowered) - 1] = '\0';
    toLowerString(lowered);

    Symbol *sym = lookupProcedure(lowered);
    if (!sym) {
        sym = lookupGlobalSymbol(lowered);
    }
    if (sym) {
        return resolveSymbolAlias(sym);
    }

    sym = lookupProcedureInAncestors(lowered, referenceNode);
    if (!sym && globalProgramNode) {
        sym = lookupProcedureInAncestors(lowered, globalProgramNode);
    }
    if (!sym) {
        AST *decl = findStaticDeclarationInAST(name, referenceNode, globalProgramNode);
        if (decl) {
            sym = lookupProcedureInAncestors(lowered, decl);
        }
    }
    return sym ? resolveSymbolAlias(sym) : NULL;
}

static VarType procPointerParamType(AST* param) {
    if (!param) {
        return TYPE_VOID;
    }
    if (param->type == AST_VAR_DECL) {
        if (param->type_def && param->type_def->var_type != TYPE_VOID) {
            return param->type_def->var_type;
        }
        if (param->right && param->right->var_type != TYPE_VOID) {
            return param->right->var_type;
        }
        return param->var_type;
    }
    return param->var_type;
}

static bool procPointerParamByRef(AST* param) {
    if (!param) {
        return false;
    }
    if (param->type == AST_VAR_DECL) {
        return param->by_ref != 0;
    }
    return param->by_ref != 0;
}

static bool compareProcPointerParams(AST* lhsParams, AST* rhsParams, const char* rhsName) {
    int lhsCount = lhsParams ? lhsParams->child_count : 0;
    int rhsCount = rhsParams ? rhsParams->child_count : 0;
    if (lhsCount != rhsCount) {
        if (rhsName) {
            fprintf(stderr,
                    "Type error: proc pointer arity mismatch for '%s' (expected %d, got %d).\n",
                    rhsName, lhsCount, rhsCount);
        } else {
            fprintf(stderr,
                    "Type error: proc pointer arity mismatch in assignment (expected %d, got %d).\n",
                    lhsCount, rhsCount);
        }
        pascal_semantic_error_count++;
        return false;
    }

    for (int i = 0; i < lhsCount; ++i) {
        AST* lhsParam = lhsParams ? lhsParams->children[i] : NULL;
        AST* rhsParam = rhsParams ? rhsParams->children[i] : NULL;
        if (!lhsParam || !rhsParam) {
            continue;
        }
        bool lhsByRef = procPointerParamByRef(lhsParam);
        bool rhsByRef = procPointerParamByRef(rhsParam);
        if (lhsByRef != rhsByRef) {
            const char* expected_conv = lhsByRef ? "VAR/OUT" : "value";
            const char* got_conv = rhsByRef ? "VAR/OUT" : "value";
            if (rhsName) {
                fprintf(stderr,
                        "Type error: proc pointer param %lld passing convention mismatch for '%s' (expected %s, got %s).\n",
                        (long long)i + 1, rhsName, expected_conv, got_conv);
            } else {
                fprintf(stderr,
                        "Type error: proc pointer param %lld passing convention mismatch in assignment (expected %s, got %s).\n",
                        (long long)i + 1, expected_conv, got_conv);
            }
            pascal_semantic_error_count++;
            return false;
        }
        VarType lhsType = procPointerParamType(lhsParam);
        VarType rhsType = procPointerParamType(rhsParam);
        if (lhsType != rhsType) {
            if (rhsName) {
                fprintf(stderr,
                        "Type error: proc pointer param %lld type mismatch for '%s' (expected %s, got %s).\n",
                        (long long)i + 1, rhsName,
                        varTypeToString(lhsType),
                        varTypeToString(rhsType));
            } else {
                fprintf(stderr,
                        "Type error: proc pointer param %lld type mismatch in assignment (expected %s, got %s).\n",
                        (long long)i + 1,
                        varTypeToString(lhsType),
                        varTypeToString(rhsType));
            }
            pascal_semantic_error_count++;
            return false;
        }
    }
    return true;
}

static bool verifyProcPointerAgainstDecl(AST* lhsProcPtr, AST* decl, const char* procName) {
    if (!lhsProcPtr || lhsProcPtr->type != AST_PROC_PTR_TYPE || !decl) {
        return true;
    }

    AST* lhsParams = (lhsProcPtr->child_count > 0) ? lhsProcPtr->children[0] : NULL;
    int lhsCount = lhsParams ? lhsParams->child_count : 0;
    int declCount = decl->child_count;
    if (lhsCount != declCount) {
        fprintf(stderr,
                "Type error: proc pointer arity mismatch for '%s' (expected %d, got %d).\n",
                procName, lhsCount, declCount);
        pascal_semantic_error_count++;
        return false;
    }

    for (int i = 0; i < declCount; ++i) {
        AST* lhsParam = lhsParams ? lhsParams->children[i] : NULL;
        AST* declParam = decl->children[i];
        if (!lhsParam || !declParam) {
            continue;
        }
        bool lhsByRef = procPointerParamByRef(lhsParam);
        bool declByRef = procPointerParamByRef(declParam);
        if (lhsByRef != declByRef) {
            fprintf(stderr,
                    "Type error: proc pointer param %lld passing convention mismatch for '%s' (expected %s, got %s).\n",
                    (long long)i + 1, procName,
                    lhsByRef ? "VAR/OUT" : "value",
                    declByRef ? "VAR/OUT" : "value");
            pascal_semantic_error_count++;
            return false;
        }
        VarType lhsType = procPointerParamType(lhsParam);
        VarType declType = procPointerParamType(declParam);
        if (lhsType != declType) {
            fprintf(stderr,
                    "Type error: proc pointer param %lld type mismatch for '%s' (expected %s, got %s).\n",
                    (long long)i + 1, procName,
                    varTypeToString(lhsType),
                    varTypeToString(declType));
            pascal_semantic_error_count++;
            return false;
        }
    }

    AST* lhsRet = lhsProcPtr->right;
    AST* declRet = decl->right;
    VarType lhsRetType = lhsRet ? lhsRet->var_type : TYPE_VOID;
    VarType declRetType = declRet ? declRet->var_type : TYPE_VOID;
    if (lhsRetType != declRetType) {
        fprintf(stderr,
                "Type error: proc pointer return type mismatch for '%s' (expected %s, got %s).\n",
                procName,
                varTypeToString(lhsRetType),
                varTypeToString(declRetType));
        pascal_semantic_error_count++;
        return false;
    }
    return true;
}

static bool verifyProcPointerTypesCompatible(AST* lhsProcPtr, AST* rhsProcPtr) {
    if (!lhsProcPtr || lhsProcPtr->type != AST_PROC_PTR_TYPE ||
        !rhsProcPtr || rhsProcPtr->type != AST_PROC_PTR_TYPE) {
        return true;
    }

    AST* lhsParams = (lhsProcPtr->child_count > 0) ? lhsProcPtr->children[0] : NULL;
    AST* rhsParams = (rhsProcPtr->child_count > 0) ? rhsProcPtr->children[0] : NULL;
    if (!compareProcPointerParams(lhsParams, rhsParams, NULL)) {
        return false;
    }

    AST* lhsRet = lhsProcPtr->right;
    AST* rhsRet = rhsProcPtr->right;
    VarType lhsRetType = lhsRet ? lhsRet->var_type : TYPE_VOID;
    VarType rhsRetType = rhsRet ? rhsRet->var_type : TYPE_VOID;
    if (lhsRetType != rhsRetType) {
        fprintf(stderr,
                "Type error: proc pointer return type mismatch in assignment (expected %s, got %s).\n",
                varTypeToString(lhsRetType),
                varTypeToString(rhsRetType));
        pascal_semantic_error_count++;
        return false;
    }

    return true;
}

AST *newASTNode(ASTNodeType type, Token *token) {
    AST *node = malloc(sizeof(AST));
    if (!node) { fprintf(stderr, "Memory allocation error in new_ast_node\n"); EXIT_FAILURE_HANDLER(); }

    // Ensure token is copied correctly, handling NULL
    node->token = token ? copyToken(token) : NULL;
    if (token && !node->token) { // Check if copyToken failed
        fprintf(stderr, "Memory allocation error copying token in newASTNode\n");
        free(node); // Free the partially allocated node
        EXIT_FAILURE_HANDLER();
    }

    setTypeAST(node, TYPE_VOID); // Default type
    node->by_ref = 0;
    node->left = node->right = node->extra = node->parent = NULL;
    node->children = NULL;
    node->child_count = 0;
    node->child_capacity = 0;
    node->type = type;
    node->is_global_scope = false;
    node->is_inline = false;
    node->is_forward_decl = false;
    node->is_virtual = false;
    node->i_val = 0; // Initialize i_val
    node->symbol_table = NULL; // Initialize symbol_table
    node->unit_list = NULL; // Initialize unit_list
    node->type_def = NULL; // Initialize type definition link
    node->freed = false; // Guard flag must start false

    return node;
}

AST *newThreadSpawn(AST *call) {
    AST *node = newASTNode(AST_THREAD_SPAWN, NULL);
    setLeft(node, call);
    return node;
}

AST *newThreadJoin(AST *expr) {
    AST *node = newASTNode(AST_THREAD_JOIN, NULL);
    setLeft(node, expr);
    return node;
}

AST *newLabelDeclaration(Token *labelToken) {
    return newASTNode(AST_LABEL_DECL, labelToken);
}

AST *newLabelStatement(Token *labelToken, AST *statement) {
    AST *node = newASTNode(AST_LABEL, labelToken);
    if (statement && statement->type != AST_NOOP) {
        setLeft(node, statement);
    } else if (statement) {
        freeAST(statement);
    }
    return node;
}

AST *newGotoStatement(Token *labelToken) {
    return newASTNode(AST_GOTO, labelToken);
}

#ifdef DEBUG
#define MAX_DEBUG_DEPTH 50
void debugAST(AST *node, int indent) {
    if (!node) return;
    if (indent > MAX_DEBUG_DEPTH) {
        for (int i = 0; i < indent; i++) printf("  ");
        printf("... (Max recursion depth %d reached in debugAST)\n", MAX_DEBUG_DEPTH);
        return;
    }
    for (int i = 0; i < indent; i++) printf("  ");
    printf("Node(type=%s", astTypeToString(node->type));
    if (node->token && node->token->value)
        printf(", token=\"%s\"", node->token->value);
    printf(", var_type=%s", varTypeToString(node->var_type));
    printf(")\n");

    if (node->left) {
        for (int i = 0; i < indent+1; i++) { printf("  "); }
        printf("Left:\n");
        debugAST(node->left, indent + 2);
    }
    if (node->right) {
        for (int i = 0; i < indent+1; i++) { printf("  "); }
        printf("Right:\n");
        debugAST(node->right, indent + 2);
    }
    if (node->extra) {
        for (int i = 0; i < indent+1; i++) { printf("  "); }
        printf("Extra:\n");
        debugAST(node->extra, indent + 2);
    }
    if (node->children && node->child_count > 0) {
        for (int i = 0; i < indent+1; i++) { printf("  "); }
        printf("Children (%d):\n", node->child_count);
        for (int i = 0; i < node->child_count; i++) {
            debugAST(node->children[i], indent + 2);
        }
    }
}
#endif // DEBUG

void addChild(AST *parent, AST *child) {
    if (!parent) {
        return;
    }

    if (parent->child_capacity == 0) {
        parent->child_capacity = 4;
        parent->children = malloc(sizeof(AST*) * parent->child_capacity);
        if (!parent->children) { fprintf(stderr, "Memory allocation error in addChild\n"); EXIT_FAILURE_HANDLER(); }
        for (int i = 0; i < parent->child_capacity; ++i) parent->children[i] = NULL;
    } else if (parent->child_count >= parent->child_capacity) {
        int old_capacity = parent->child_capacity;
        parent->child_capacity *= 2;
        parent->children = realloc(parent->children, sizeof(AST*) * parent->child_capacity);
        if (!parent->children) { fprintf(stderr, "Memory allocation error in addChild (realloc)\n"); EXIT_FAILURE_HANDLER(); }
        for (int i = old_capacity; i < parent->child_capacity; ++i) parent->children[i] = NULL;
    }

    parent->children[parent->child_count++] = child;
    if (child) {
        child->parent = parent;
    }
}

void setLeft(AST *parent, AST *child) {
    if (!parent) return;
    parent->left = child;
    if (child) {
#ifdef DEBUG
        fprintf(stderr, "[setLeft] Parent: %p (Type: %s), Child: %p (Type: %s). Setting child->parent to %p.\n",
                (void*)parent, astTypeToString(parent->type),
                (void*)child, astTypeToString(child->type),
                (void*)parent);
        fflush(stderr);
#endif
        child->parent = parent;
    }
}

void setRight(AST *parent, AST *child) {
    if (!parent) return;
    parent->right = child;
    if (child) {
#ifdef DEBUG
        fprintf(stderr, "[setRight] Parent: %p (Type: %s), Child: %p (Type: %s). Setting child->parent to %p.\n",
                (void*)parent, astTypeToString(parent->type),
                (void*)child, astTypeToString(child->type),
                (void*)parent);
        fflush(stderr);
#endif
        child->parent = parent;
    }
}
void setExtra(AST *parent, AST *child) {
    if (!parent) return;
    parent->extra = child;
    if (child) child->parent = parent;
}

void freeAST(AST *node) {
    if (!node) return;

    static AST** freed_nodes = NULL;
    static size_t freed_count = 0;
    static size_t freed_capacity = 0;
    for (size_t i = 0; i < freed_count; i++) {
        if (freed_nodes[i] == node) {
            return;
        }
    }

    if (node->freed) {
        if (freed_count == freed_capacity) {
            size_t new_cap = freed_capacity == 0 ? 64 : freed_capacity * 2;
            AST** new_buf = realloc(freed_nodes, new_cap * sizeof(AST*));
            if (!new_buf) return;
            freed_nodes = new_buf;
            freed_capacity = new_cap;
        }
        freed_nodes[freed_count++] = node;
        return;
    }
    node->freed = true;

    if (isNodeInTypeTable(node)) {
        return;
    }

    bool skip_left_free = (node->type == AST_TYPE_DECL);
    bool skip_right_free = (node->type == AST_TYPE_REFERENCE);

    if (node->left) {
        if (!skip_left_free) freeAST(node->left);
        node->left = NULL;
    }
    if (node->right) {
        if (!skip_right_free) freeAST(node->right);
        node->right = NULL;
    }
    if (node->extra) {
        freeAST(node->extra);
        node->extra = NULL;
    }
    if (node->children) {
        for (int i = 0; i < node->child_count; i++) {
            if (node->children[i]) freeAST(node->children[i]);
            node->children[i] = NULL;
        }
        free(node->children);
        node->children = NULL;
        node->child_count = 0;
        node->child_capacity = 0;
    }

    if (node->type == AST_USES_CLAUSE && node->unit_list) {
        freeList(node->unit_list);
        node->unit_list = NULL;
    }
    if (node->type == AST_UNIT && node->symbol_table) {
        node->symbol_table = NULL; // Should be freed by freeUnitSymbolTable
    }

    if (node->token) {
        freeToken(node->token);
        node->token = NULL;
    }
    if (freed_count == freed_capacity) {
        size_t new_cap = freed_capacity == 0 ? 64 : freed_capacity * 2;
        AST** new_buf = realloc(freed_nodes, new_cap * sizeof(AST*));
        if (!new_buf) {
            free(node);
            return;
        }
        freed_nodes = new_buf;
        freed_capacity = new_cap;
    }
    freed_nodes[freed_count++] = node;
    free(node);
}

void dumpASTFromRoot(AST *node) {
    printf("===== Dumping AST From Root START =====\n");
    if (!node) return;
    while (node->parent != NULL) {
        node = node->parent;
    }
    dumpAST(node, 0);
    printf("===== Dumping AST From Root END =====\n");
}

static void printIndent(int indent) { // Kept your original printIndent for textual dump
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
}

void dumpAST(AST *node, int indent) { // This is your original textual dump
    if (node == NULL) return;
    printIndent(indent);
    printf("Node(type=%s", astTypeToString(node->type));
    if (node->token && node->token->value)
        printf(", token=\"%s\"", node->token->value);
    printf(", var_type=%s", varTypeToString(node->var_type));
    printf(")\n");

    if (node->left) {
        printIndent(indent + 1);
        printf("Left:\n");
        dumpAST(node->left, indent + 2);
    }
    if (node->right) {
        printIndent(indent + 1);
        printf("Right:\n");
        dumpAST(node->right, indent + 2);
    }
    if (node->extra) {
        printIndent(indent + 1);
        printf("Extra:\n");
        dumpAST(node->extra, indent + 2);
    }
    if (node->children && node->child_count > 0) {
        printIndent(indent + 1);
        printf("Children (%d):\n", node->child_count);
        for (int i = 0; i < node->child_count; i++) {
            printIndent(indent + 2);
            printf("Child[%d]:\n", i);
            dumpAST(node->children[i], indent + 3);
        }
    }
    printf("Node(type=%s", astTypeToString(node->type));
    if (node->token && node->token->value)
        printf(", token=\"%s\"", node->token->value);
    printf(", var_type=%s", varTypeToString(node->var_type));
    if (node->type == AST_BOOLEAN || node->type == AST_NUMBER || node->type == AST_ENUM_VALUE) { // Add AST_BOOLEAN here
        printf(", i_val=%d", node->i_val); // Print i_val for these types
    }
    printf(")\n");
}

void setTypeAST(AST *node, VarType type) {
    if (!node) {
        fprintf(stderr, "Internal error: setTypeAST called with NULL node.\n");
        return;
    }
    node->var_type = type;
}

static bool constDeclMatches(AST* node, const char* varName) {
    return node && node->type == AST_CONST_DECL &&
           node->token && node->token->value &&
           strcasecmp(node->token->value, varName) == 0;
}

static int declarationLine(AST* decl) {
    if (!decl) return 0;
    if (decl->token) return decl->token->line;
    if (decl->child_count > 0 && decl->children) {
        for (int i = 0; i < decl->child_count; i++) {
            AST* child = decl->children[i];
            if (!child) continue;
            if (child->token) return child->token->line;
            if (child->left && child->left->token) return child->left->token->line;
            if (child->right && child->right->token) return child->right->token->line;
        }
    }
    if (decl->left && decl->left->token) return decl->left->token->line;
    if (decl->right && decl->right->token) return decl->right->token->line;
    return 0;
}

static AST* matchVarDecl(AST* varDeclGroup, const char* varName);

AST* findDeclarationInScope(const char* varName, AST* currentScopeNode, AST* referenceNode) {
    if (!currentScopeNode || !varName || !referenceNode) return NULL;

    int referenceLine = 0;
    if (referenceNode->token) {
        referenceLine = referenceNode->token->line;
    }

    AST* node = referenceNode;
    if (node && node == currentScopeNode) {
        node = node->parent;
    }
    while (node && node != currentScopeNode) {
        AST* parent = node->parent;
        if (parent && parent->type == AST_COMPOUND) {
            for (int i = 0; i < parent->child_count; i++) {
                AST* sibling = parent->children[i];
                if (sibling == node) break;
                if (!sibling) continue;
                if (sibling->type == AST_VAR_DECL) {
                    AST* found = matchVarDecl(sibling, varName);
                    if (found) return found;
                } else if (constDeclMatches(sibling, varName)) {
                    return sibling;
                }
            }
        }
        node = parent;
    }

    if (currentScopeNode->type == AST_COMPOUND) {
        return NULL;
    }

    if (currentScopeNode->type != AST_PROCEDURE_DECL &&
        currentScopeNode->type != AST_FUNCTION_DECL) {
        return NULL;
    }

    bool scanningParameters = (referenceNode == currentScopeNode);
    for (int i = 0; i < currentScopeNode->child_count; i++) {
        AST* paramDeclGroup = currentScopeNode->children[i];
        if (!paramDeclGroup) continue;
        if (paramDeclGroup->type == AST_VAR_DECL) {
            AST* found = matchVarDecl(paramDeclGroup, varName);
            if (found) {
                if (!scanningParameters && referenceLine > 0) {
                    int declLine = declarationLine(paramDeclGroup);
                    if (declLine > referenceLine) continue;
                }
                return paramDeclGroup;
            }
        } else if (constDeclMatches(paramDeclGroup, varName)) {
            if (referenceLine > 0) {
                int declLine = declarationLine(paramDeclGroup);
                if (declLine > referenceLine) continue;
            }
            return paramDeclGroup;
        }
    }

    if (currentScopeNode->type == AST_FUNCTION_DECL) {
        if (strcasecmp(currentScopeNode->token->value, varName) == 0 ||
            strcasecmp("result", varName) == 0) {
            return currentScopeNode;
        }
    }

    AST* blockNode = (currentScopeNode->type == AST_PROCEDURE_DECL)
                         ? currentScopeNode->right
                         : currentScopeNode->extra;
    if (blockNode && blockNode->type == AST_BLOCK && blockNode->child_count > 0) {
        AST* declarationsNode = blockNode->children[0];
        if (declarationsNode && declarationsNode->type == AST_COMPOUND) {
            for (int i = 0; i < declarationsNode->child_count; i++) {
                AST* varDeclGroup = declarationsNode->children[i];
                if (!varDeclGroup) continue;
                if (varDeclGroup->type == AST_VAR_DECL) {
                    AST* found = matchVarDecl(varDeclGroup, varName);
                    if (found) {
                        if (referenceLine > 0) {
                            int declLine = declarationLine(varDeclGroup);
                            if (declLine > referenceLine) continue;
                        }
                        return varDeclGroup;
                    }
                } else if (constDeclMatches(varDeclGroup, varName)) {
                    if (referenceLine > 0) {
                        int declLine = declarationLine(varDeclGroup);
                        if (declLine > referenceLine) continue;
                    }
                    return varDeclGroup;
                }
            }
        }
    }

    return NULL;
}

static AST* matchVarDecl(AST* varDeclGroup, const char* varName) {
    for (int j = 0; j < varDeclGroup->child_count; j++) {
        AST* varNameNode = varDeclGroup->children[j];
        if (!varNameNode) continue;
        if (varNameNode->type == AST_VARIABLE && varNameNode->token &&
            strcasecmp(varNameNode->token->value, varName) == 0) {
            return varDeclGroup;
        } else if (varNameNode->type == AST_ASSIGN && varNameNode->left &&
                   varNameNode->left->type == AST_VARIABLE &&
                   varNameNode->left->token &&
                   strcasecmp(varNameNode->left->token->value, varName) == 0) {
            return varDeclGroup;
        }
    }
    return NULL;
}

static AST* findStaticDeclarationInASTWithRef(const char* varName, AST* currentScopeNode, AST* referenceNode, AST* globalProgramNode) {
     if (!varName) return NULL;
     AST* foundDecl = NULL;

     int referenceLine = 0;
     if (referenceNode && referenceNode->token) {
         referenceLine = referenceNode->token->line;
     }

     // First, check for the identifier in the global symbol table. This will find enums.
     Symbol* sym = lookupGlobalSymbol(varName);
     if (sym) {
         // If a symbol is found, return its type definition AST node.
         // For enums, this correctly points back to the AST_ENUM_TYPE node.
         return sym->type_def;
     }

    // First, search within the current scope (procedure/function) where the
    // reference occurs. This finds parameters and locals declared in the
    // scope's declarations block.
    if (currentScopeNode) {
        // Quick path: directly scan the current scope's local declarations.
        if ((currentScopeNode->type == AST_PROCEDURE_DECL || currentScopeNode->type == AST_FUNCTION_DECL) &&
            currentScopeNode->right && currentScopeNode->right->type == AST_BLOCK &&
            currentScopeNode->right->child_count > 0) {
            AST* declarationsNode = currentScopeNode->right->children[0];
            if (declarationsNode && declarationsNode->type == AST_COMPOUND) {
                for (int i = 0; i < declarationsNode->child_count && !foundDecl; i++) {
                    AST* varDeclGroup = declarationsNode->children[i];
                    if (!varDeclGroup) continue;
                    if (varDeclGroup->type == AST_VAR_DECL) {
                        for (int j = 0; j < varDeclGroup->child_count; j++) {
                            AST* nameNode = varDeclGroup->children[j];
                            if (!nameNode) continue;
                            if ((nameNode->type == AST_VARIABLE && nameNode->token && nameNode->token->value &&
                                 strcasecmp(nameNode->token->value, varName) == 0) ||
                                (nameNode->type == AST_ASSIGN && nameNode->left && nameNode->left->type == AST_VARIABLE &&
                                 nameNode->left->token && nameNode->left->token->value &&
                                 strcasecmp(nameNode->left->token->value, varName) == 0)) {
                                foundDecl = varDeclGroup;
                                break;
                            }
                        }
                    } else if (constDeclMatches(varDeclGroup, varName)) {
                        foundDecl = varDeclGroup;
                    }
                }
            }
        }

        // Fallback to the general-purpose scope walk if still not found.
        if (!foundDecl) {
        foundDecl = findDeclarationInScope(varName, currentScopeNode, referenceNode ? referenceNode : currentScopeNode);
        }

        // As an additional fallback (handles languages where declarations and
        // statements are interleaved inside a single COMPOUND), walk up to the
        // nearest COMPOUND ancestor of the reference node and scan all earlier
        // and later siblings for a matching VAR_DECL.
        if (!foundDecl && referenceNode) {
            AST* ancestor = referenceNode->parent;
            while (ancestor && ancestor != currentScopeNode) {
                if (ancestor->type == AST_COMPOUND) {
                    for (int i = 0; i < ancestor->child_count && !foundDecl; i++) {
                        AST* sibling = ancestor->children[i];
                        if (!sibling) continue;
                        if (sibling->type == AST_VAR_DECL) {
                            AST* m = matchVarDecl(sibling, varName);
                            if (m) {
                                if (referenceLine > 0) {
                                    int declLine = declarationLine(sibling);
                                    if (declLine > referenceLine) continue;
                                }
                                foundDecl = sibling; break;
                            }
                        } else if (constDeclMatches(sibling, varName)) {
                            if (referenceLine > 0) {
                                int declLine = declarationLine(sibling);
                                if (declLine > referenceLine) continue;
                            }
                            foundDecl = sibling;
                            break;
                        }
                    }
                }
                if (foundDecl) break;
                ancestor = ancestor->parent;
            }
        }

    }

    // If not found, walk outward and search enclosing procedure/function scopes.
    if (!foundDecl) {
        AST* parentScope = currentScopeNode ? currentScopeNode->parent : NULL;
        while (!foundDecl && parentScope) {
            if (parentScope->type == AST_PROCEDURE_DECL ||
                parentScope->type == AST_FUNCTION_DECL) {
                foundDecl = findDeclarationInScope(varName, parentScope, parentScope);
                if (foundDecl) break;
            }
            parentScope = parentScope->parent;
        }
    }

    if (!foundDecl && globalProgramNode && globalProgramNode->type == AST_PROGRAM) {
          if (globalProgramNode->right && globalProgramNode->right->type == AST_BLOCK && globalProgramNode->right->child_count > 0) {
              AST* globalDeclarationsNode = globalProgramNode->right->children[0];
              if (globalDeclarationsNode && globalDeclarationsNode->type == AST_COMPOUND) {
                   for (int i = 0; i < globalDeclarationsNode->child_count; i++) {
                       AST* declGroup = globalDeclarationsNode->children[i];
                       if (declGroup && declGroup->type == AST_VAR_DECL) {
                           for (int j = 0; j < declGroup->child_count; j++) {
                               AST* varNameNode = declGroup->children[j];
                               if (varNameNode) {
                                   if (varNameNode->token && varNameNode->type == AST_VARIABLE &&
                                       strcasecmp(varNameNode->token->value, varName) == 0) {
                                       if (referenceLine <= 0 || declarationLine(declGroup) <= referenceLine) {
                                           foundDecl = declGroup;
                                           goto found_static_decl;
                                       }
                                    } else if (varNameNode->type == AST_ASSIGN && varNameNode->left &&
                                                varNameNode->left->type == AST_VARIABLE && varNameNode->left->token &&
                                                strcasecmp(varNameNode->left->token->value, varName) == 0) {
                                       if (referenceLine <= 0 || declarationLine(declGroup) <= referenceLine) {
                                           foundDecl = declGroup;
                                           goto found_static_decl;
                                       }
                                    }
                                }
                            }
                       }
                        else if (declGroup && declGroup->type == AST_CONST_DECL) {
                             if (declGroup->token && strcasecmp(declGroup->token->value, varName) == 0) {
                                  if (referenceLine <= 0 || declarationLine(declGroup) <= referenceLine) {
                                      foundDecl = declGroup;
                                      goto found_static_decl;
                                  }
                             }
                       }
                   }
              }
          }
     }
 found_static_decl:;
     return foundDecl;
}

// Backwards-compatible wrapper retained for existing call sites.
AST* findStaticDeclarationInAST(const char* varName, AST* currentScopeNode, AST* globalProgramNode) {
    return findStaticDeclarationInASTWithRef(varName, currentScopeNode, currentScopeNode, globalProgramNode);
}

void annotateTypes(AST *node, AST *currentScopeNode, AST *globalProgramNode) {
    if (!node) return;

    AST *childScopeNode = currentScopeNode;
    if (node->type == AST_PROCEDURE_DECL || node->type == AST_FUNCTION_DECL) {
        childScopeNode = node;
    }

    if (node->type == AST_BLOCK) {
        // Preserve is_global_scope as set during parsing; do not override here.
    }

    if (node->left) annotateTypes(node->left, childScopeNode, globalProgramNode);
    if (node->right) annotateTypes(node->right, childScopeNode, globalProgramNode);
    if (node->extra) annotateTypes(node->extra, childScopeNode, globalProgramNode);
    for (int i = 0; i < node->child_count; ++i) {
         if(node->children && node->children[i]) {
              annotateTypes(node->children[i], childScopeNode, globalProgramNode);
         }
    }

    if (node->var_type == TYPE_VOID || node->var_type == TYPE_UNKNOWN) {
        switch(node->type) {
            case AST_ADDR_OF: {
                node->var_type = TYPE_POINTER;
                if (!node->left) {
                    fprintf(stderr, "Compiler Error: '@' missing operand.\n");
                    pascal_semantic_error_count++;
                    break;
                }

                bool operand_is_procedure = false;
                if (node->left->type == AST_VARIABLE && node->left->token && node->left->token->value) {
                    const char* name = node->left->token->value;
                    Symbol* procSym = resolveProcedureSymbolInScope(name, node, globalProgramNode);
                    if (procSym) {
                        operand_is_procedure = true;
                    }
                }

                if (!operand_is_procedure) {
                    AST* baseType = node->left->type_def ? resolveTypeAlias(node->left->type_def) : NULL;
                    if (!baseType) {
                        const char* builtinName = NULL;
                        switch (node->left->var_type) {
                            case TYPE_INT32:
                                builtinName = "integer";
                                break;
                            case TYPE_INT64:
                                builtinName = "int64";
                                break;
                            case TYPE_UINT64:
                                builtinName = "uint64";
                                break;
                            case TYPE_UINT32:
                                builtinName = "uint32";
                                break;
                            case TYPE_DOUBLE:
                            case TYPE_FLOAT:
                            case TYPE_LONG_DOUBLE:
                                builtinName = "real";
                                break;
                            case TYPE_BOOLEAN:
                                builtinName = "boolean";
                                break;
                            case TYPE_CHAR:
                                builtinName = "char";
                                break;
                            case TYPE_STRING:
                                builtinName = "string";
                                break;
                            case TYPE_BYTE:
                                builtinName = "byte";
                                break;
                            case TYPE_WORD:
                                builtinName = "word";
                                break;
                            default:
                                break;
                        }
                        if (builtinName) {
                            AST* looked = lookupType(builtinName);
                            if (looked) {
                                baseType = looked;
                            }
                        }
                    }

                    if (baseType) {
                        AST* ptrNode = newASTNode(AST_POINTER_TYPE, NULL);
                        if (ptrNode) {
                            setTypeAST(ptrNode, TYPE_POINTER);
                            setRight(ptrNode, baseType);
                            node->type_def = ptrNode;
                        }
                    }
                }
                break;
            }
            case AST_TYPE_ASSERT: {
                AST *targetNode = node->right;
                AST *resolvedTarget = NULL;
                if (targetNode) {
                    if (targetNode->type_def) {
                        resolvedTarget = resolveTypeAlias(targetNode->type_def);
                    } else if (targetNode->right) {
                        resolvedTarget = resolveTypeAlias(targetNode->right);
                    }
                }
                if (!resolvedTarget) {
                    resolvedTarget = targetNode;
                }
                if (resolvedTarget) {
                    node->var_type = resolvedTarget->var_type;
                    node->type_def = resolvedTarget;
                }
                break;
            }
            case AST_VARIABLE: {
                if (node->parent && node->parent->type == AST_VAR_DECL) {
                    node->var_type = node->parent->var_type;
                    node->type_def = node->parent->right;
                    break;
                }
                const char* varName = node->token ? node->token->value : NULL;
                if (!varName) { node->var_type = TYPE_VOID; break; }

                // Rea: inside a class method, 'myself' refers to the implicit
                // receiver pointer. Type it as a POINTER so method calls that
                // expect a receiver of pointer type type-check correctly.
                if (strcasecmp(varName, "myself") == 0) {
                    node->var_type = TYPE_POINTER;
                    // Try to infer the class type from the current scope's name: Class.Method
                    AST* clsType = NULL;
                    if (childScopeNode && childScopeNode->token && childScopeNode->token->value) {
                        const char* fn = childScopeNode->token->value;
                        const char* dot = strchr(fn, '.');
                        if (dot && dot != fn) {
                            size_t len = (size_t)(dot - fn);
                            char cname[MAX_SYMBOL_LENGTH];
                            if (len >= sizeof(cname)) len = sizeof(cname) - 1;
                            memcpy(cname, fn, len);
                            cname[len] = '\0';
                            clsType = lookupType(cname);
                        }
                    }
                    // Build a pointer type node that points at the class record type if we found it.
                    if (clsType) {
                        AST* ptrNode = newASTNode(AST_POINTER_TYPE, NULL);
                        if (ptrNode) {
                            setRight(ptrNode, clsType);
                            node->type_def = ptrNode;
                        }
                    }
                    break;
                }

                // First, consult the global symbol table.  This handles variables
                // and constants imported from units via linkUnit.
                Symbol* sym = lookupGlobalSymbol(varName);
                if (sym) {
                    node->var_type = sym->type;
                    node->type_def = sym->type_def;
                } else {
                    AST* declNode = findStaticDeclarationInASTWithRef(varName, childScopeNode, node, globalProgramNode);
                    if (declNode) {
                        if (declNode->type == AST_ENUM_TYPE) {
                            node->var_type = TYPE_ENUM;
                            node->type_def = declNode;
                        } else if (declNode->type == AST_VAR_DECL) {
                            node->var_type = declNode->var_type;
                            node->type_def = declNode->right;
                        } else if (declNode->type == AST_CONST_DECL) {
                            node->var_type = declNode->var_type;
                            if (node->var_type == TYPE_VOID && declNode->left) {
                                node->var_type = declNode->left->var_type;
                            }
                            node->type_def = declNode->right;
                        } else if (declNode->type == AST_FUNCTION_DECL) {
                            if (declNode->right) node->var_type = declNode->right->var_type;
                            else node->var_type = TYPE_VOID;
                        } else {
                            node->var_type = TYPE_VOID;
                        }
                    } else {
                        // Attempt to resolve as a field of the enclosing class
                        AST* scope = childScopeNode;
                        while (scope && scope->type != AST_FUNCTION_DECL && scope->type != AST_PROCEDURE_DECL) {
                            scope = scope->parent;
                        }
                        const char* clsName = NULL;
                        if (scope && scope->token && scope->token->value) {
                            const char* fn = scope->token->value;
                            const char* dot = strchr(fn, '.');
                            if (dot) {
                                size_t len = (size_t)(dot - fn);
                                char tmp[MAX_SYMBOL_LENGTH];
                                if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
                                memcpy(tmp, fn, len);
                                tmp[len] = '\0';
                                clsName = tmp;
                                AST* ctype = lookupType(clsName);
                                ctype = resolveTypeAlias(ctype);
                                if (ctype && ctype->type == AST_RECORD_TYPE) {
                                    for (int i = 0; i < ctype->child_count; i++) {
                                        AST* f = ctype->children[i];
                                        if (!f || f->type != AST_VAR_DECL) continue;
                                        for (int j = 0; j < f->child_count; j++) {
                                            AST* v = f->children[j];
                                            if (v && v->token && strcmp(v->token->value, varName) == 0) {
                                                node->var_type = f->var_type;
                                                node->type_def = f->right;
                                                goto resolved_field;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        {
                            AST* typeDef = lookupType(varName);
                            if (typeDef) {
                                node->var_type = typeDef->var_type;
                                node->type_def = typeDef;
                                #ifdef DEBUG
                                fprintf(stderr, "[Annotate Warning] Type identifier '%s' used directly in expression?\n", varName);
                                #endif
                            } else {
                                #ifdef DEBUG
                                if (currentScopeNode != globalProgramNode || (globalProgramNode && globalProgramNode->left != node)) {
                                    fprintf(stderr, "[Annotate Warning] Undeclared identifier '%s' used in expression.\n", varName);
                                }
                                #endif
                                node->var_type = TYPE_VOID;
                            }
                        }
resolved_field: ;
                    }
                }
                if (strcasecmp(varName, "result") == 0 && childScopeNode && childScopeNode->type == AST_FUNCTION_DECL) {
                    if (childScopeNode->right) node->var_type = childScopeNode->right->var_type;
                    else node->var_type = TYPE_VOID;
                }
                break;
            }
            case AST_BINARY_OP: {
                VarType leftType = node->left ? node->left->var_type : TYPE_VOID;
                VarType rightType = node->right ? node->right->var_type : TYPE_VOID;
                TokenType op = node->token ? node->token->type : TOKEN_UNKNOWN;

                if (op == TOKEN_EQUAL || op == TOKEN_NOT_EQUAL || op == TOKEN_LESS ||
                    op == TOKEN_LESS_EQUAL || op == TOKEN_GREATER || op == TOKEN_GREATER_EQUAL ||
                    op == TOKEN_IN) {
                    node->var_type = TYPE_BOOLEAN;
                }
                else if (op == TOKEN_AND || op == TOKEN_OR ) {
                    if (leftType == TYPE_INTEGER && rightType == TYPE_INTEGER) {
                        node->var_type = TYPE_INTEGER;
                    } else {
                        node->var_type = TYPE_BOOLEAN; // Default to boolean for mixed or boolean types
                    }
                }
                else if (op == TOKEN_SLASH) {
                    if (leftType == TYPE_LONG_DOUBLE || rightType == TYPE_LONG_DOUBLE) {
                        node->var_type = TYPE_LONG_DOUBLE;
                    } else if (leftType == TYPE_DOUBLE || rightType == TYPE_DOUBLE) {
                        node->var_type = TYPE_DOUBLE;
                    } else if (leftType == TYPE_FLOAT || rightType == TYPE_FLOAT) {
                        node->var_type = TYPE_FLOAT;
                    } else {
                        node->var_type = TYPE_DOUBLE; // Default for integer / integer
                    }
                }
                else if (isRealType(leftType) || isRealType(rightType)) {
                    if (leftType == TYPE_LONG_DOUBLE || rightType == TYPE_LONG_DOUBLE) {
                        node->var_type = TYPE_LONG_DOUBLE;
                    } else if (leftType == TYPE_DOUBLE || rightType == TYPE_DOUBLE) {
                        node->var_type = TYPE_DOUBLE;
                    } else {
                        node->var_type = TYPE_FLOAT;
                    }
                }
                else if (op == TOKEN_PLUS && (leftType == TYPE_STRING || rightType == TYPE_STRING || leftType == TYPE_CHAR || rightType == TYPE_CHAR)) {
                    node->var_type = TYPE_STRING;
                }
                else if (leftType == TYPE_INTEGER && rightType == TYPE_INTEGER) {
                    node->var_type = TYPE_INTEGER;
                }
                else {
                    node->var_type = TYPE_VOID;
                }
                break;
            }
            case AST_TERNARY: {
                VarType thenType = node->right ? node->right->var_type : TYPE_UNKNOWN;
                VarType elseType = node->extra ? node->extra->var_type : TYPE_UNKNOWN;

                if (thenType == TYPE_POINTER || elseType == TYPE_POINTER) {
                    node->var_type = TYPE_POINTER;
                } else if (isRealType(thenType) && isIntlikeType(elseType)) {
                    node->var_type = thenType;
                } else if (isRealType(elseType) && isIntlikeType(thenType)) {
                    node->var_type = elseType;
                } else if (isRealType(thenType) && isRealType(elseType)) {
                    if (thenType == TYPE_LONG_DOUBLE || elseType == TYPE_LONG_DOUBLE) node->var_type = TYPE_LONG_DOUBLE;
                    else if (thenType == TYPE_DOUBLE || elseType == TYPE_DOUBLE) node->var_type = TYPE_DOUBLE;
                    else node->var_type = TYPE_FLOAT;
                } else if (thenType == TYPE_STRING || elseType == TYPE_STRING) {
                    node->var_type = TYPE_STRING;
                } else if (thenType == TYPE_BOOLEAN && elseType == TYPE_BOOLEAN) {
                    node->var_type = TYPE_BOOLEAN;
                } else if (thenType != TYPE_UNKNOWN && thenType != TYPE_VOID) {
                    node->var_type = thenType;
                } else {
                    node->var_type = elseType;
                }

                AST *preferredTypeDef = NULL;
                if (node->var_type == TYPE_POINTER) {
                    AST *thenDef = (node->right && node->right->var_type == TYPE_POINTER) ? node->right->type_def : NULL;
                    AST *elseDef = (node->extra && node->extra->var_type == TYPE_POINTER) ? node->extra->type_def : NULL;
                    if (thenDef && elseDef) {
                        AST *resolvedThen = resolveTypeAlias(thenDef);
                        AST *resolvedElse = resolveTypeAlias(elseDef);
                        if (resolvedThen && resolvedElse && resolvedThen->type == resolvedElse->type) {
                            if (resolvedThen->type == AST_POINTER_TYPE) {
                                AST *thenTarget = resolveTypeAlias(resolvedThen->right);
                                AST *elseTarget = resolveTypeAlias(resolvedElse->right);
                                if (thenTarget == elseTarget || !elseTarget) {
                                    preferredTypeDef = thenDef;
                                } else if (!thenTarget) {
                                    preferredTypeDef = elseDef;
                                } else {
                                    preferredTypeDef = thenDef;
                                }
                            } else {
                                preferredTypeDef = thenDef;
                            }
                        } else {
                            preferredTypeDef = thenDef;
                        }
                    } else {
                        preferredTypeDef = thenDef ? thenDef : elseDef;
                    }
                } else {
                    if (node->right && node->right->type_def && node->right->var_type == node->var_type) {
                        preferredTypeDef = node->right->type_def;
                    }
                    if ((!preferredTypeDef || preferredTypeDef == NULL) && node->extra && node->extra->type_def && node->extra->var_type == node->var_type) {
                        preferredTypeDef = node->extra->type_def;
                    }
                }

                if (preferredTypeDef) {
                    node->type_def = preferredTypeDef;
                }
                break;
            }
            case AST_UNARY_OP:
                node->var_type = (node->token && node->token->type == TOKEN_NOT) ? TYPE_BOOLEAN : (node->left ? node->left->var_type : TYPE_VOID);
                break;
            case AST_PROCEDURE_CALL: {
                 Symbol *procSymbol = node->token ? resolveProcedureSymbolInScope(node->token->value, node, globalProgramNode) : NULL;
                if (procSymbol) {
                    node->var_type = procSymbol->type;
                } else {
                    if (node->token) {
                         node->var_type = getBuiltinReturnType(node->token->value);
                          if (node->var_type == TYPE_VOID && isBuiltin(node->token->value)) {
                              // Known built-in procedure
                          } else if (node->var_type == TYPE_VOID) {
                               #ifdef DEBUG
                               fprintf(stderr, "[Annotate Warning] Call to undeclared procedure/function '%s'.\n", node->token->value);
                               #endif
                          }
                     } else {
                          node->var_type = TYPE_VOID;
                     }
                 }
                // Minimal argument checking for procedure-pointer parameters
                 if (procSymbol && procSymbol->type_def && node->child_count > 0) {
                     AST* decl = procSymbol->type_def; // PROCEDURE_DECL or FUNCTION_DECL
                     int expected = decl->child_count;
                     int given = node->child_count;
                     if (given >= expected) {
                         for (int i = 0; i < expected; ++i) {
                             AST* formal = decl->children[i];
                             AST* actual = node->children[i];
                             if (!formal || !actual) continue;
                             AST* ftype = resolveTypeAlias(formal->right); // type node
                             if (ftype && ftype->type == AST_PROC_PTR_TYPE) {
                                 if (actual->type == AST_ADDR_OF && actual->left && actual->left->token) {
                                     const char* aname = actual->left->token->value;
                                     Symbol* as = resolveProcedureSymbolInScope(aname, node, globalProgramNode);
                                     if (as && as->type_def) {
                                         AST* adecl = as->type_def;
                                         AST* fparams = (ftype->child_count > 0) ? ftype->children[0] : NULL;
                                         int fpc = fparams ? fparams->child_count : 0;
                                         int apc = adecl->child_count;
                                         if (fpc != apc) {
                                             fprintf(stderr, "Type error: proc pointer arity mismatch for '%s' (expected %d, got %d).\n", aname, fpc, apc);
                                             pascal_semantic_error_count++;
                                         } else {
                                             for (int j = 0; j < fpc; ++j) {
                                                 AST* ft = fparams->children[j];
                                                 AST* at = adecl->children[j];
                                                 if (ft && at && ft->var_type != at->var_type) {
                                                    fprintf(stderr, "Type error: proc pointer param %lld type mismatch for '%s' (expected %s, got %s).\n",
                                                            (long long)j + 1, aname,
                                                            varTypeToString(ft->var_type),
                                                            varTypeToString(at->var_type));
                                                     pascal_semantic_error_count++;
                                                     break;
                                                 }
                                             }
                                         }
                                         AST* fret = ftype->right; // may be NULL for procedure
                                         AST* aret = adecl->right; // may be NULL for procedure
                                         VarType fRT = fret ? fret->var_type : TYPE_VOID;
                                         VarType aRT = aret ? aret->var_type : TYPE_VOID;
                                         if (fRT != aRT) {
                                             fprintf(stderr, "Type error: proc pointer return type mismatch for '%s' (expected %s, got %s).\n", aname, varTypeToString(fRT), varTypeToString(aRT));
                                             pascal_semantic_error_count++;
                                         }
                                      } else {
                                         fprintf(stderr, "Type error: '@%s' does not name a known procedure or function.\n", aname);
                                         pascal_semantic_error_count++;
                                      }
                                  } else {
                                     fprintf(stderr, "Type error: expected '@proc' for procedure pointer argument.\n");
                                     pascal_semantic_error_count++;
                                  }
                              }
                          }
                      }
                 }
                // Note: language-specific frontends may handle receiver typing separately.
                /*
                 * Some builtins (e.g., succ/pred) return the same type as
                 * their first argument.  If no explicit return type was
                 * resolved above, infer it from the argument so enum literals
                 * retain their declared type.
                 */
                if (node->token && node->child_count > 0 && node->children[0]) {
                    const char* builtin_name = node->token->value;
                    if (strcasecmp(builtin_name, "succ") == 0 ||
                        strcasecmp(builtin_name, "pred") == 0 ||
                        strcasecmp(builtin_name, "low")  == 0 ||
                        strcasecmp(builtin_name, "high") == 0) {
                        AST* arg = node->children[0];
                        AST* resolved = NULL;
                        if (arg->token) {
                            resolved = lookupType(arg->token->value);
                        }
                        if (!resolved) {
                            resolved = arg->type_def;
                        }
                        resolved = resolveTypeAlias(resolved);
                        if (resolved) {
                            node->var_type = resolved->var_type;
                            node->type_def = resolved;
                        } else if (arg->token && arg->token->value) {
                            const char* tn = arg->token->value;
                            if      (strcasecmp(tn, "integer") == 0) node->var_type = TYPE_INTEGER;
                            else if (strcasecmp(tn, "char")    == 0) node->var_type = TYPE_CHAR;
                            else if (strcasecmp(tn, "boolean") == 0) node->var_type = TYPE_BOOLEAN;
                            else if (strcasecmp(tn, "byte")    == 0) node->var_type = TYPE_BYTE;
                            else if (strcasecmp(tn, "word")    == 0) node->var_type = TYPE_WORD;
                            else {
                                node->var_type = arg->var_type;
                                node->type_def = arg->type_def;
                            }
                        } else {
                            node->var_type = arg->var_type;
                            node->type_def = arg->type_def;
                        }
                    } else if (strcasecmp(builtin_name, "abs") == 0) {
                        AST* arg = node->children[0];
                        node->var_type = arg->var_type;
                        node->type_def = arg->type_def;
                    }
                }
                break;
            }
            case AST_FIELD_ACCESS: {
                node->var_type = TYPE_VOID;
                if (node->left && node->left->var_type == TYPE_RECORD && node->left->type_def) {
                    AST* record_definition_node = node->left->type_def;
                    if (record_definition_node->type == AST_TYPE_REFERENCE && record_definition_node->right) {
                        record_definition_node = record_definition_node->right;
                    }
                    while (record_definition_node && record_definition_node->type == AST_TYPE_REFERENCE && record_definition_node->right) {
                        record_definition_node = record_definition_node->right;
                    }
                    while (record_definition_node && record_definition_node->type == AST_RECORD_TYPE) {
                        const char* field_to_find = node->token ? node->token->value : NULL;
                        if (field_to_find) {
                            for (int i = 0; i < record_definition_node->child_count; i++) {
                                AST* field_decl_group = record_definition_node->children[i];
                                if (field_decl_group && field_decl_group->type == AST_VAR_DECL) {
                                    for (int j = 0; j < field_decl_group->child_count; j++) {
                                        AST* field_name_node = field_decl_group->children[j];
                                        if (field_name_node && field_name_node->token &&
                                            strcasecmp(field_name_node->token->value, field_to_find) == 0) {
                                            node->var_type = field_decl_group->var_type;
                                            node->type_def = field_decl_group->right;
                                            goto field_found_annotate;
                                        }
                                    }
                                }
                            }
                            // If not found, follow parent via record_definition_node->extra (TYPE_REFERENCE to parent)
                            AST* parent = record_definition_node->extra;
                            if (parent) {
                                // Resolve parent reference to actual type definition
                                AST* pref = parent;
                                if (pref->type == AST_TYPE_REFERENCE && pref->token && pref->token->value) {
                                    AST* looked = lookupType(pref->token->value);
                                    if (looked) {
                                        record_definition_node = looked;
                                        if (record_definition_node->type == AST_TYPE_REFERENCE && record_definition_node->right)
                                            record_definition_node = record_definition_node->right;
                                        // Continue while-loop to search parent
                                        continue;
                                    }
                                }
                            }
                            #ifdef DEBUG
                            fprintf(stderr, "[Annotate Warning] Field '%s' not found in record type '%s'.\n",
                                    field_to_find,
                                    node->left->token ? node->left->token->value : "UNKNOWN_RECORD");
                            #endif
                        }
                        // If we got here without continue, break out of while
                        break;
                    }
                } else if (node->left) { /* ... debug warnings ... */ }
                field_found_annotate:;
                break;
            }
            case AST_DEREFERENCE: {
                node->var_type = TYPE_VOID;
                node->type_def = NULL;
                if (node->left && node->left->type_def) {
                    AST *ptrType = resolveTypeAlias(node->left->type_def);
                    if (ptrType && ptrType->type == AST_POINTER_TYPE && ptrType->right) {
                        AST *baseType = resolveTypeAlias(ptrType->right);
                        if (baseType && baseType->type == AST_VARIABLE && baseType->token && baseType->token->value) {
                            AST *looked = lookupType(baseType->token->value);
                            if (looked) baseType = looked;
                        }
                        if (baseType) {
                            if (baseType->var_type == TYPE_VOID && baseType->token && baseType->token->value) {
                                const char* tn = baseType->token->value;
                                if      (strcasecmp(tn, "integer") == 0) baseType->var_type = TYPE_INTEGER;
                                else if (strcasecmp(tn, "real")    == 0) baseType->var_type = TYPE_REAL;
                                else if (strcasecmp(tn, "string")  == 0) baseType->var_type = TYPE_STRING;
                                else if (strcasecmp(tn, "char")    == 0) baseType->var_type = TYPE_CHAR;
                                else if (strcasecmp(tn, "boolean") == 0) baseType->var_type = TYPE_BOOLEAN;
                                else if (strcasecmp(tn, "byte")    == 0) baseType->var_type = TYPE_BYTE;
                                else if (strcasecmp(tn, "word")    == 0) baseType->var_type = TYPE_WORD;
                            }
                            node->var_type = baseType->var_type;
                            node->type_def = baseType;
                        }
                    }
                }
                break;
            }
            case AST_ARRAY_ACCESS: {
                node->var_type = TYPE_VOID;
                node->type_def = NULL;
                if (node->left) {
                    AST *arrayType = resolveTypeAlias(node->left->type_def);
                    if (arrayType && arrayType->type == AST_ARRAY_TYPE) {
                        AST *elemType = resolveTypeAlias(arrayType->right);
                        if (elemType) {
                            node->type_def = elemType;
                            if (elemType->type == AST_POINTER_TYPE) {
                                node->var_type = TYPE_POINTER;
                            } else {
                                node->var_type = elemType->var_type;
                            }
                        }
                    } else if (node->left->var_type == TYPE_STRING) {
                        // Special case: indexing into a string yields a char
                        node->var_type = TYPE_CHAR;
                        node->type_def = lookupType("char");
                    }
                }
                break;
            }
            // Adding types for literals if not set by parser (though parser usually does)
            case AST_NUMBER:
                node->var_type = (node->token && node->token->type == TOKEN_REAL_CONST) ? TYPE_REAL : TYPE_INTEGER;
                break;
            case AST_STRING:
                if (node->token && node->token->value) {
                    size_t literal_len = (node->i_val > 0) ? (size_t)node->i_val
                                                : strlen(node->token->value);
                    if (literal_len == 1) {
                        node->var_type = TYPE_CHAR;
                        node->type_def = lookupType("char");
                        break;
                    }
                }
                node->var_type = TYPE_STRING;
                break;
            case AST_BOOLEAN:
                node->var_type = TYPE_BOOLEAN;
                break;
            case AST_NIL:
                node->var_type = TYPE_NIL; // Or TYPE_POINTER if nil is a generic pointer type
                break;
            case AST_ASSIGN: {
                // Minimal semantic check: procedure-pointer assignment
                if (node->left && node->right) {
                    AST* lhs = node->left;
                    AST* rhs = node->right;
                    AST* lhsType = resolveTypeAlias(lhs->type_def);
                    if (!lhsType && lhs->token && lhs->token->value) {
                        Symbol *lhsProc = resolveProcedureSymbolInScope(lhs->token->value, node, globalProgramNode);
                        if (lhsProc && lhsProc->type_def) {
                            AST *retType = lhsProc->type_def->right;
                            lhsType = resolveTypeAlias(retType);
                        }
                    }
                    if (lhsType && lhsType->type == AST_PROC_PTR_TYPE) {
                        bool rhsIsProcPointer = false;
                        if (rhs->type == AST_ADDR_OF && rhs->left && rhs->left->token) {
                            const char* pname = rhs->left->token->value;
                            Symbol* psym = resolveProcedureSymbolInScope(pname, node, globalProgramNode);
                            if (psym && psym->type_def) {
                                rhsIsProcPointer = true;
                                verifyProcPointerAgainstDecl(lhsType, psym->type_def, pname);
                            } else {
                                fprintf(stderr, "Type error: '@%s' does not name a known procedure or function.\n", pname);
                                pascal_semantic_error_count++;
                            }
                        } else {
                            AST* rhsType = resolveTypeAlias(rhs->type_def);
                            if (!rhsType && rhs->type == AST_VARIABLE && rhs->token && rhs->token->value) {
                                Symbol* rhsSym = lookupSymbol(rhs->token->value);
                                if (rhsSym && rhsSym->type_def) {
                                    rhsType = resolveTypeAlias(rhsSym->type_def);
                                }
                            }

                            if (!rhsType && rhs->token && rhs->token->value && rhs->type == AST_PROCEDURE_CALL && rhs->child_count == 0) {
                                const char *callName = rhs->token->value;
                                Symbol* rhsProc = resolveProcedureSymbolInScope(callName, node, globalProgramNode);
                                if (rhsProc && rhsProc->type_def) {
                                    AST *resolvedProcType = resolveTypeAlias(rhsProc->type_def);
                                    AST *resolvedReturnType = resolvedProcType ?
                                            resolveTypeAlias(resolvedProcType->right) : NULL;

                                    bool returnIsProcPointer = resolvedReturnType &&
                                            resolvedReturnType->type == AST_PROC_PTR_TYPE;

                                    if (!returnIsProcPointer) {
                                        verifyProcPointerAgainstDecl(lhsType, rhsProc->type_def, callName);
                                    }

                                    if (returnIsProcPointer) {
                                        rhsIsProcPointer = true;
                                        rhs->var_type = TYPE_POINTER;
                                        rhs->type_def = resolvedReturnType;
                                        verifyProcPointerTypesCompatible(lhsType, resolvedReturnType);
                                    } else {
                                        rhsIsProcPointer = true;
                                        AST *designator = newASTNode(AST_VARIABLE, rhs->token);
                                        if (rhs->token) {
                                            freeToken(rhs->token);
                                            rhs->token = NULL;
                                        }
                                        rhs->type = AST_ADDR_OF;
                                        rhs->var_type = TYPE_POINTER;
                                        rhs->type_def = lhsType;
                                        setLeft(rhs, designator);
                                    }
                                }
                            }

                            if (!rhsIsProcPointer) {
                                if (rhsType && rhsType->type == AST_PROC_PTR_TYPE) {
                                    rhsIsProcPointer = true;
                                    verifyProcPointerTypesCompatible(lhsType, rhsType);
                                } else if (rhs->var_type == TYPE_POINTER) {
                                    rhsIsProcPointer = true;
                                }
                            }
                        }

                        if (!rhsIsProcPointer) {
                            fprintf(stderr, "Type error: expected '@proc' on right-hand side of proc pointer assignment.\n");
                            pascal_semantic_error_count++;
                        }
                    }
                }
                break;
            }
            default:
                 break;
        }
    }
}

VarType getBuiltinReturnType(const char* name) {
    if (!name) return TYPE_VOID;

    /*
     * This function is used as a fallback when a built-in routine was not
     * explicitly registered in the procedure table (for example, when running
     * the front-end tools directly).  Previously it only recognised a couple
     * of routines which meant many math functions defaulted to TYPE_VOID and
     * produced "UNKNOWN_VAR_TYPE" compile errors.  Populate the mapping with
     * the common built-ins so their return types can be inferred.
     */

    /* Character and ordinal helpers */
    if (strcasecmp(name, "chr")  == 0) return TYPE_CHAR;
    if (strcasecmp(name, "ord")  == 0) return TYPE_INTEGER;
    if (strcasecmp(name, "pollkey") == 0) return TYPE_INTEGER;

    /* CLike-style cast helpers */
    if (strcasecmp(name, "int") == 0 || strcasecmp(name, "toint") == 0) return TYPE_INT64;
    if (strcasecmp(name, "double") == 0 || strcasecmp(name, "todouble") == 0) return TYPE_DOUBLE;
    if (strcasecmp(name, "float") == 0 || strcasecmp(name, "tofloat") == 0) return TYPE_FLOAT;
    if (strcasecmp(name, "char") == 0 || strcasecmp(name, "tochar") == 0) return TYPE_CHAR;
    if (strcasecmp(name, "bool") == 0 || strcasecmp(name, "tobool") == 0) return TYPE_BOOLEAN;

    if (strcasecmp(name, "realtimeclock") == 0) return TYPE_DOUBLE;

    /* Math routines returning REAL */
    if (strcasecmp(name, "cos")   == 0 ||
        strcasecmp(name, "sin")   == 0 ||
        strcasecmp(name, "tan")   == 0 ||
        strcasecmp(name, "sqrt")  == 0 ||
        strcasecmp(name, "ln")    == 0 ||
        strcasecmp(name, "exp")   == 0 ||
        strcasecmp(name, "real")  == 0 ||
        strcasecmp(name, "arctan") == 0 ||
        strcasecmp(name, "arctan2") == 0 ||
        strcasecmp(name, "atan2")  == 0 ||
        strcasecmp(name, "arcsin") == 0 ||
        strcasecmp(name, "arccos") == 0 ||
        strcasecmp(name, "cotan")  == 0 ||
        strcasecmp(name, "power")  == 0 ||
        strcasecmp(name, "log10")  == 0 ||
        strcasecmp(name, "sinh")   == 0 ||
        strcasecmp(name, "cosh")   == 0 ||
        strcasecmp(name, "tanh")   == 0 ||
        strcasecmp(name, "max")    == 0 ||
        strcasecmp(name, "min")    == 0) {
        return TYPE_REAL;
    }

    /* Math routines returning INTEGER */
    /*
     * `abs` is intentionally omitted here because it returns the same
     * type as its argument.  Its return type is inferred during AST
     * annotation based on the provided parameter.
     */
    if (strcasecmp(name, "round")     == 0 ||
        strcasecmp(name, "trunc")     == 0 ||
        strcasecmp(name, "random")    == 0 ||
        strcasecmp(name, "ioresult")  == 0 ||
        strcasecmp(name, "filesize")  == 0 ||
        strcasecmp(name, "paramcount")== 0 ||
        strcasecmp(name, "length")    == 0 ||
        strcasecmp(name, "pos")       == 0 ||
        strcasecmp(name, "sizeof")    == 0 ||
        strcasecmp(name, "screencols")== 0 ||
        strcasecmp(name, "screenrows")== 0 ||
        strcasecmp(name, "wherex")    == 0 ||
        strcasecmp(name, "wherey")    == 0 ||
        strcasecmp(name, "getmaxx")   == 0 ||
        strcasecmp(name, "getmaxy")   == 0 ||
        strcasecmp(name, "mutex")     == 0 ||
        strcasecmp(name, "rcmutex")   == 0 ||
        strcasecmp(name, "floor")     == 0 ||
        strcasecmp(name, "ceil")      == 0) {
        return TYPE_INTEGER;
    }

    /* String producing helpers */
    if (strcasecmp(name, "formatfloat") == 0 ||
        strcasecmp(name, "inttostr")  == 0 ||
        strcasecmp(name, "realtostr") == 0 ||
        strcasecmp(name, "paramstr")  == 0 ||
        strcasecmp(name, "copy")      == 0 ||
        strcasecmp(name, "mstreambuffer") == 0) {
        return TYPE_STRING;
    }

    /* Memory stream helpers */
    if (strcasecmp(name, "mstreamcreate") == 0) return TYPE_MEMORYSTREAM;
    if (strcasecmp(name, "mstreamloadfromfile") == 0) return TYPE_BOOLEAN;

    /* Threading helpers (new API) */
    if (strcasecmp(name, "createthread") == 0) return TYPE_THREAD;
    if (strcasecmp(name, "waitforthread") == 0) return TYPE_INTEGER;

    /* HTTP session helpers */
    if (strcasecmp(name, "httpsession") == 0) return TYPE_INTEGER;
    if (strcasecmp(name, "httprequest") == 0) return TYPE_INTEGER;
    if (strcasecmp(name, "httprequesttofile") == 0) return TYPE_INTEGER;
    if (strcasecmp(name, "httprequestasync") == 0) return TYPE_INTEGER;
    if (strcasecmp(name, "httprequestasynctofile") == 0) return TYPE_INTEGER;
    if (strcasecmp(name, "httpisdone") == 0) return TYPE_INTEGER;
    if (strcasecmp(name, "httptryawait") == 0) return TYPE_INTEGER;
    if (strcasecmp(name, "httpcancel") == 0) return TYPE_INTEGER;
    if (strcasecmp(name, "httpgetasyncprogress") == 0) return TYPE_INTEGER;
    if (strcasecmp(name, "httpgetasynctotal") == 0) return TYPE_INTEGER;
    if (strcasecmp(name, "httpawait") == 0) return TYPE_INTEGER;
    if (strcasecmp(name, "httperrorcode") == 0) return TYPE_INTEGER;
    if (strcasecmp(name, "httpgetheader") == 0) return TYPE_STRING;
    if (strcasecmp(name, "httpgetlastheaders") == 0) return TYPE_STRING;
    if (strcasecmp(name, "httplasterror") == 0) return TYPE_STRING;

    /* ReadKey and UpCase return a single character */
    if (strcasecmp(name, "readkey") == 0 ||
        strcasecmp(name, "upcase")  == 0) {
        return TYPE_CHAR;
    }

    return TYPE_VOID;
}

typedef struct {
    AST *original;
    AST *copy;
} ASTCopyPair;

typedef struct {
    ASTCopyPair *pairs;
    size_t count;
    size_t capacity;
} ASTCopyContext;

static AST *lookupCopiedNode(ASTCopyContext *ctx, AST *original) {
    if (!ctx || !original) return NULL;
    for (size_t i = 0; i < ctx->count; ++i) {
        if (ctx->pairs[i].original == original) {
            return ctx->pairs[i].copy;
        }
    }
    return NULL;
}

static bool registerCopiedNode(ASTCopyContext *ctx, AST *original, AST *copy) {
    if (!ctx || !original || !copy) return false;
    if (ctx->count == ctx->capacity) {
        size_t newCap = ctx->capacity ? ctx->capacity * 2 : 32;
        ASTCopyPair *newPairs = realloc(ctx->pairs, newCap * sizeof(ASTCopyPair));
        if (!newPairs) {
            return false;
        }
        ctx->pairs = newPairs;
        ctx->capacity = newCap;
    }
    ctx->pairs[ctx->count].original = original;
    ctx->pairs[ctx->count].copy = copy;
    ctx->count++;
    return true;
}

static void popCopiedNode(ASTCopyContext *ctx) {
    if (ctx && ctx->count > 0) {
        ctx->count--;
    }
}

static AST *copyASTRecursive(AST *node, ASTCopyContext *ctx) {
    if (!node) return NULL;

    AST *memo = lookupCopiedNode(ctx, node);
    if (memo) {
        return memo;
    }

    AST *newNode = newASTNode(node->type, node->token);
    if (!newNode) {
        return NULL;
    }
    newNode->freed = false;

    newNode->var_type = node->var_type;
    newNode->by_ref = node->by_ref;
    newNode->is_global_scope = node->is_global_scope;
    newNode->is_inline = node->is_inline;
    newNode->is_forward_decl = node->is_forward_decl;
    newNode->is_virtual = node->is_virtual;
    newNode->i_val = node->i_val;
    newNode->unit_list = node->unit_list;
    newNode->symbol_table = node->symbol_table;
    newNode->type_def = NULL;

    if (!registerCopiedNode(ctx, node, newNode)) {
        freeAST(newNode);
        EXIT_FAILURE_HANDLER();
        return NULL;
    }

    AST *copiedLeft = copyASTRecursive(node->left, ctx);
    AST *copiedExtra = copyASTRecursive(node->extra, ctx);
    AST *copiedRight = NULL;

    if (node->type == AST_TYPE_REFERENCE) {
        copiedRight = node->right;
    } else {
        copiedRight = copyASTRecursive(node->right, ctx);
    }

    if (node->left && !copiedLeft) {
        popCopiedNode(ctx);
        freeAST(newNode);
        return NULL;
    }
    if (node->extra && !copiedExtra) {
        popCopiedNode(ctx);
        freeAST(newNode);
        return NULL;
    }
    if (node->right && node->type != AST_TYPE_REFERENCE && !copiedRight) {
        popCopiedNode(ctx);
        freeAST(newNode);
        return NULL;
    }

    newNode->left = copiedLeft;
    if (newNode->left) newNode->left->parent = newNode;

    newNode->right = copiedRight;
    if (newNode->right && node->type != AST_TYPE_REFERENCE) {
        newNode->right->parent = newNode;
    }

    newNode->extra = copiedExtra;
    if (newNode->extra) newNode->extra->parent = newNode;

    if (node->type_def) {
        if (node->type_def == node->right) {
            newNode->type_def = newNode->right;
        } else {
            AST *resolved = lookupCopiedNode(ctx, node->type_def);
            newNode->type_def = resolved ? resolved : node->type_def;
        }
    }

    if (node->child_count > 0 && node->children) {
        newNode->child_capacity = node->child_count;
        newNode->child_count = node->child_count;
        newNode->children = malloc(sizeof(AST*) * newNode->child_capacity);
        if (!newNode->children) {
            popCopiedNode(ctx);
            freeAST(newNode);
            EXIT_FAILURE_HANDLER();
            return NULL;
        }
        for (int i = 0; i < newNode->child_count; i++) {
            newNode->children[i] = NULL;
        }
        for (int i = 0; i < node->child_count; i++) {
            AST *childCopy = copyASTRecursive(node->children[i], ctx);
            if (node->children[i] && !childCopy) {
                for (int j = 0; j <= i; j++) {
                    if (newNode->children[j]) freeAST(newNode->children[j]);
                }
                free(newNode->children);
                newNode->children = NULL;
                newNode->child_count = 0;
                newNode->child_capacity = 0;
                popCopiedNode(ctx);
                freeAST(newNode);
                return NULL;
            }
            newNode->children[i] = childCopy;
            if (newNode->children[i]) newNode->children[i]->parent = newNode;
        }
    } else {
        newNode->children = NULL;
        newNode->child_count = 0;
        newNode->child_capacity = 0;
    }

    return newNode;
}

AST *copyAST(AST *node) {
    if (!node) return NULL;
    ASTCopyContext ctx = {0};
    AST *result = copyASTRecursive(node, &ctx);
    free(ctx.pairs);
    return result;
}

bool verifyASTLinks(AST *node, AST *expectedParent) {
     if (!node) return true;
     bool links_ok = true;
     #ifdef DEBUG
     fprintf(stderr, "[VERIFY_CHECK] Node %p (Type: %s, Token: '%s'), Actual Parent: %p, Expected Parent Param: %p\n",
             (void*)node, astTypeToString(node->type),
             (node->token && node->token->value) ? node->token->value : "NULL",
             (void*)node->parent, (void*)expectedParent);
     #endif
     if (node->parent != expectedParent) {
         fprintf(stderr, "AST Link Error: Node %p (Type: %s, Token: '%s') has parent %p, but expected %p\n",
                 (void*)node, astTypeToString(node->type),
                 (node->token && node->token->value) ? node->token->value : "NULL",
                 (void*)node->parent, (void*)expectedParent);
         links_ok = false;
     }
     if (!verifyASTLinks(node->left, node)) links_ok = false;
     if (!verifyASTLinks(node->right, node)) links_ok = false;
     if (!verifyASTLinks(node->extra, node)) links_ok = false;
     if (node->children) {
         for (int i = 0; i < node->child_count; i++) {
             if (node->children[i]) {
                  #ifdef DEBUG
                  fprintf(stderr, "[VERIFY_RECURSE] Calling verify for Child %d of Node %p. Child Node: %p, Passing Expected Parent: %p\n",
                          i, (void*)node, (void*)node->children[i], (void*)node);
                  #endif
                  if (!verifyASTLinks(node->children[i], node)) links_ok = false;
             }
         }
     }
     return links_ok;
}

void freeTypeTableASTNodes(void) {
     TypeEntry *entry = type_table;
     #ifdef DEBUG
     fprintf(stderr, "[DEBUG] freeTypeTableASTNodes: Starting cleanup of type definition ASTs.\n");
     #endif
     while (entry) {
         if (entry->typeAST) {
             #ifdef DEBUG
             fprintf(stderr, "[DEBUG]  - Freeing AST for type '%s' at %p\n",
                     entry->name ? entry->name : "?", (void*)entry->typeAST);
             #endif
             freeAST(entry->typeAST);
             entry->typeAST = NULL;
         }
         entry = entry->next;
     }
     #ifdef DEBUG
     fprintf(stderr, "[DEBUG] freeTypeTableASTNodes: Finished cleanup.\n");
     #endif
}

// --- JSON DUMPING FUNCTIONS ---

// Helper to escape strings for JSON
static void escapeJSONString(FILE *out, const char *str) {
    if (!str) {
        fprintf(out, "null");
        return;
    }
    fputc('"', out);
    while (*str) {
        switch (*str) {
            case '"':  fprintf(out, "\\\""); break;
            case '\\': fprintf(out, "\\\\"); break;
            case '\b': fprintf(out, "\\b");  break;
            case '\f': fprintf(out, "\\f");  break;
            case '\n': fprintf(out, "\\n");  break;
            case '\r': fprintf(out, "\\r");  break;
            case '\t': fprintf(out, "\\t");  break;
            default:
                if ((unsigned char)*str < 32 || *str == 127) { // Control characters or DEL
                    fprintf(out, "\\u%04x", (unsigned char)*str);
                } else {
                    fputc(*str, out);
                }
                break;
        }
        str++;
    }
    fputc('"', out);
}

// Forward declaration for the recursive helper
static void dumpASTJSONRecursive(AST *node, FILE *outFile, int indentLevel, bool isLastChildInList);

static void printJSONIndent(FILE *outFile, int indentLevel) {
    for (int i = 0; i < indentLevel; ++i) {
        fprintf(outFile, "  ");
    }
}

// Public function to initiate JSON dump
void dumpASTJSON(AST *node, FILE *outFile) {
    if (!node || !outFile) {
        if (outFile) fprintf(outFile, "null");
        return;
    }
    dumpASTJSONRecursive(node, outFile, 0, true);
    fprintf(outFile, "\n"); // Ensure a final newline
}

static void dumpASTJSONRecursive(AST *node, FILE *outFile, int indentLevel, bool isLastChildInList) {
    if (!node) {
        printJSONIndent(outFile, indentLevel);
        fprintf(outFile, "null");
        if (!isLastChildInList) fprintf(outFile, ",");
        fprintf(outFile, "\n");
        return;
    }

    printJSONIndent(outFile, indentLevel);
    fprintf(outFile, "{\n");

    int nextIndent = indentLevel + 1;
    bool first_field_has_been_printed = false;

    #define PRINT_JSON_FIELD_SEPARATOR() \
        if (first_field_has_been_printed) { fprintf(outFile, ",\n"); } \
        else { fprintf(outFile, "\n"); first_field_has_been_printed = true; }

    // --- 1. Common Node Attributes ---
    PRINT_JSON_FIELD_SEPARATOR();
    printJSONIndent(outFile, nextIndent);
    fprintf(outFile, "\"node_type\": \"%s\"", astTypeToString(node->type));

    if (node->token) {
        PRINT_JSON_FIELD_SEPARATOR();
        printJSONIndent(outFile, nextIndent);
        fprintf(outFile, "\"token\": {\n");
        printJSONIndent(outFile, nextIndent + 1);
        fprintf(outFile, "  \"type\": \"%s\",\n", tokenTypeToString(node->token->type));
        printJSONIndent(outFile, nextIndent + 1);
        fprintf(outFile, "  \"value\": ");
        escapeJSONString(outFile, node->token->value);
        fprintf(outFile, "\n");
        printJSONIndent(outFile, nextIndent);
        fprintf(outFile, "}");
    }

    PRINT_JSON_FIELD_SEPARATOR();
    printJSONIndent(outFile, nextIndent);
    fprintf(outFile, "\"var_type_annotated\": \"%s\"", varTypeToString(node->var_type));

    if (node->type == AST_VAR_DECL && node->parent &&
        (node->parent->type == AST_PROCEDURE_DECL || node->parent->type == AST_FUNCTION_DECL)) {
        PRINT_JSON_FIELD_SEPARATOR();
        printJSONIndent(outFile, nextIndent);
        fprintf(outFile, "\"by_ref\": %s", node->by_ref ? "true" : "false");
    }

    if (node->type == AST_ENUM_VALUE || node->type == AST_NUMBER) {
        PRINT_JSON_FIELD_SEPARATOR();
        printJSONIndent(outFile, nextIndent);
        fprintf(outFile, "\"i_val\": %d", node->i_val);
    }
    if (node->type == AST_PROCEDURE_DECL || node->type == AST_FUNCTION_DECL) {
        PRINT_JSON_FIELD_SEPARATOR();
        printJSONIndent(outFile, nextIndent);
        fprintf(outFile, "\"is_inline\": %s", node->is_inline ? "true" : "false");
    }
    // --- End Common Node Attributes ---


    // --- 2. Child Nodes & Specific Structures ---
    if (node->type == AST_PROGRAM) {
        if (node->left) {
            PRINT_JSON_FIELD_SEPARATOR();
            printJSONIndent(outFile, nextIndent);
            fprintf(outFile, "\"program_name_node\": \n");
            dumpASTJSONRecursive(node->left, outFile, nextIndent, true);
        }
        if (node->right) {
            PRINT_JSON_FIELD_SEPARATOR();
            printJSONIndent(outFile, nextIndent);
            fprintf(outFile, "\"main_block\": \n");
            dumpASTJSONRecursive(node->right, outFile, nextIndent, true);
        }
        if (node->child_count > 0 && node->children) {
            PRINT_JSON_FIELD_SEPARATOR();
            printJSONIndent(outFile, nextIndent);
            fprintf(outFile, "\"uses_clauses\": [\n");
            for (int i = 0; i < node->child_count; i++) {
                dumpASTJSONRecursive(node->children[i], outFile, nextIndent + 1, (i == node->child_count - 1));
            }
            printJSONIndent(outFile, nextIndent);
            fprintf(outFile, "]");
        }
    } else if (node->type == AST_BLOCK) {
        PRINT_JSON_FIELD_SEPARATOR();
        printJSONIndent(outFile, nextIndent);
        fprintf(outFile, "\"is_global_scope\": %s", node->is_global_scope ? "true" : "false");

        PRINT_JSON_FIELD_SEPARATOR();
        printJSONIndent(outFile, nextIndent);
        fprintf(outFile, "\"declarations\": ");
        if (node->child_count > 0 && node->children[0]) {
            fprintf(outFile, "\n");
            dumpASTJSONRecursive(node->children[0], outFile, nextIndent, true);
        } else {
            fprintf(outFile, "null");
        }

        PRINT_JSON_FIELD_SEPARATOR();
        printJSONIndent(outFile, nextIndent);
        fprintf(outFile, "\"body\": ");
        if (node->child_count > 1 && node->children[1]) {
            fprintf(outFile, "\n");
            dumpASTJSONRecursive(node->children[1], outFile, nextIndent, true);
        } else {
            fprintf(outFile, "null");
        }
    } else if (node->type == AST_USES_CLAUSE) {
        if (node->unit_list && node->unit_list->size > 0) {
            PRINT_JSON_FIELD_SEPARATOR();
            printJSONIndent(outFile, nextIndent);
            fprintf(outFile, "\"unit_list\": [\n");
            ListNode *current_unit = node->unit_list->head;
            bool first_in_array = true;
            while(current_unit) {
                if (!first_in_array) { fprintf(outFile, ",\n"); } else { fprintf(outFile, "\n"); }
                printJSONIndent(outFile, nextIndent + 1);
                escapeJSONString(outFile, current_unit->value);
                first_in_array = false;
                current_unit = current_unit->next;
            }
            fprintf(outFile, "\n");
            printJSONIndent(outFile, nextIndent);
            fprintf(outFile, "]");
        }
    } else {
        if (node->left) {
            PRINT_JSON_FIELD_SEPARATOR();
            printJSONIndent(outFile, nextIndent);
            fprintf(outFile, "\"left\": \n");
            dumpASTJSONRecursive(node->left, outFile, nextIndent, true);
        }
        if (node->right) {
            PRINT_JSON_FIELD_SEPARATOR();
            printJSONIndent(outFile, nextIndent);
            fprintf(outFile, "\"right\": \n");
            dumpASTJSONRecursive(node->right, outFile, nextIndent, true);
        }
        if (node->extra) {
            PRINT_JSON_FIELD_SEPARATOR();
            printJSONIndent(outFile, nextIndent);
            fprintf(outFile, "\"extra\": \n");
            dumpASTJSONRecursive(node->extra, outFile, nextIndent, true);
        }
        
        if (node->child_count > 0 && node->children) {
            PRINT_JSON_FIELD_SEPARATOR();
            printJSONIndent(outFile, nextIndent);
            fprintf(outFile, "\"children\": [\n");
            for (int i = 0; i < node->child_count; i++) {
                dumpASTJSONRecursive(node->children[i], outFile, nextIndent + 1, (i == node->child_count - 1));
            }
            printJSONIndent(outFile, nextIndent);
            fprintf(outFile, "]");
        }
    }
    // --- End Child Nodes & Specific Structures ---

    fprintf(outFile, "\n");
    printJSONIndent(outFile, indentLevel);
    fprintf(outFile, "}");

    if (!isLastChildInList) {
        fprintf(outFile, ",");
    }
    fprintf(outFile, "\n");
}
