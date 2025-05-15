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

    // Removed debug dump from here, let caller handle if needed
    return node;
}

#ifdef DEBUG
// Define the helper function *only* when DEBUG is enabled
// (Your existing debugAST function code here)
#define MAX_DEBUG_DEPTH 50 // Adjust limit as needed
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
    // Use node->var_type which should be set by parser or annotateTypes
    printf(", var_type=%s", varTypeToString(node->var_type));
    printf(")\n");

    if (node->left) {
        for (int i = 0; i < indent+1; i++) { printf("  "); } // Braces added
        printf("Left:\n");
        debugAST(node->left, indent + 2);
    }
    if (node->right) {
        for (int i = 0; i < indent+1; i++) { printf("  "); } // Braces added
        printf("Right:\n");
        debugAST(node->right, indent + 2);
    }
    if (node->extra) {
        for (int i = 0; i < indent+1; i++) { printf("  "); } // Braces added
        printf("Extra:\n");
        debugAST(node->extra, indent + 2);
    }
    if (node->children && node->child_count > 0) {
         for (int i = 0; i < indent+1; i++) { printf("  "); } // Braces added
         printf("Children (%d):\n", node->child_count);
         for (int i = 0; i < node->child_count; i++) {
             debugAST(node->children[i], indent + 2);
         }
    }
}
#endif // DEBUG

void addChild(AST *parent, AST *child) {
    if (!parent || !child) { // Added NULL check for child
         #ifdef DEBUG
         fprintf(stderr, "[addChild Warning] Attempted to add %s to %s parent.\n",
                 child ? "child" : "NULL child", parent ? "valid" : "NULL");
         #endif
         return;
    }
    if (parent->child_capacity == 0) {
        parent->child_capacity = 4; // Initial capacity
        parent->children = malloc(sizeof(AST*) * parent->child_capacity);
        if (!parent->children) { fprintf(stderr, "Memory allocation error in addChild\n"); EXIT_FAILURE_HANDLER(); }
         // Initialize new pointers to NULL
         for(int i=0; i < parent->child_capacity; ++i) parent->children[i] = NULL;
    } else if (parent->child_count >= parent->child_capacity) {
        int old_capacity = parent->child_capacity;
        parent->child_capacity *= 2;
        parent->children = realloc(parent->children, sizeof(AST*) * parent->child_capacity);
        if (!parent->children) { fprintf(stderr, "Memory allocation error in addChild (realloc)\n"); EXIT_FAILURE_HANDLER(); }
         // Initialize new pointers to NULL
         for(int i=old_capacity; i < parent->child_capacity; ++i) parent->children[i] = NULL;
    }
    parent->children[parent->child_count++] = child;
    child->parent = parent; // Set parent pointer
}

void setLeft(AST *parent, AST *child) {
    if (!parent) return; // Safety check
    parent->left = child;
    if (child) child->parent = parent;
}

void setRight(AST *parent, AST *child) {
    if (!parent) return; // Safety check
    parent->right = child;
    if (child) child->parent = parent;
}

void setExtra(AST *parent, AST *child) {
    if (!parent) return; // Safety check
    parent->extra = child;
    if (child) child->parent = parent;
}

void freeAST(AST *node) {
    if (!node) return;

    // Check if this node is owned by the global type table. If so, its freeing is managed elsewhere.
    // This check is needed because type definition AST nodes are linked into the type_table
    // but can also be children within a larger AST structure (like a UNIT AST).
    if (isNodeInTypeTable(node)) { // Keep this check
        // #ifdef DEBUG // Keep debug prints conditional
        // fprintf(stderr, "[DEBUG_FREE] Postponing free for Node %p (Type: %s) as it's in type_table.\n", (void*)node, astTypeToString(node->type)); #endif
        return; // Do NOT free this node or its contents recursively from here
    }

    // #ifdef DEBUG // Keep debug prints conditional
    // fprintf(stderr, "[DEBUG_FREE] Enter freeAST for Node %p (Type: %s, Token: '%s')\n", (void*)node, astTypeToString(node->type), (node->token && node->token->value) ? node->token->value : "NULL"); #endif


    // Recursively free children and branches.
    // This will now correctly recurse into routine declarations' contents
    // if they are part of the AST being freed.
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
        // #ifdef DEBUG fprintf(stderr, "[DEBUG_FREE]  - Freeing children array for Node %p\n", (void*)node); #endif
        free(node->children);
        node->children = NULL;
        node->child_count = 0;
        node->child_capacity = 0;
    }

    // Free any dynamically allocated list attached to this specific node (e.g., unit_list for USES_CLAUSE)
    if (node->type == AST_USES_CLAUSE && node->unit_list) {
        freeList(node->unit_list); // Assuming freeList is correctly implemented
        node->unit_list = NULL;
    }
    // The symbol_table attached to a UNIT node should have been freed by freeUnitSymbolTable in linkUnit.
    // We just nullify the pointer here defensively.
    if (node->type == AST_UNIT && node->symbol_table) {
         node->symbol_table = NULL;
    }


    // Free token (copy created by newASTNode)
    if (node->token) {
        // #ifdef DEBUG fprintf(stderr, "[DEBUG_FREE]  - Freeing Token for Node %p\n", (void*)node); #endif
        freeToken(node->token);
        node->token = NULL;
    }

    // Free the node struct itself
    // #ifdef DEBUG fprintf(stderr, "[DEBUG_FREE] Freeing Node struct %p itself (Type: %s)\n", (void*)node, astTypeToString(node->type)); #endif
    free(node);
}

