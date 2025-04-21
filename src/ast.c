#include "types.h"
#include "utils.h"
#include "list.h"
#include "ast.h"
#include "globals.h"
#include "symbol.h"
#include "parser.h"

AST *newASTNode(ASTNodeType type, Token *token) {
    AST *node = malloc(sizeof(AST));
    if (!node) { fprintf(stderr, "Memory allocation error in new_ast_node\n"); EXIT_FAILURE_HANDLER(); }

    node->token = token ? copyToken(token) : NULL;
    setTypeAST(node, TYPE_VOID);
    node->by_ref = 0;
    node->left = node->right = node->extra = node->parent = NULL;
    node->children = NULL;
    node->child_count = 0;
    node->child_capacity = 0;
    node->type = type;
    node->is_global_scope = false;

#ifdef DEBUG
    if (dumpExec) debugAST(node, 0);
#endif
    return node;
}

#ifdef DEBUG

#define MAX_DEBUG_DEPTH 50 // Adjust limit as needed

void debugAST(AST *node, int indent) {
    if (!node) return;

    // --- ADD DEPTH CHECK ---
    if (indent > MAX_DEBUG_DEPTH) {
         for (int i = 0; i < indent; i++) printf("  "); // Maintain indentation for context
         printf("... (Max recursion depth %d reached in debugAST)\n", MAX_DEBUG_DEPTH);
         return; // Stop recursing
    }
    // --- END DEPTH CHECK ---

    // Original printing logic
    for (int i = 0; i < indent; i++) printf("  ");
    printf("Node(type=%s", astTypeToString(node->type));
    if (node->token && node->token->value)
        printf(", token=\"%s\"", node->token->value);
    printf(", var_type=%s", varTypeToString(node->var_type));
    printf(")\n");

    // Original recursive calls
    if (node->left) {
        for (int i = 0; i < indent+1; i++) printf("  "); printf("Left:\n");
        debugAST(node->left, indent + 2);
    }
    if (node->right) {
        for (int i = 0; i < indent+1; i++) printf("  "); printf("Right:\n");
        debugAST(node->right, indent + 2);
    }
    if (node->extra) {
        for (int i = 0; i < indent+1; i++) printf("  "); printf("Extra:\n");
        debugAST(node->extra, indent + 2);
    }
    if (node->children && node->child_count > 0) {
         for (int i = 0; i < indent+1; i++) printf("  "); printf("Children (%d):\n", node->child_count);
         for (int i = 0; i < node->child_count; i++) {
             debugAST(node->children[i], indent + 2);
         }
    }
}
#endif

void addChild(AST *parent, AST *child) {
    if (!parent) return;
    if (parent->child_capacity == 0) {
        parent->child_capacity = 4;
        parent->children = malloc(sizeof(AST*) * parent->child_capacity);
        if (!parent->children) { fprintf(stderr, "Memory allocation error\n"); EXIT_FAILURE_HANDLER(); }
    } else if (parent->child_count >= parent->child_capacity) {
        parent->child_capacity *= 2;
        parent->children = realloc(parent->children, sizeof(AST*) * parent->child_capacity);
        if (!parent->children) { fprintf(stderr, "Memory allocation error\n"); EXIT_FAILURE_HANDLER(); }
    }
    parent->children[parent->child_count++] = child;
    child->parent = parent; // Bidrectional is the future
}

void setLeft(AST *parent, AST *child) {
    parent->left = child;
    if (child) child->parent = parent;
}

void setRight(AST *parent, AST *child) {
    parent->right = child;
    if (child) child->parent = parent;
}

void setExtra(AST *parent, AST *child) {
    parent->extra = child;
    if (child) child->parent = parent;
}

