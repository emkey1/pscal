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

    // --- ADDED: Check if node is directly in type_table ---
    // If so, let freeTypeTableASTNodes handle it exclusively.
    // This requires the original main.c cleanup order: freeAST(Global) then freeTypeTable.
    if (isNodeInTypeTable(node)) {
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG_FREE] Postponing free for Node %p (Type: %s) as it's in type_table.\n",
                (void*)node, astTypeToString(node->type));
        #endif
        return; // Don't free contents or node itself here
    }
    // --- END ADDED CHECK ---

    bool skip_right_free = (node->type == AST_TYPE_REFERENCE);
    bool skip_left_free = (node->type == AST_TYPE_DECL); // Keep this from previous fix

    #ifdef DEBUG
    fprintf(stderr, "[DEBUG_FREE] Enter freeAST for Node %p (Type: %s, Token: '%s')\n",
            (void*)node,
            astTypeToString(node->type),
            (node->token && node->token->value) ? node->token->value : "NULL");
    #endif

    // Free Left, Extra, and Children first (using skip flags)
    if (node->left) {
        if (!skip_left_free) {
            #ifdef DEBUG
            fprintf(stderr, "[DEBUG_FREE]  - Recursing Left from Node %p into Node %p (Type: %s)\n", (void*)node, (void*)node->left, astTypeToString(node->left->type));
            #endif
            freeAST(node->left);
        } else {
             #ifdef DEBUG
             fprintf(stderr, "[DEBUG_FREE]  - Skipping freeAST(node->left) for TYPE_DECL node %p\n", (void*)node);
             #endif
        }
        node->left = NULL;
    }

    if (node->extra) {
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG_FREE]  - Recursing Extra from Node %p into Node %p (Type: %s)\n", (void*)node, (void*)node->extra, astTypeToString(node->extra->type));
        #endif
        freeAST(node->extra);
        node->extra = NULL;
    }
    if (node->children) {
        for (int i = 0; i < node->child_count; i++) {
            if (node->children[i]) {
                #ifdef DEBUG
                fprintf(stderr, "[DEBUG_FREE]  - Recursing Child %d from Node %p into Node %p (Type: %s)\n", i, (void*)node, (void*)node->children[i], astTypeToString(node->children[i]->type));
                #endif
                freeAST(node->children[i]);
                node->children[i] = NULL;
            }
        }
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG_FREE]  - Freeing children array for Node %p\n", (void*)node);
        #endif
        free(node->children);
        node->children = NULL;
        node->child_count = 0;
        node->child_capacity = 0;
    }

    // Conditionally free the 'right' subtree
    if (node->right) {
        if (!skip_right_free) {
            #ifdef DEBUG
            fprintf(stderr, "[DEBUG_FREE]  - Recursing Right from Node %p (%s) into Node %p (%s)\n", (void*)node, astTypeToString(node->type), (void*)node->right, astTypeToString(node->right->type));
            #endif
            freeAST(node->right);
        } else {
            #ifdef DEBUG
            fprintf(stderr, "[DEBUG_FREE]  - Skipping freeAST(node->right) for TYPE_REFERENCE node %p\n", (void*)node);
            #endif
        }
        node->right = NULL;
    }

    // Free token
    if (node->token) {
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG_FREE]  - Freeing Token for Node %p\n", (void*)node);
        #endif
        freeToken(node->token);
        node->token = NULL;
    }

    // Free the node struct itself
    #ifdef DEBUG
    fprintf(stderr, "[DEBUG_FREE] Freeing Node struct %p itself (Type: %s)\n", (void*)node, astTypeToString(node->type));
    #endif
    free(node);
}

