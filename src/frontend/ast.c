#include "types.h"
#include "utils.h"
#include "list.h"
#include "ast.h"
#include "globals.h"
#include "symbol.h"
#include "parser.h"
#include "builtin.h"

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
    node->i_val = 0; // Initialize i_val
    node->symbol_table = NULL; // Initialize symbol_table
    node->unit_list = NULL; // Initialize unit_list

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
    if (!parent || !child) {
        #ifdef DEBUG
        fprintf(stderr, "[addChild Warning] Attempted to add %s to %s parent.\n",
                child ? "child" : "NULL child", parent ? "valid" : "NULL");
        #endif
        return;
    }
    if (parent->child_capacity == 0) {
        parent->child_capacity = 4;
        parent->children = malloc(sizeof(AST*) * parent->child_capacity);
        if (!parent->children) { fprintf(stderr, "Memory allocation error in addChild\n"); EXIT_FAILURE_HANDLER(); }
        for(int i=0; i < parent->child_capacity; ++i) parent->children[i] = NULL;
    } else if (parent->child_count >= parent->child_capacity) {
        int old_capacity = parent->child_capacity;
        parent->child_capacity *= 2;
        parent->children = realloc(parent->children, sizeof(AST*) * parent->child_capacity);
        if (!parent->children) { fprintf(stderr, "Memory allocation error in addChild (realloc)\n"); EXIT_FAILURE_HANDLER(); }
        for(int i=old_capacity; i < parent->child_capacity; ++i) parent->children[i] = NULL;
    }
    parent->children[parent->child_count++] = child;
    child->parent = parent;
}

void setLeft(AST *parent, AST *child) {
    if (!parent) return;
    parent->left = child;
    if (child) child->parent = parent;
}

void setRight(AST *parent, AST *child) {
    if (!parent) return;
    parent->right = child;
    if (child) child->parent = parent;
}

void setExtra(AST *parent, AST *child) {
    if (!parent) return;
    parent->extra = child;
    if (child) child->parent = parent;
}