void freeAST(AST *node) {
    if (!node) return;

    // --- Print Node address and Type BEFORE processing ---
#ifdef DEBUG
    fprintf(stderr, "[DEBUG] Enter freeAST for Node %p (Type: %s, Token: '%s')\n",
            (void*)node,
            astTypeToString(node->type),
            (node->token && node->token->value) ? node->token->value : "NULL");
#endif

    // --- CHANGE: New Freeing Order ---

    // 1. Recurse Left
    if (node->left) {
#ifdef DEBUG
        fprintf(stderr, "[DEBUG]  - Recursing Left from Node %p into Node %p (Type: %s)\n",
                (void*)node, (void*)node->left, astTypeToString(node->left->type));
#endif
        freeAST(node->left);
        node->left = NULL;
    }

    // 2. Recurse Right
    if (node->right) {
#ifdef DEBUG
        fprintf(stderr, "[DEBUG]  - Recursing Right from Node %p into Node %p (Type: %s)\n",
                (void*)node, (void*)node->right, astTypeToString(node->right->type));
#endif
        freeAST(node->right);
        node->right = NULL;
    }

    // 3. Recurse Extra
    if (node->extra) {
#ifdef DEBUG
        fprintf(stderr, "[DEBUG]  - Recursing Extra from Node %p into Node %p (Type: %s)\n",
                (void*)node, (void*)node->extra, astTypeToString(node->extra->type));
#endif
        freeAST(node->extra);
        node->extra = NULL;
    }

    // 4. Recurse Children and free array
    if (node->children) {
         for (int i = 0; i < node->child_count; i++) {
             if (node->children[i]) {
#ifdef DEBUG
                fprintf(stderr, "[DEBUG]  - Recursing Child %d from Node %p into Node %p (Type: %s)\n",
                        i, (void*)node, (void*)node->children[i], astTypeToString(node->children[i]->type));
#endif
                 freeAST(node->children[i]);
                 node->children[i] = NULL; // Not strictly necessary before freeing array, but good practice
             } else {
#ifdef DEBUG
                 fprintf(stderr, "[DEBUG]  - Child %d of Node %p is NULL, skipping free.\n", i, (void*)node);
#endif
             }
         }
#ifdef DEBUG
         fprintf(stderr, "[DEBUG]  - Freeing children array for Node %p\n", (void*)node);
#endif
         free(node->children); // Free the array itself
         node->children = NULL;
         node->child_count = 0;
         node->child_capacity = 0;
    }

    // 5. Free token (after all subtrees/children are handled)
    if (node->token) {
#ifdef DEBUG // Or DEBUG_FREE if you prefer
fprintf(stderr, "[DEBUG_FREE_TOKEN] Preparing to free token for Node %p (Type: %s, TokenValue: '%s')\n",
        (void*)node,
        astTypeToString(node->type),
        (node->token && node->token->value) ? node->token->value : "NULL");
#endif
#ifdef DEBUG
        fprintf(stderr, "[DEBUG]  - Freeing Token for Node %p\n", (void*)node);
#endif
        freeToken(node->token); // freeToken should handle freeing value and struct
        node->token = NULL;
    }

    // 6. Free the node struct itself
#ifdef DEBUG
    fprintf(stderr, "[DEBUG] Freeing Node struct %p itself (Type: %s)\n",
            (void*)node, astTypeToString(node->type));
#endif
    free(node);

    // --- END CHANGE ---
}

void dumpASTFromRoot(AST *node) {
    if (!node) return;

    // Step 1: Traverse up to the root
    while (node->parent != NULL) {
        node = node->parent;
    }

    // Step 2: Dump the AST from the root
    dumpAST(node, 0);
}

/* Helper function to print indentation */
static void printIndent(int indent) {
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
}

/*
 * dump_ast - Recursively prints the AST tree in a human-readable format.
 *
 * Parameters:
 *   node   - Pointer to the current AST node.
 *   indent - Current indentation level for pretty printing.
 *
 * The function prints the node's type (as an integer), token (if available),
 * and var_type. It then recursively dumps the left, right, extra subtrees,
 * as well as any children stored in the children array.
 */
// Entry point: climb to root, then dump from there
void dumpAST(AST *node, int indent) {
    if (node == NULL)
        return;
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
            dumpAST(node->children[i], indent + 2);
        }
    }
}