// --- dumpASTFromRoot Function ---
void dumpASTFromRoot(AST *node) {
    printf("===== Dumping AST From Root START =====\n");
    if (!node) return;
    while (node->parent != NULL) {
        node = node->parent;
    }
    dumpAST(node, 0);
    printf("===== Dumping AST From Root END =====\n");
}

// --- printIndent Helper ---
static void printIndent(int indent) {
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
}

// --- dumpAST Function ---
void dumpAST(AST *node, int indent) {
    if (node == NULL) return;
    printIndent(indent);
    printf("Node(type=%s", astTypeToString(node->type));
    if (node->token && node->token->value)
        printf(", token=\"%s\"", node->token->value);
    // Use node->var_type which should be set by parser or annotateTypes
    printf(", var_type=%s", varTypeToString(node->var_type));
    // Optionally print parent address for debugging links
    // printf(", parent=%p", (void*)node->parent);
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
             // Add index print for easier debugging
             printIndent(indent + 2);
             printf("Child[%d]:\n", i);
            dumpAST(node->children[i], indent + 3); // Increase indent for child content
        }
    }
}

// --- setTypeAST Function ---
void setTypeAST(AST *node, VarType type) {
    if (!node) {
        fprintf(stderr, "Internal error: setTypeAST called with NULL node.\n");
        // Consider if EXIT_FAILURE_HANDLER is too harsh here, maybe return?
        return; // Or EXIT_FAILURE_HANDLER();
    }
    node->var_type = type;
}

// --- findDeclarationInScope Function (Helper for annotateTypes) ---
// (Keep your existing implementation of findDeclarationInScope)
AST* findDeclarationInScope(const char* varName, AST* currentScopeNode) {
     if (!currentScopeNode || !varName) return NULL;
     if (currentScopeNode->type != AST_PROCEDURE_DECL && currentScopeNode->type != AST_FUNCTION_DECL) return NULL;

     // 1. Check Parameters
     for (int i = 0; i < currentScopeNode->child_count; i++) {
         AST* paramDeclGroup = currentScopeNode->children[i];
         if (paramDeclGroup && paramDeclGroup->type == AST_VAR_DECL) {
              for (int j = 0; j < paramDeclGroup->child_count; j++) {
                  AST* paramNameNode = paramDeclGroup->children[j];
                  if (paramNameNode && paramNameNode->type == AST_VARIABLE && paramNameNode->token &&
                      strcasecmp(paramNameNode->token->value, varName) == 0) {
                      return paramDeclGroup; // Return the VAR_DECL group node
                  }
              }
         }
     }
      // 1b. Check implicit function result variable
      if (currentScopeNode->type == AST_FUNCTION_DECL) {
           if (strcasecmp(currentScopeNode->token->value, varName) == 0 || strcasecmp("result", varName) == 0) {
                return currentScopeNode; // Return FUNC_DECL node itself
           }
      }

     // 2. Check Local Variables
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
                             return varDeclGroup; // Return the VAR_DECL group node
                         }
                     }
                 }
             }
         }
     }
     return NULL; // Not found in local scope
}