void freeAST(AST *node) {
    if (!node) return;

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
                          if (varNameNode && varNameNode->type == AST_VARIABLE && varNameNode->token &&
                              strcasecmp(varNameNode->token->value, varName) == 0) {
                             return varDeclGroup;
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

     if (currentScopeNode && currentScopeNode != globalProgramNode) {
         foundDecl = findDeclarationInScope(varName, currentScopeNode);
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
                               if (varNameNode && varNameNode->token && strcasecmp(varNameNode->token->value, varName) == 0) {
                                   foundDecl = declGroup;
                                   goto found_static_decl;
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
        node->is_global_scope = (node->parent && node->parent->type == AST_PROGRAM);
    }

    if (node->left) annotateTypes(node->left, childScopeNode, globalProgramNode);
    if (node->right) annotateTypes(node->right, childScopeNode, globalProgramNode);
    if (node->extra) annotateTypes(node->extra, childScopeNode, globalProgramNode);
    for (int i = 0; i < node->child_count; ++i) {
         if(node->children && node->children[i]) {
              annotateTypes(node->children[i], childScopeNode, globalProgramNode);
         }
    }

    if (node->var_type == TYPE_VOID) {
        switch(node->type) {
            case AST_VARIABLE: {
                const char* varName = node->token ? node->token->value : NULL;
                if (!varName) { node->var_type = TYPE_VOID; break; }
                AST* declNode = findStaticDeclarationInAST(varName, childScopeNode, globalProgramNode);
                if (declNode) {
                    if (declNode->type == AST_VAR_DECL) {
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
                     } else { node->var_type = TYPE_VOID; }
                 } else {
                      AST* typeDef = lookupType(varName);
                      if (typeDef) {
                           node->var_type = TYPE_VOID;
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
                 if (strcasecmp(varName, "result") == 0 && childScopeNode && childScopeNode->type == AST_FUNCTION_DECL) {
                      if(childScopeNode->right) node->var_type = childScopeNode->right->var_type;
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
                      node->var_type = TYPE_BOOLEAN;
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
                 break;
             }
            case AST_FIELD_ACCESS: {
                node->var_type = TYPE_VOID;
                if (node->left && node->left->var_type == TYPE_RECORD && node->left->type_def) {
                    AST* record_definition_node = node->left->type_def;
                    if (record_definition_node->type == AST_TYPE_REFERENCE && record_definition_node->right) {
                        record_definition_node = record_definition_node->right;
                    }
                    if (record_definition_node && record_definition_node->type == AST_RECORD_TYPE) {
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
                            #ifdef DEBUG
                            fprintf(stderr, "[Annotate Warning] Field '%s' not found in record type '%s'.\n",
                                    field_to_find,
                                    node->left->token ? node->left->token->value : "UNKNOWN_RECORD");
                            #endif
                        }
                    } else { /* ... debug warnings ... */ }
                } else if (node->left) { /* ... debug warnings ... */ }
                field_found_annotate:;
                break;
            }
             case AST_ARRAY_ACCESS: {
                  node->var_type = TYPE_VOID;
                  if(node->left) {
                      if (node->left->var_type == TYPE_ARRAY && node->left->type_def) {
                           AST* arrayDefNode = node->left->type_def;
                           if (arrayDefNode && arrayDefNode->type == AST_TYPE_REFERENCE) arrayDefNode = arrayDefNode->right;
                           if (arrayDefNode && arrayDefNode->type == AST_ARRAY_TYPE && arrayDefNode->right) {
                               node->var_type = arrayDefNode->right->var_type;
                               node->type_def = arrayDefNode->right;
                           }
                      } else if (node->left->var_type == TYPE_STRING) {
                           node->var_type = TYPE_CHAR;
                      }
                  }
                 break;
             }
            // Adding types for literals if not set by parser (though parser usually does)
            case AST_NUMBER:
                node->var_type = (node->token && node->token->type == TOKEN_REAL_CONST) ? TYPE_REAL : TYPE_INTEGER;
                break;
            case AST_STRING:
                node->var_type = TYPE_STRING;
                break;
            case AST_BOOLEAN:
                node->var_type = TYPE_BOOLEAN;
                break;
            case AST_NIL:
                node->var_type = TYPE_NIL; // Or TYPE_POINTER if nil is a generic pointer type
                break;
            default:
                 break;
        }
    }
}

VarType getBuiltinReturnType(const char* name) {
     if (strcasecmp(name, "chr")==0) return TYPE_CHAR;
     if (strcasecmp(name, "ord")==0) return TYPE_INTEGER;
     // ... (rest of your getBuiltinReturnType implementation) ...
     if (strcasecmp(name,"wherey")==0) return TYPE_INTEGER;
     return TYPE_VOID;
}

AST *copyAST(AST *node) {
    if (!node) return NULL;
    AST *newNode = newASTNode(node->type, node->token);
    if (!newNode) return NULL;
    newNode->var_type = node->var_type;
    newNode->by_ref = node->by_ref;
    newNode->is_global_scope = node->is_global_scope;
    newNode->i_val = node->i_val;
    newNode->unit_list = node->unit_list;
    newNode->symbol_table = node->symbol_table;

    AST *copiedLeft = copyAST(node->left);
    AST *copiedRight = copyAST(node->right);
    AST *copiedExtra = copyAST(node->extra);
    
#ifdef DEBUG
if (node->type == AST_POINTER_TYPE || node->type == AST_TYPE_REFERENCE) {
    fprintf(stderr, "[DEBUG copyAST] Copied Node %p (Type %s). Original->right=%p. Copied Node %p->right set to %p (Type %s).\n",
            (void*)node, astTypeToString(node->type), (void*)node->right,
            (void*)newNode, (void*)copiedRight, copiedRight ? astTypeToString(copiedRight->type) : "NULL");
    if (node->type == AST_POINTER_TYPE && copiedRight) {
         fprintf(stderr, "[DEBUG copyAST]   Base type node copied for POINTER_TYPE is at %p (Type %s)\n", (void*)copiedRight, astTypeToString(copiedRight->type));
    }
    fflush(stderr);
}
#endif

    newNode->left = copiedLeft; if (newNode->left) newNode->left->parent = newNode;
    newNode->right = copiedRight; if (newNode->right) newNode->right->parent = newNode;
    newNode->extra = copiedExtra; if (newNode->extra) newNode->extra->parent = newNode;

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

    if (node->type_def) {
        PRINT_JSON_FIELD_SEPARATOR();
        printJSONIndent(outFile, nextIndent);
        fprintf(outFile, "\"type_definition_link\": \"%s (details not expanded)\"", astTypeToString(node->type_def->type));
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