void setTypeAST(AST *node, VarType type) {
    if (!node) {
        fprintf(stderr, "Internal error: setType called with NULL node.\n");
        EXIT_FAILURE_HANDLER();
    }

#ifdef DEBUG
    if (node->var_type != type) {
        DEBUG_PRINT("Changing type of AST node from %s to %s\n",
                varTypeToString(node->var_type),
                varTypeToString(type));
    }
#endif

    // You could add logic to prevent invalid transitions here:
    // e.g., if node already has a STRING type and type == INTEGER, warn or assert.

    node->var_type = type;
}

// Finds the AST node representing the declaration of a variable within a specific scope.
// Searches parameters and local VAR declarations of the given procedure/function node.
// Returns the AST_VAR_DECL node if found, otherwise NULL.
AST* findDeclarationInScope(const char* varName, AST* currentScopeNode) {
    if (!currentScopeNode || !varName) {
        return NULL; // Not inside a procedure/function scope or no name given
    }

    // Ensure we have a procedure or function declaration node
    if (currentScopeNode->type != AST_PROCEDURE_DECL && currentScopeNode->type != AST_FUNCTION_DECL) {
        // This shouldn't happen if called correctly, but handle defensively
        return NULL;
    }

    // 1. Check Parameters (direct children of PROC/FUNC DECL node are param groups)
    for (int i = 0; i < currentScopeNode->child_count; i++) {
        AST* paramDeclGroup = currentScopeNode->children[i]; // This is an AST_VAR_DECL for the group
        if (paramDeclGroup && paramDeclGroup->type == AST_VAR_DECL) {
             for (int j = 0; j < paramDeclGroup->child_count; j++) { // Iterate names in group
                 AST* paramNameNode = paramDeclGroup->children[j];
                 if (paramNameNode && paramNameNode->type == AST_VARIABLE && paramNameNode->token &&
                     strcasecmp(paramNameNode->token->value, varName) == 0) {
                     // Found the parameter name. Return the VAR_DECL group node,
                     // as it holds the type information (var_type and right child link).
                     return paramDeclGroup;
                 }
             }
        }
    }
     // Check for implicit function result variable ('result' or function name itself)
     if (currentScopeNode->type == AST_FUNCTION_DECL) {
          if (strcasecmp(currentScopeNode->token->value, varName) == 0 || strcasecmp("result", varName) == 0) {
               // Return the function declaration node itself; its type info is in ->right
               return currentScopeNode; // Special case: type info is in the FUNC_DECL node's ->right
          }
     }


    // 2. Check Local Variables
    // Find the BLOCK node (depends on FUNC vs PROC structure)
    AST* blockNode = NULL;
    if (currentScopeNode->type == AST_PROCEDURE_DECL) blockNode = currentScopeNode->right; // Proc body is right child
    else if (currentScopeNode->type == AST_FUNCTION_DECL) blockNode = currentScopeNode->extra; // Func body is extra child

    // Check if block exists and has declarations part
    if (blockNode && blockNode->type == AST_BLOCK && blockNode->child_count > 0) {
        AST* declarationsNode = blockNode->children[0]; // First child is declarations compound
        if (declarationsNode && declarationsNode->type == AST_COMPOUND) {
            // Iterate through declaration sections (CONST, TYPE, VAR, etc.)
            for (int i = 0; i < declarationsNode->child_count; i++) {
                AST* varDeclGroup = declarationsNode->children[i]; // Could be VAR_DECL, TYPE_DECL etc.
                if (varDeclGroup && varDeclGroup->type == AST_VAR_DECL) {
                    // Iterate through variable names in this VAR declaration group
                    for (int j = 0; j < varDeclGroup->child_count; j++) {
                        AST* varNameNode = varDeclGroup->children[j];
                         if (varNameNode && varNameNode->type == AST_VARIABLE && varNameNode->token &&
                             strcasecmp(varNameNode->token->value, varName) == 0) {
                            // Found the local variable name. Return the VAR_DECL group node.
                            return varDeclGroup;
                        }
                    }
                }
            }
        }
    }

    // Not found in local scope (params or vars)
    return NULL;
}