// --- findStaticDeclarationInAST Function (Helper for annotateTypes) ---
// (Keep your existing implementation of findStaticDeclarationInAST)
AST* findStaticDeclarationInAST(const char* varName, AST* currentScopeNode, AST* globalProgramNode) {
     if (!varName) return NULL;
     AST* foundDecl = NULL;

     // 1. Search Local Scope (if inside one)
     if (currentScopeNode && currentScopeNode != globalProgramNode) {
         foundDecl = findDeclarationInScope(varName, currentScopeNode);
     }

     // 2. Search Global Scope if not found locally (and we have a global node)
     if (!foundDecl && globalProgramNode && globalProgramNode->type == AST_PROGRAM) {
          // Global scope is the PROGRAM's main BLOCK's declarations
          if (globalProgramNode->right && globalProgramNode->right->type == AST_BLOCK && globalProgramNode->right->child_count > 0) {
              AST* globalDeclarationsNode = globalProgramNode->right->children[0];
              if (globalDeclarationsNode && globalDeclarationsNode->type == AST_COMPOUND) {
                   for (int i = 0; i < globalDeclarationsNode->child_count; i++) {
                       AST* declGroup = globalDeclarationsNode->children[i];
                       // Check only VAR declarations in global scope
                       if (declGroup && declGroup->type == AST_VAR_DECL) {
                           for (int j = 0; j < declGroup->child_count; j++) {
                               AST* varNameNode = declGroup->children[j];
                               if (varNameNode && varNameNode->token && strcasecmp(varNameNode->token->value, varName) == 0) {
                                   foundDecl = declGroup; // Found global var
                                   goto found_static_decl; // Exit loops
                               }
                           }
                       }
                        // Could also check CONST declarations if needed
                        else if (declGroup && declGroup->type == AST_CONST_DECL) {
                             if (declGroup->token && strcasecmp(declGroup->token->value, varName) == 0) {
                                  foundDecl = declGroup; // Found global const
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

    if (node->var_type == TYPE_VOID) { // Only set if not already determined by parser
        switch(node->type) {
            // ... (cases for literals, variable, binary_op, unary_op as before) ...
            case AST_VARIABLE: {
                // ... (existing logic for AST_VARIABLE, which uses findStaticDeclarationInAST) ...
                // This part seems okay as it uses findStaticDeclarationInAST which returns AST*
                // and then inspects declNode->var_type or declNode->right->var_type.
                // It does not directly call lookupProcedure.
                const char* varName = node->token ? node->token->value : NULL;
                if (!varName) { node->var_type = TYPE_VOID; break; }

                AST* declNode = findStaticDeclarationInAST(varName, childScopeNode, globalProgramNode);
                if (declNode) {
                    if (declNode->type == AST_VAR_DECL) {
                        node->var_type = declNode->var_type;
                        // Crucially, link to the type definition AST node stored by varDeclaration
                        node->type_def = declNode->right; // Assuming varDeclaration stores type AST in 'right'
                    } else if (declNode->type == AST_CONST_DECL) {
                        // ... (handle const type and type_def if it's a typed constant) ...
                        node->var_type = declNode->var_type;
                        if (node->var_type == TYPE_VOID && declNode->left) { // Simple const
                             node->var_type = declNode->left->var_type;
                        }
                        node->type_def = declNode->right; // For typed constants (like array constants)
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
                           // This warning is normal for the program name 'foo' in the test case
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
                 // ... (rest of binary op type inference from before, no changes needed here from this refactor) ...
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
                 Symbol *procSymbol = node->token ? lookupProcedure(node->token->value) : NULL; // << MODIFIED call, returns Symbol*
                 if (procSymbol) { // Found in user-defined or built-in (if built-ins populated procedure_table as Symbols)
                     // The symbol's type IS the return type of the function, or VOID for procedure
                     node->var_type = procSymbol->type;

                     // If it's a function, its AST declaration is in type_def
                     // AST* funcDeclAST = procSymbol->type_def;
                     // if (funcDeclAST && funcDeclAST->type == AST_FUNCTION_DECL) {
                     //     // This is redundant if procSymbol->type is already correctly set
                     // }
                 } else {
                      // This 'else' might be less likely if all builtins are in procedure_table.
                      // However, getBuiltinReturnType can be a fallback if lookupProcedure fails for a known C-level builtin.
                      if (node->token) {
                           node->var_type = getBuiltinReturnType(node->token->value);
                           if (node->var_type == TYPE_VOID && isBuiltin(node->token->value)) {
                               // It's a known built-in procedure, VOID is correct.
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
            // ... (other cases like AST_FIELD_ACCESS, AST_ARRAY_ACCESS might need review if they use lookupProcedure) ...
            // For now, assuming they primarily use type information propagated from their left child.
            case AST_FIELD_ACCESS: {
                node->var_type = TYPE_VOID; // Default
                // Ensure node->left (the record variable) has been annotated first
                // Its node->left->var_type should be TYPE_RECORD
                // Its node->left->type_def should point to the AST_RECORD_TYPE

                if (node->left && node->left->var_type == TYPE_RECORD && node->left->type_def) {
                    AST* record_definition_node = node->left->type_def;
                    // If record_definition_node is an AST_TYPE_REFERENCE, resolve it
                    if (record_definition_node->type == AST_TYPE_REFERENCE && record_definition_node->right) {
                        record_definition_node = record_definition_node->right;
                    }

                    if (record_definition_node && record_definition_node->type == AST_RECORD_TYPE) {
                        // node->token of AST_FIELD_ACCESS holds the field name
                        const char* field_to_find = node->token ? node->token->value : NULL;
                        if (field_to_find) {
                            for (int i = 0; i < record_definition_node->child_count; i++) {
                                AST* field_decl_group = record_definition_node->children[i]; // This is an AST_VAR_DECL
                                if (field_decl_group && field_decl_group->type == AST_VAR_DECL) {
                                    for (int j = 0; j < field_decl_group->child_count; j++) {
                                        AST* field_name_node = field_decl_group->children[j]; // This is an AST_VARIABLE
                                        if (field_name_node && field_name_node->token &&
                                            strcasecmp(field_name_node->token->value, field_to_find) == 0) {
                                            node->var_type = field_decl_group->var_type; // The type of the var group
                                            node->type_def = field_decl_group->right;    // The type AST of the var group
                                            goto field_found_annotate; // Using goto as in your original
                                        }
                                    }
                                }
                            }
                             // If loop finishes, field was not found
                            #ifdef DEBUG
                            fprintf(stderr, "[Annotate Warning] Field '%s' not found in record type '%s'.\n",
                                    field_to_find,
                                    node->left->token ? node->left->token->value : "UNKNOWN_RECORD");
                            #endif
                        }
                    } else {
                         #ifdef DEBUG
                         fprintf(stderr, "[Annotate Warning] Left operand of field access '%s' is not a properly defined record or its type_def is not AST_RECORD_TYPE.\n",
                                 node->left->token ? node->left->token->value : "UNKNOWN_LHS");
                         if (record_definition_node) {
                             fprintf(stderr, "[Annotate Debug] record_definition_node type is %s\n", astTypeToString(record_definition_node->type));
                         }
                         #endif
                    }
                } else if (node->left) {
                    #ifdef DEBUG
                    fprintf(stderr, "[Annotate Warning] Left operand of field access for field '%s' is not a record (type: %s) or type_def is missing.\n",
                            node->token ? node->token->value : "UNKNOWN_FIELD",
                            varTypeToString(node->left->var_type));
                    #endif
                }
                field_found_annotate:;
                break;
            }
             case AST_ARRAY_ACCESS: {
                  node->var_type = TYPE_VOID;
                  if(node->left) { // Ensure left child (array variable) exists
                      if (node->left->var_type == TYPE_ARRAY && node->left->type_def) {
                           AST* arrayDefNode = node->left->type_def; // Get the AST_ARRAY_TYPE node
                           if (arrayDefNode && arrayDefNode->type == AST_TYPE_REFERENCE) arrayDefNode = arrayDefNode->right; // Resolve

                           if (arrayDefNode && arrayDefNode->type == AST_ARRAY_TYPE && arrayDefNode->right) {
                               node->var_type = arrayDefNode->right->var_type; // Element type
                               node->type_def = arrayDefNode->right; // Link to element type def AST
                           }
                      } else if (node->left->var_type == TYPE_STRING) {
                           node->var_type = TYPE_CHAR; // Indexing a string yields a char
                           // For type_def, can point to a generic CHAR type node if one exists, or NULL
                           // AST* charTypeNode = lookupType("char"); if (charTypeNode) node->type_def = charTypeNode;
                      }
                  }
                 break;
             }

            default:
                 break;
        }
    }
    // ... (final variable check as before, if necessary) ...
}
VarType getBuiltinReturnType(const char* name) {
     // Simplified version - add more built-in *functions* here
     if (strcasecmp(name, "chr")==0) return TYPE_CHAR;
     if (strcasecmp(name, "ord")==0) return TYPE_INTEGER;
     if (strcasecmp(name, "length")==0) return TYPE_INTEGER;
     if (strcasecmp(name, "abs")==0) return TYPE_INTEGER; // Or REAL depending on arg
     if (strcasecmp(name, "sqr")==0) return TYPE_INTEGER; // Or REAL depending on arg
     if (strcasecmp(name, "sqrt")==0) return TYPE_REAL;
     if (strcasecmp(name, "sin")==0) return TYPE_REAL;
     if (strcasecmp(name, "cos")==0) return TYPE_REAL;
     if (strcasecmp(name, "ln")==0) return TYPE_REAL;
     if (strcasecmp(name, "exp")==0) return TYPE_REAL;
     if (strcasecmp(name, "trunc")==0) return TYPE_INTEGER;
     if (strcasecmp(name, "round")==0) return TYPE_INTEGER;
     if (strcasecmp(name, "random")==0) return TYPE_REAL; // Default, can be integer
     if (strcasecmp(name, "keypressed")==0) return TYPE_BOOLEAN;
     if (strcasecmp(name, "ioresult")==0) return TYPE_INTEGER;
     if (strcasecmp(name, "eof")==0) return TYPE_BOOLEAN;
     if (strcasecmp(name, "pos")==0) return TYPE_INTEGER;
     if (strcasecmp(name, "copy")==0) return TYPE_STRING;
     if (strcasecmp(name, "inttostr")==0) return TYPE_STRING;
     if (strcasecmp(name, "paramcount")==0) return TYPE_INTEGER;
     if (strcasecmp(name, "paramstr")==0) return TYPE_STRING;
     if (strcasecmp(name, "readkey")==0) return TYPE_CHAR; // Or String[1]?
     if (strcasecmp(name,"low")==0) return TYPE_INTEGER; // Placeholder, depends on arg type
     if (strcasecmp(name,"high")==0) return TYPE_INTEGER; // Placeholder, depends on arg type
     if (strcasecmp(name,"succ")==0) return TYPE_INTEGER; // Placeholder, depends on arg type
     if (strcasecmp(name,"pred")==0) return TYPE_INTEGER; // Placeholder, depends on arg type
     if (strcasecmp(name,"upcase")==0) return TYPE_CHAR;
     if (strcasecmp(name,"screencols")==0) return TYPE_INTEGER;
     if (strcasecmp(name,"screenrows")==0) return TYPE_INTEGER;
     if (strcasecmp(name,"wherex")==0) return TYPE_INTEGER;
     if (strcasecmp(name,"wherey")==0) return TYPE_INTEGER;
     // Add other functions...
     return TYPE_VOID; // Default for procedures or unknown
}
AST *copyAST(AST *node) {
    if (!node) return NULL;
    AST *newNode = newASTNode(node->type, node->token);
    if (!newNode) return NULL;
    newNode->var_type = node->var_type;
    newNode->by_ref = node->by_ref;
    newNode->is_global_scope = node->is_global_scope;
    newNode->i_val = node->i_val;
    newNode->unit_list = node->unit_list;     // Shallow copy list pointer
    newNode->symbol_table = node->symbol_table; // Shallow copy symbol table pointer

    AST *copiedLeft = copyAST(node->left);
    AST *copiedRight = copyAST(node->right);
    AST *copiedExtra = copyAST(node->extra);
    
#ifdef DEBUG
// ADDED: Debug print after copying right child
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
        for (int i = 0; i < newNode->child_count; i++) newNode->children[i] = NULL; // Init

        for (int i = 0; i < node->child_count; i++) {
            newNode->children[i] = copyAST(node->children[i]);
            if (!newNode->children[i] && node->children[i]) { // Check copy failure
                 for(int j = 0; j < i; ++j) freeAST(newNode->children[j]);
                 free(newNode->children); newNode->children = NULL;
                 newNode->child_count = 0; newNode->child_capacity = 0;
                 freeAST(newNode); return NULL;
             }
            if (newNode->children[i]) newNode->children[i]->parent = newNode; // Set parent
        }
    } else {
        newNode->children = NULL; newNode->child_count = 0; newNode->child_capacity = 0;
    }
    return newNode;
}

// --- verifyASTLinks Function ---
// (Keep your existing implementation of verifyASTLinks)
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
         // Optionally dump more context here if needed
         // dumpAST(node, 0); // Careful with recursion depth
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

// --- freeTypeTableASTNodes Function ---
// (Keep implementation from previous step)
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
