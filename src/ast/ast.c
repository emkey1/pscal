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
    if (node->freed) return;
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

AST* findDeclarationInScope(const char* varName, AST* currentScopeNode) {
     if (!currentScopeNode || !varName) return NULL;
     if (currentScopeNode->type != AST_PROCEDURE_DECL && currentScopeNode->type != AST_FUNCTION_DECL) return NULL;

     for (int i = 0; i < currentScopeNode->child_count; i++) {
         AST* paramDeclGroup = currentScopeNode->children[i];
         if (paramDeclGroup && paramDeclGroup->type == AST_VAR_DECL) {
              for (int j = 0; j < paramDeclGroup->child_count; j++) {
                  AST* paramNameNode = paramDeclGroup->children[j];
                  if (paramNameNode && paramNameNode->type == AST_VARIABLE && paramNameNode->token &&
                      strcasecmp(paramNameNode->token->value, varName) == 0) {
                      return paramDeclGroup;
                  }
              }
         }
     }
      if (currentScopeNode->type == AST_FUNCTION_DECL) {
           if (strcasecmp(currentScopeNode->token->value, varName) == 0 || strcasecmp("result", varName) == 0) {
                return currentScopeNode;
           }
      }

     AST* blockNode = (currentScopeNode->type == AST_PROCEDURE_DECL) ? currentScopeNode->right : currentScopeNode->extra;
     if (blockNode && blockNode->type == AST_BLOCK && blockNode->child_count > 0) {
         AST* declarationsNode = blockNode->children[0];
         if (declarationsNode && declarationsNode->type == AST_COMPOUND) {
             for (int i = 0; i < declarationsNode->child_count; i++) {
                 AST* varDeclGroup = declarationsNode->children[i];
                 if (varDeclGroup && varDeclGroup->type == AST_VAR_DECL) {
                     for (int j = 0; j < varDeclGroup->child_count; j++) {
                         AST* varNameNode = varDeclGroup->children[j];
                         if (varNameNode) {
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
                     }
                 }
            }
        }
    }
     return NULL;
}

AST* findStaticDeclarationInAST(const char* varName, AST* currentScopeNode, AST* globalProgramNode) {
     if (!varName) return NULL;
     AST* foundDecl = NULL;

     // First, check for the identifier in the global symbol table. This will find enums.
     Symbol* sym = lookupGlobalSymbol(varName);
     if (sym) {
         // If a symbol is found, return its type definition AST node.
         // For enums, this correctly points back to the AST_ENUM_TYPE node.
         return sym->type_def;
     }

    if (currentScopeNode && currentScopeNode != globalProgramNode) {
        foundDecl = findDeclarationInScope(varName, currentScopeNode);
    }

    // If not found in the immediate scope, walk up parent scopes to
    // support nested routines accessing variables from enclosing
    // procedures/functions (upvalues).
    AST* parentScope = currentScopeNode ? currentScopeNode->parent : NULL;
    while (!foundDecl && parentScope) {
        if (parentScope->type == AST_PROCEDURE_DECL || parentScope->type == AST_FUNCTION_DECL) {
            foundDecl = findDeclarationInScope(varName, parentScope);
            if (foundDecl) break;
        }
        parentScope = parentScope->parent;
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
                                       foundDecl = declGroup;
                                       goto found_static_decl;
                                   } else if (varNameNode->type == AST_ASSIGN && varNameNode->left &&
                                              varNameNode->left->type == AST_VARIABLE && varNameNode->left->token &&
                                              strcasecmp(varNameNode->left->token->value, varName) == 0) {
                                       foundDecl = declGroup;
                                       goto found_static_decl;
                                   }
                               }
                           }
                       }
                        else if (declGroup && declGroup->type == AST_CONST_DECL) {
                             if (declGroup->token && strcasecmp(declGroup->token->value, varName) == 0) {
                                  foundDecl = declGroup;
                                  goto found_static_decl;
                             }
                       }
                   }
              }
          }
     }
 found_static_decl:;
     return foundDecl;
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
                // Address-of: ensure left is an identifier referencing a procedure/function
                node->var_type = TYPE_POINTER;
                if (node->left && node->left->token && node->left->token->value) {
                    const char* name = node->left->token->value;
                    Symbol* procSym = lookupProcedure(name);
                    if (!procSym) {
                        fprintf(stderr, "L%d: Compiler Error: '@' requires a procedure or function identifier (got '%s').\n",
                                node->token ? node->token->line : 0, name);
                        pascal_semantic_error_count++;
                    }
                } else {
                    fprintf(stderr, "Compiler Error: '@' missing identifier operand.\n");
                    pascal_semantic_error_count++;
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

                // First, consult the global symbol table.  This handles variables
                // and constants imported from units via linkUnit.
                Symbol* sym = lookupGlobalSymbol(varName);
                if (sym) {
                    node->var_type = sym->type;
                    node->type_def = sym->type_def;
                } else {
                    AST* declNode = findStaticDeclarationInAST(varName, childScopeNode, globalProgramNode);
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
                            const char* us = strchr(fn, '_');
                            if (us) {
                                size_t len = (size_t)(us - fn);
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
                     node->var_type = TYPE_REAL;
                 }
                 else if (leftType == TYPE_REAL || rightType == TYPE_REAL) {
                      node->var_type = TYPE_REAL;
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
            case AST_UNARY_OP:
                node->var_type = (node->token && node->token->type == TOKEN_NOT) ? TYPE_BOOLEAN : (node->left ? node->left->var_type : TYPE_VOID);
                break;
            case AST_PROCEDURE_CALL: {
                 Symbol *procSymbol = node->token ? lookupProcedure(node->token->value) : NULL;
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
                                     Symbol* as = lookupProcedure(aname);
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
                                                     fprintf(stderr, "Type error: proc pointer param %d type mismatch for '%s' (expected %s, got %s).\n", j+1, aname, varTypeToString(ft->var_type), varTypeToString(at->var_type));
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
                    if (lhsType && lhsType->type == AST_PROC_PTR_TYPE) {
                        if (rhs->type == AST_ADDR_OF && rhs->left && rhs->left->token) {
                            const char* pname = rhs->left->token->value;
                            Symbol* psym = lookupProcedure(pname);
                            if (psym && psym->type_def) {
                                // Compare signatures: param count and simple VarType equality
                                AST* decl = psym->type_def; // PROCEDURE_DECL or FUNCTION_DECL
                                int declParamCount = decl->child_count;
                                AST* paramsList = (lhsType->child_count > 0) ? lhsType->children[0] : NULL; // AST_LIST
                                int ptrParamCount = paramsList ? paramsList->child_count : 0;
                                if (declParamCount != ptrParamCount) {
                                    fprintf(stderr, "Type error: proc pointer arity mismatch for '%s' (expected %d, got %d).\n",
                                            pname, ptrParamCount, declParamCount);
                                    pascal_semantic_error_count++;
                                } else {
                                    for (int i = 0; i < declParamCount; ++i) {
                                        AST* dparam = decl->children[i];
                                        AST* tparam = paramsList ? paramsList->children[i] : NULL;
                                        if (!dparam || !tparam) continue;
                                        VarType dt = dparam->var_type;
                                        VarType tt = tparam->var_type;
                                        if (dt != tt) {
                                            fprintf(stderr, "Type error: proc pointer param %d type mismatch for '%s' (expected %s, got %s).\n",
                                                    i+1, pname, varTypeToString(tt), varTypeToString(dt));
                                            pascal_semantic_error_count++;
                                            break;
                                        }
                                    }
                                }
                                // Return type for function pointers
                                AST* lhsRet = lhsType->right; // may be NULL for procedure
                                if (lhsRet) {
                                    AST* declRet = decl->right; // may be NULL for procedure
                                    VarType lrt = lhsRet->var_type;
                                    VarType drt = declRet ? declRet->var_type : TYPE_VOID;
                                    if (lrt != drt) {
                                        fprintf(stderr, "Type error: proc pointer return type mismatch for '%s' (expected %s, got %s).\n",
                                                pname, varTypeToString(lrt), varTypeToString(drt));
                                        pascal_semantic_error_count++;
                                    }
                                }
                            } else {
                                fprintf(stderr, "Type error: '@%s' does not name a known procedure or function.\n", pname);
                                pascal_semantic_error_count++;
                            }
                        } else {
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

    /* Math routines returning REAL */
    if (strcasecmp(name, "cos")   == 0 ||
        strcasecmp(name, "sin")   == 0 ||
        strcasecmp(name, "tan")   == 0 ||
        strcasecmp(name, "sqrt")  == 0 ||
        strcasecmp(name, "ln")    == 0 ||
        strcasecmp(name, "exp")   == 0 ||
        strcasecmp(name, "real")  == 0 ||
        strcasecmp(name, "arctan") == 0 ||
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
        strcasecmp(name, "paramcount")== 0 ||
        strcasecmp(name, "length")    == 0 ||
        strcasecmp(name, "pos")       == 0 ||
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
    if (strcasecmp(name, "inttostr")  == 0 ||
        strcasecmp(name, "realtostr") == 0 ||
        strcasecmp(name, "paramstr")  == 0 ||
        strcasecmp(name, "copy")      == 0 ||
        strcasecmp(name, "mstreambuffer") == 0) {
        return TYPE_STRING;
    }

    /* Memory stream helpers */
    if (strcasecmp(name, "mstreamcreate") == 0) return TYPE_MEMORYSTREAM;

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

AST *copyAST(AST *node) {
    if (!node) return NULL;
    AST *newNode = newASTNode(node->type, node->token);
    if (!newNode) return NULL;
    // Ensure fresh node isn't marked as freed
    newNode->freed = false;

    // Copy all scalar fields
    newNode->var_type = node->var_type;
    newNode->by_ref = node->by_ref;
    newNode->is_global_scope = node->is_global_scope;
    newNode->is_inline = node->is_inline;
    newNode->is_virtual = node->is_virtual;
    newNode->i_val = node->i_val;
    // Preserve pointers for unit_list and symbol_table (shallow copy).
    // These structures are managed elsewhere and do not require deep copies
    // when duplicating the AST node.  Retaining the symbol_table pointer is
    // especially important for procedure declarations that contain nested
    // routines; the VM relies on this table at runtime to resolve calls to
    // inner procedures by their bytecode address.
    newNode->unit_list = node->unit_list;
    newNode->symbol_table = node->symbol_table;
    newNode->type_def = NULL;
    
    // Handle children first
    AST *copiedLeft = copyAST(node->left);
    AST *copiedExtra = copyAST(node->extra);
    AST *copiedRight = NULL;

    if (node->type == AST_TYPE_REFERENCE) {
        // For a type reference, do not deep copy the 'right' pointer.
        // It points to a canonical definition in the type_table.
        // Just copy the pointer value itself.
        copiedRight = node->right;
    } else {
        // For all other node types, perform a recursive deep copy.
        copiedRight = copyAST(node->right);
    }
    
    // Now set the pointers and parents
    newNode->left = copiedLeft;
    if (newNode->left) newNode->left->parent = newNode;
    
    newNode->right = copiedRight;
    // Only set the parent pointer if we made a deep copy.
    // Do NOT change the parent of a node from the type_table.
    if (newNode->right && node->type != AST_TYPE_REFERENCE) {
        newNode->right->parent = newNode;
    }

    newNode->extra = copiedExtra;
    if (newNode->extra) newNode->extra->parent = newNode;

    // Mirror the original node's type_def pointer without deep copying to
    // preserve canonical type nodes and avoid recursion.
    if (node->type_def) {
        newNode->type_def = (node->type_def == node->right)
                               ? newNode->right
                               : node->type_def;
    }
    
    // Children copy logic remains the same
    if (node->child_count > 0 && node->children) {
        newNode->child_capacity = node->child_count;
        newNode->child_count = node->child_count;
        newNode->children = malloc(sizeof(AST*) * newNode->child_capacity);
        if (!newNode->children) { freeAST(newNode); return NULL; }
        for (int i = 0; i < newNode->child_count; i++) newNode->children[i] = NULL;

        for (int i = 0; i < node->child_count; i++) {
            newNode->children[i] = copyAST(node->children[i]);
            if (!newNode->children[i] && node->children[i]) {
                 for(int j = 0; j < i; ++j) freeAST(newNode->children[j]);
                 free(newNode->children); newNode->children = NULL;
                 newNode->child_count = 0; newNode->child_capacity = 0;
                 freeAST(newNode); return NULL;
             }
            if (newNode->children[i]) newNode->children[i]->parent = newNode;
        }
    } else {
        newNode->children = NULL; newNode->child_count = 0; newNode->child_capacity = 0;
    }
    return newNode;
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