// Helper function to find STATIC declarations in AST scopes
AST* findStaticDeclarationInAST(const char* varName, AST* currentScopeNode, AST* globalProgramNode) {
    if (!varName) return NULL;
    AST* foundDecl = NULL;

    // 1. Search Local Scope (if inside one)
    if (currentScopeNode && currentScopeNode != globalProgramNode) {
        if (currentScopeNode->type == AST_PROCEDURE_DECL || currentScopeNode->type == AST_FUNCTION_DECL) {
            // 1a. Check Parameters
            for (int i = 0; i < currentScopeNode->child_count; i++) {
                AST* paramDeclGroup = currentScopeNode->children[i];
                if (paramDeclGroup && paramDeclGroup->type == AST_VAR_DECL) {
                     for (int j = 0; j < paramDeclGroup->child_count; j++) {
                         AST* paramNameNode = paramDeclGroup->children[j];
                         if (paramNameNode && paramNameNode->type == AST_VARIABLE && paramNameNode->token &&
                             strcasecmp(paramNameNode->token->value, varName) == 0) {
                             foundDecl = paramDeclGroup; // Found param
                             goto found_in_scope; // Exit loops early
                         }
                     }
                }
            }
             // 1b. Check Function Name (as implicit variable)
             if (!foundDecl && currentScopeNode->type == AST_FUNCTION_DECL) {
                 if (strcasecmp(currentScopeNode->token->value, varName) == 0 || strcasecmp("result", varName) == 0) {
                      foundDecl = currentScopeNode; // Return FUNCTION_DECL itself
                      goto found_in_scope;
                 }
             }

            // 1c. Check Local Variables within the procedure/function's BLOCK
            AST* blockNode = (currentScopeNode->type == AST_PROCEDURE_DECL) ? currentScopeNode->right : currentScopeNode->extra;
            if (!foundDecl && blockNode && blockNode->type == AST_BLOCK && blockNode->child_count > 0) {
                AST* declarationsNode = blockNode->children[0];
                if (declarationsNode && declarationsNode->type == AST_COMPOUND) {
                    for (int i = 0; i < declarationsNode->child_count; i++) {
                        AST* declGroup = declarationsNode->children[i];
                        if (declGroup && declGroup->type == AST_VAR_DECL) {
                            for (int j = 0; j < declGroup->child_count; j++) {
                                AST* varNameNode = declGroup->children[j];
                                if (varNameNode && varNameNode->token && strcasecmp(varNameNode->token->value, varName) == 0) {
                                    foundDecl = declGroup; // Found local var
                                    goto found_in_scope; // Exit loops early
                                }
                            }
                        }
                    }
                }
            }
        }
    }
found_in_scope:; // Label for goto jump

    // 2. Search Global Scope if not found locally
    if (!foundDecl && globalProgramNode && globalProgramNode->type == AST_PROGRAM && globalProgramNode->right &&
        globalProgramNode->right->type == AST_BLOCK && globalProgramNode->right->child_count > 0)
    {
         AST* globalDeclarationsNode = globalProgramNode->right->children[0];
         if (globalDeclarationsNode && globalDeclarationsNode->type == AST_COMPOUND) {
             for (int i = 0; i < globalDeclarationsNode->child_count; i++) {
                 AST* declGroup = globalDeclarationsNode->children[i];
                 if (declGroup && declGroup->type == AST_VAR_DECL) {
                     for (int j = 0; j < declGroup->child_count; j++) {
                         AST* varNameNode = declGroup->children[j];
                         if (varNameNode && varNameNode->token && strcasecmp(varNameNode->token->value, varName) == 0) {
                             foundDecl = declGroup; // Found global var
                             goto found_global; // Exit loops
                         }
                     }
                 }
             }
         }
    }
found_global:;

    return foundDecl; // Return found declaration node (VAR_DECL or FUNCTION_DECL) or NULL
}