// --- dumpASTFromRoot Function ---
void dumpASTFromRoot(AST *node) {
    if (!node) return;
    while (node->parent != NULL) {
        node = node->parent;
    }
    dumpAST(node, 0);
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
        case AST_BOOLEAN: node->var_type = TYPE_BOOLEAN; break;
        case AST_ENUM_VALUE: node->var_type = TYPE_ENUM; break;
        case AST_SET: node->var_type = TYPE_SET; break;
        case AST_ARRAY_LITERAL: node->var_type = TYPE_ARRAY; break;

        // Variable Usage
        case AST_VARIABLE: {
            const char* varName = node->token ? node->token->value : NULL;
            if (!varName) { node->var_type = TYPE_VOID; break; }

            bool skipLookup = false;
             if (node->parent) {
                 if (node->parent->type == AST_PROGRAM && node == node->parent->left) skipLookup = true;
                 else if (node->parent->type == AST_VAR_DECL && node == node->parent->right) skipLookup = true;
                 else if (node->parent->type == AST_TYPE_DECL && node == node->parent->left) skipLookup = true;
                 else if (node->parent->type == AST_TYPE_REFERENCE) skipLookup = true;
                 else if (node->parent->type == AST_FIELD_ACCESS && node->token == node->parent->token) skipLookup = true;
                 else if ((node->parent->type == AST_FUNCTION_DECL || node->parent->type == AST_PROCEDURE_DECL) && node->token == node->parent->token) skipLookup = true;
                 else if ((node->parent->type == AST_FUNCTION_DECL) && node == node->parent->right) skipLookup = true;
             }
            if (skipLookup) break;

            // Only annotate if type is currently VOID (or default)
            // This prevents overwriting types set correctly earlier by the parser for declarations.
            if (node->var_type == TYPE_VOID) { // <<< Check against TYPE_VOID
                 AST* declNode = findStaticDeclarationInAST(varName, currentScopeNode, globalProgramNode);
                 if (declNode) {
                     // Get type info from the declaration node
                     if (declNode->type == AST_FUNCTION_DECL) {
                         node->var_type = declNode->var_type;
                     } else if (declNode->type == AST_VAR_DECL) {
                         node->var_type = declNode->var_type;
                     } else if (declNode->type == AST_CONST_DECL) {
                         // Constant type inference might be tricky, rely on what parser/eval might set
                         // or attempt inference from declNode->left if needed.
                         if (declNode->left) node->var_type = declNode->left->var_type; // If left side has type
                         else node->var_type = TYPE_VOID; // Fallback
                     }
                 } else {
                      AST* typeDef = lookupType(varName);
                      if (typeDef) {
                           node->var_type = typeDef->var_type;
                      } else {
                           #ifdef DEBUG
                           fprintf(stderr, "[Annotate Warning] Undeclared identifier '%s' used in expression.\n", varName);
                           #endif
                           node->var_type = TYPE_VOID; // Mark as VOID/error
                      }
                 }
            }
            break;
        } // End AST_VARIABLE case

        // Operations
        case AST_BINARY_OP: {
             VarType leftType = node->left ? node->left->var_type : TYPE_VOID;
             VarType rightType = node->right ? node->right->var_type : TYPE_VOID;
             // Your existing type inference logic (kept from previous correct version)
             if (node->token->type == TOKEN_SLASH || leftType == TYPE_REAL || rightType == TYPE_REAL) node->var_type = TYPE_REAL;
             else if (node->token->type == TOKEN_PLUS && (leftType == TYPE_STRING || rightType == TYPE_STRING || leftType == TYPE_CHAR || rightType == TYPE_CHAR)) node->var_type = TYPE_STRING;
             else if (node->token->type == TOKEN_EQUAL || node->token->type == TOKEN_NOT_EQUAL || node->token->type == TOKEN_LESS || node->token->type == TOKEN_LESS_EQUAL || node->token->type == TOKEN_GREATER || node->token->type == TOKEN_GREATER_EQUAL || node->token->type == TOKEN_AND || node->token->type == TOKEN_OR || node->token->type == TOKEN_IN) node->var_type = TYPE_BOOLEAN;
             else if (leftType == TYPE_INTEGER && rightType == TYPE_INTEGER) node->var_type = TYPE_INTEGER;
             else if (leftType == TYPE_SET || rightType == TYPE_SET) node->var_type = TYPE_BOOLEAN; // IN op results in BOOLEAN, other set ops need handling
             else if (leftType != TYPE_VOID && rightType == TYPE_VOID) node->var_type = leftType;
             else if (leftType == TYPE_VOID && rightType != TYPE_VOID) node->var_type = rightType;
             else node->var_type = TYPE_INTEGER; // Default guess
            break;
        }
        case AST_UNARY_OP:
            node->var_type = (node->token->type == TOKEN_NOT) ? TYPE_BOOLEAN : (node->left ? node->left->var_type : TYPE_VOID);
            break;

        // Calls
        case AST_PROCEDURE_CALL: {
             // Only annotate if type is currently VOID
             if (node->var_type == TYPE_VOID) {
                  Procedure *proc = lookupProcedure(node->token->value);
                  if (proc && proc->proc_decl && proc->proc_decl->type == AST_FUNCTION_DECL) {
                      node->var_type = proc->proc_decl->var_type;
                  } else {
                      BuiltinRoutineType btype = getBuiltinType(node->token->value);
                      if (btype == BUILTIN_TYPE_FUNCTION) {
                           // TODO: Lookup actual builtin return type if possible
                           // If registerBuiltinFunction correctly sets var_type on the dummy AST,
                           // maybe proc->proc_decl->var_type works even for builtins? Check registration.
                           // For now, leave as VOID and let eval determine type? Or make best guess?
                      } else {
                           node->var_type = TYPE_VOID; // Procedures return void
                      }
                  }
             }
             break;
         }

        // Accessors
        case AST_FIELD_ACCESS: {
             node->var_type = TYPE_VOID; // Default
             if(node->left && (node->left->var_type == TYPE_RECORD)) {
                AST* recordDeclNode = node->left->right;
                if (recordDeclNode && recordDeclNode->type == AST_TYPE_REFERENCE) recordDeclNode = recordDeclNode->right;
                if (recordDeclNode && recordDeclNode->type == AST_RECORD_TYPE) {
                    const char* fieldName = node->token ? node->token->value : NULL;
                    if (fieldName) {
                        // Find field declaration logic...
                        for (int i = 0; i < recordDeclNode->child_count; ++i) {
                            AST* fieldDeclGroup = recordDeclNode->children[i];
                            if (!fieldDeclGroup || fieldDeclGroup->type != AST_VAR_DECL) continue;
                            for (int j = 0; j < fieldDeclGroup->child_count; ++j) {
                                 AST* varNameNode = fieldDeclGroup->children[j];
                                 if (varNameNode && varNameNode->token && strcasecmp(varNameNode->token->value, fieldName) == 0) {
                                      AST* fieldTypeNode = fieldDeclGroup->right;
                                      if (fieldTypeNode) {
                                           if(fieldTypeNode->type == AST_TYPE_REFERENCE) fieldTypeNode = fieldTypeNode->right;
                                           if(fieldTypeNode) node->var_type = fieldTypeNode->var_type;
                                      }
                                      goto field_found_annotate_corrected; // Use corrected label
                                 }
                            }
                        }
                        field_found_annotate_corrected:; // Corrected label
                    }
                }
             }
             break;
        }
         case AST_ARRAY_ACCESS: {
              node->var_type = TYPE_VOID; // Default
              if(node->left && node->left->var_type == TYPE_ARRAY) {
                 AST* arrayDeclNode = node->left->right;
                 if (arrayDeclNode && arrayDeclNode->type == AST_TYPE_REFERENCE) arrayDeclNode = arrayDeclNode->right;
                 if(arrayDeclNode && arrayDeclNode->type == AST_ARRAY_TYPE && arrayDeclNode->right) {
                     AST* elemTypeNode = arrayDeclNode->right;
                     if(elemTypeNode && elemTypeNode->type == AST_TYPE_REFERENCE) elemTypeNode = elemTypeNode->right;
                     if (elemTypeNode) node->var_type = elemTypeNode->var_type;
                 }
             } else if (node->left && node->left->var_type == TYPE_STRING) {
                 node->var_type = TYPE_CHAR;
             }
             break;
         }
        default:
             break;
    } // End switch
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