void annotateTypes(AST *node, AST *currentScopeNode, AST *globalProgramNode) {
    if (!node) return;

    AST *childScopeNode = currentScopeNode;
    if (node->type == AST_PROCEDURE_DECL || node->type == AST_FUNCTION_DECL) {
        childScopeNode = node;
    }

    // Post-order Traversal
    if (node->left) annotateTypes(node->left, childScopeNode, globalProgramNode);
    if (node->right) annotateTypes(node->right, childScopeNode, globalProgramNode);
    if (node->extra) annotateTypes(node->extra, childScopeNode, globalProgramNode);
    for (int i = 0; i < node->child_count; ++i) {
         if(node->children && node->children[i]) {
              annotateTypes(node->children[i], childScopeNode, globalProgramNode);
         }
    }

    // Set Type for Current Node
    switch(node->type) {
        // Literals
        case AST_NUMBER: node->var_type = (node->token && node->token->type == TOKEN_REAL_CONST) ? TYPE_REAL : TYPE_INTEGER; break;
        case AST_STRING: node->var_type = TYPE_STRING; break;
        // Removed AST_CHAR_CONST
        case AST_BOOLEAN: node->var_type = TYPE_BOOLEAN; break;
        case AST_ENUM_VALUE: node->var_type = TYPE_ENUM; break;
        case AST_SET: node->var_type = TYPE_SET; break;

        // Variable Usage
        case AST_VARIABLE: {
            const char* varName = node->token ? node->token->value : NULL;
            if (!varName) break;

            // Skip special identifiers that aren't variables needing type lookup here
            bool skipLookup = false;
            if (node->parent) {
                 if (node->parent->type == AST_PROGRAM && node == node->parent->left) skipLookup = true;
                 else if (node->parent->type == AST_VAR_DECL && node == node->parent->right) skipLookup = true;
                 else if (node->parent->type == AST_TYPE_DECL && node == node->parent->left) skipLookup = true;
                 else if (node->parent->type == AST_TYPE_REFERENCE) skipLookup = true;
                 // *** FIX: Removed incorrect pointer comparisons ***
                 else if (node->parent->type == AST_FIELD_ACCESS && node->token == node->parent->token) skipLookup = true; // Skip the field *name* token itself
                 else if ((node->parent->type == AST_FUNCTION_DECL || node->parent->type == AST_PROCEDURE_DECL) && node->token == node->parent->token) skipLookup = true; // Skip the proc/func *name* token itself
                 else if ((node->parent->type == AST_FUNCTION_DECL) && node == node->parent->right) skipLookup = true; // Skip return type node
            }
            if (skipLookup) break;

#ifdef DEBUG
            fprintf(stderr, "[Annotate] Processing VARIABLE Usage: %s in scope %s (Current node type: %s)\n",
                    varName, currentScopeNode ? currentScopeNode->token->value : "Global", varTypeToString(node->var_type));
#endif
            // 1. Static lookup in AST scopes
            AST* declNode = findStaticDeclarationInAST(varName, currentScopeNode, globalProgramNode);

            if (declNode) {
                 AST* typeNode = NULL;
                 if (declNode->type == AST_FUNCTION_DECL) { typeNode = declNode->right; }
                 else { typeNode = declNode->right; } // Assumed AST_VAR_DECL

                 if(typeNode) {
                     AST* actualTypeNode = typeNode;
                     if (actualTypeNode->type == AST_TYPE_REFERENCE) {
                         actualTypeNode = actualTypeNode->right; // Resolve reference
                     }
                     if (actualTypeNode) {
                         node->var_type = actualTypeNode->var_type; // Get type from definition
                         // setRight(node, actualTypeNode); // <<< REMOVED/COMMENTED OUT >>>
 #ifdef DEBUG
                         fprintf(stderr, "[Annotate]   Found declaration for '%s'. Set type to %s.\n", varName, varTypeToString(node->var_type));
 #endif
                     } else { node->var_type = TYPE_VOID; /* Broken link? */ }
                 } else { node->var_type = TYPE_VOID; /* Decl node missing type? */ }
             } else {
                 // ... (Type lookup fallback) ...
                  AST* typeNode = lookupType(varName);
                  if (typeNode) {
                      // ... (Set var_type from type lookup) ...
                      node->var_type = typeNode->var_type;
                      // setRight(node, typeNode); // <<< REMOVED/COMMENTED OUT >>>
                  } else {
                       // ... (Undeclared) ...
                  }
             }
#ifdef DEBUG
            fprintf(stderr, "[Annotate]   After processing, node '%s' var_type is now: %s\n", varName, varTypeToString(node->var_type));
#endif
            break;
        } // End AST_VARIABLE case

        // Operations
        case AST_BINARY_OP: {
            VarType leftType = node->left ? node->left->var_type : TYPE_VOID;
            VarType rightType = node->right ? node->right->var_type : TYPE_VOID;
            // Keep your existing type inference logic here, ensure it handles VOID inputs
            // ... (Your logic) ...
             if (node->token->type == TOKEN_SLASH || leftType == TYPE_REAL || rightType == TYPE_REAL) node->var_type = TYPE_REAL;
             else if (node->token->type == TOKEN_PLUS && (leftType == TYPE_STRING || rightType == TYPE_STRING || leftType == TYPE_CHAR || rightType == TYPE_CHAR)) node->var_type = TYPE_STRING;
             else if (node->token->type == TOKEN_EQUAL || node->token->type == TOKEN_NOT_EQUAL || node->token->type == TOKEN_LESS || node->token->type == TOKEN_LESS_EQUAL || node->token->type == TOKEN_GREATER || node->token->type == TOKEN_GREATER_EQUAL || node->token->type == TOKEN_AND || node->token->type == TOKEN_OR || node->token->type == TOKEN_IN) node->var_type = TYPE_BOOLEAN;
             else if (leftType == TYPE_INTEGER && rightType == TYPE_INTEGER) node->var_type = TYPE_INTEGER;
             else if (leftType == TYPE_SET || rightType == TYPE_SET) node->var_type = TYPE_SET;
             else if (leftType != TYPE_VOID && rightType == TYPE_VOID) node->var_type = leftType; // Propagate if one side is known
             else if (leftType == TYPE_VOID && rightType != TYPE_VOID) node->var_type = rightType; // Propagate if one side is known
             else node->var_type = TYPE_INTEGER; // Default guess - risky
#ifdef DEBUG
             fprintf(stderr, "[Annotate] Binary Op %s (%s, %s) -> %s\n", node->token->value, varTypeToString(leftType), varTypeToString(rightType), varTypeToString(node->var_type));
#endif
            break;
        }
        case AST_UNARY_OP:
            node->var_type = (node->token->type == TOKEN_NOT) ? TYPE_BOOLEAN : (node->left ? node->left->var_type : TYPE_VOID);
            break;

        // Calls (Using static procedure_table lookup)
        case AST_PROCEDURE_CALL: {
             Procedure *proc = lookupProcedure(node->token->value);
             if (proc && proc->proc_decl && proc->proc_decl->type == AST_FUNCTION_DECL) {
                 node->var_type = proc->proc_decl->var_type;
                 if (proc->proc_decl->right) setRight(node, proc->proc_decl->right);
             } else {
                 node->var_type = TYPE_VOID;
             }
#ifdef DEBUG
             fprintf(stderr, "[Annotate] Call '%s' annotated with type %s\n", node->token->value, varTypeToString(node->var_type));
#endif
             break;
         }

        // Accessors
        case AST_FIELD_ACCESS: {
             node->var_type = TYPE_VOID; // Default
             // Check left child is annotated as RECORD and linked to its type definition
             if(node->left && node->left->var_type == TYPE_RECORD && node->left->right) {
                 AST* recordTypeDef = node->left->right;
                 if (recordTypeDef && recordTypeDef->type == AST_TYPE_REFERENCE) recordTypeDef = recordTypeDef->right; // Resolve ref

                 if(recordTypeDef && recordTypeDef->type == AST_RECORD_TYPE) {
                     // *** FIX: Inlined field lookup logic ***
                     const char* fieldName = node->token->value;
                     AST* fieldDeclGroup = NULL;
                     AST* fieldTypeNode = NULL;
                     // Iterate through VAR_DECL children of the RECORD_TYPE node
                     for (int i = 0; i < recordTypeDef->child_count; ++i) {
                          AST* currentDeclGroup = recordTypeDef->children[i];
                          if (!currentDeclGroup || currentDeclGroup->type != AST_VAR_DECL) continue;
                          // Iterate through variable names within this VAR_DECL group
                          for (int j = 0; j < currentDeclGroup->child_count; ++j) {
                               AST* varNameNode = currentDeclGroup->children[j];
                               if (varNameNode && varNameNode->token && strcasecmp(varNameNode->token->value, fieldName) == 0) {
                                    fieldDeclGroup = currentDeclGroup; // Found the group
                                    break;
                               }
                          }
                          if (fieldDeclGroup) break;
                     }
                     if(fieldDeclGroup && fieldDeclGroup->right) {
                          fieldTypeNode = fieldDeclGroup->right;
                          // Resolve field type if it's a reference
                          if (fieldTypeNode->type == AST_TYPE_REFERENCE) {
                              fieldTypeNode = fieldTypeNode->right;
                          }
                          if(fieldTypeNode){
                              node->var_type = fieldTypeNode->var_type;
                              setRight(node, fieldTypeNode); // Link field access node to field's type node
                          }
                     } // else: Field not found, remains VOID
                 }
             }
#ifdef DEBUG
              fprintf(stderr, "[Annotate] Field Access '%s' annotated with type %s\n", node->token->value, varTypeToString(node->var_type));
#endif
             break;
        }
         case AST_ARRAY_ACCESS: {
              node->var_type = TYPE_VOID; // Default
              if(node->left && node->left->right && node->left->var_type == TYPE_ARRAY) {
                 AST* arrayTypeDef = node->left->right;
                  if (arrayTypeDef && arrayTypeDef->type == AST_TYPE_REFERENCE) arrayTypeDef = arrayTypeDef->right; // Resolve ref
                 if(arrayTypeDef && arrayTypeDef->type == AST_ARRAY_TYPE && arrayTypeDef->right) {
                     AST* elemTypeNode = arrayTypeDef->right; // Element type node is right child
                     // Resolve element type if it's a reference
                     if(elemTypeNode && elemTypeNode->type == AST_TYPE_REFERENCE) {
                         elemTypeNode = elemTypeNode->right;
                     }
                     if (elemTypeNode) {
                         node->var_type = elemTypeNode->var_type;
                         setRight(node, elemTypeNode); // Link usage to actual element type node
                     }
                 }
             }
#ifdef DEBUG
              fprintf(stderr, "[Annotate] Array Access annotated with type %s\n", varTypeToString(node->var_type));
#endif
             break;
         }

        // Other node types
        default:
             // *** FIX: Replace UNKNOWN_VAR_TYPE with TYPE_VOID ***
             if (node->var_type == TYPE_VOID) { // Was it previously unassigned?
                // Potentially assign default VOID, but be careful not to overwrite
                // types correctly set by the parser (like for declarations)
             }
             break;
    }
}

// In src/ast.c (Replace the previous copyAST with this)
#include <string.h> // For strdup if needed by copyToken
#include <stdlib.h> // For malloc, NULL
// Include ast.h, list.h, utils.h etc.

AST *copyAST(AST *node) {
    if (!node) return NULL;

    // Create a new node of the same type, copying the token
    // copyToken handles copying token->value (string, etc.)
    AST *newNode = newASTNode(node->type, node->token);
    if (!newNode) return NULL; // Allocation failed

    // Copy simple properties from the AST struct definition
    newNode->var_type = node->var_type;
    newNode->by_ref = node->by_ref;
    newNode->is_global_scope = node->is_global_scope;
    newNode->i_val = node->i_val; // Copy the integer value field

    // Note: symbol_table and unit_list are not deep copied here.
    // If they need deep copies, that logic would be added.
    // For now, let's assume they are metadata/references managed elsewhere.
    newNode->unit_list = node->unit_list;     // Shallow copy (pointer)
    newNode->symbol_table = node->symbol_table; // Shallow copy (pointer)


    // Recursively copy subtrees
    AST *copiedLeft = copyAST(node->left);
    AST *copiedRight = copyAST(node->right);
    AST *copiedExtra = copyAST(node->extra);

    // --- Safely assign copied subtrees and set parent pointers ---
    newNode->left = copiedLeft;
    if (newNode->left) newNode->left->parent = newNode;

    newNode->right = copiedRight;
    if (newNode->right) newNode->right->parent = newNode;

    newNode->extra = copiedExtra;
    if (newNode->extra) newNode->extra->parent = newNode;
    // ---

    // Copy children array
    if (node->child_count > 0 && node->children) {
        newNode->child_capacity = node->child_count; // Allocate exact capacity needed
        newNode->child_count = node->child_count;
        newNode->children = malloc(sizeof(AST*) * newNode->child_capacity);
        if (!newNode->children) { freeAST(newNode); return NULL; } // Allocation failed

        // Initialize child pointers to NULL
        for (int i = 0; i < newNode->child_count; i++) {
             newNode->children[i] = NULL;
        }

        // Copy each child
        for (int i = 0; i < node->child_count; i++) {
            newNode->children[i] = copyAST(node->children[i]);
            // Check if copy failed for a non-NULL original child
            if (!newNode->children[i] && node->children[i]) {
                 // Important: Need to free already copied children before returning NULL
                 for(int j = 0; j < i; ++j) { // Free successfully copied children so far
                     freeAST(newNode->children[j]);
                 }
                 free(newNode->children); // Free the children array itself
                 newNode->children = NULL;
                 newNode->child_count = 0;
                 newNode->child_capacity = 0;
                 // Free other copied parts (left, right, extra, token, node itself)
                 freeAST(newNode);
                 return NULL; // Indicate copy failure
             }
            // Set parent pointer in the successfully copied child
            if (newNode->children[i]) {
                newNode->children[i]->parent = newNode;
            }
        }
    } else {
        newNode->children = NULL;
        newNode->child_count = 0;
        newNode->child_capacity = 0;
    }

    return newNode;
}

// Function to recursively verify parent pointers in the AST
// Returns true if all links are consistent, false otherwise.
// Prints errors to stderr upon finding inconsistencies.
bool verifyASTLinks(AST *node, AST *expectedParent) {
    if (!node) return true; // Base case: NULL node is valid

    bool links_ok = true;

    // --- ADDED: Detailed Debug Print BEFORE check ---
    #ifdef DEBUG // Optional: Use your existing DEBUG flag
    fprintf(stderr, "[VERIFY_CHECK] Node %p (Type: %s, Token: '%s'), Actual Parent: %p, Expected Parent Param: %p\n",
            (void*)node,
            astTypeToString(node->type),
            (node->token && node->token->value) ? node->token->value : "NULL",
            (void*)node->parent,
            (void*)expectedParent);
    #endif
    // --- END ADDED DEBUG PRINT ---

    // 1. Check current node's parent pointer
    if (node->parent != expectedParent) {
        // Existing error print
        fprintf(stderr, "AST Link Error: Node %p (Type: %s, Token: '%s') has parent %p, but expected %p\n",
                (void*)node,
                astTypeToString(node->type),
                (node->token && node->token->value) ? node->token->value : "NULL",
                (void*)node->parent,
                (void*)expectedParent);
        links_ok = false;
    }

    // 2. Recursively check subtrees, passing 'node' as the expectedParent
    if (!verifyASTLinks(node->left, node)) {
        links_ok = false;
    }
    if (!verifyASTLinks(node->right, node)) {
        links_ok = false;
    }
    if (!verifyASTLinks(node->extra, node)) {
        links_ok = false;
    }

    // 3. Recursively check children array
    if (node->children) {
        for (int i = 0; i < node->child_count; i++) {
            // Pass 'node' as the expected parent for each child
            if (node->children[i]) {
                 // --- ADDED: Debug Print BEFORE recursive call for child ---
                 #ifdef DEBUG
                 fprintf(stderr, "[VERIFY_RECURSE] Calling verify for Child %d of Node %p. Child Node: %p, Passing Expected Parent: %p\n",
                         i, (void*)node, (void*)node->children[i], (void*)node);
                 #endif
                 // --- END ADDED DEBUG PRINT ---
                 if (!verifyASTLinks(node->children[i], node)) {
                     links_ok = false;
                 }
            }
        }
    }

    return links_ok;
}
