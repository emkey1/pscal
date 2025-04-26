#include "interpreter.h"
#include "builtin.h"
#include "symbol.h"
#include "utils.h"
#include "globals.h"
#include <stdio.h>
#include <stdlib.h>

AST *GlobalASTRoot = NULL;  // Declare global AST root

// Inside pscal/interpreter.c
void popLocalEnv(void) {
    Symbol *sym = localSymbols;
    
#ifdef DEBUG
    fprintf(stderr, "[DEBUG] Popping local env (localSymbols=%p)\n", (void*)sym);
#endif
    while (sym) {
        Symbol *next = sym->next;
#ifdef DEBUG
        fprintf(stderr, "[DEBUG]   Processing local symbol '%s' at %p (is_alias=%d, is_local_var=%d)\n", // Added is_local_var
                sym->name ? sym->name : "NULL", (void*)sym, sym->is_alias, sym->is_local_var); // Added is_local_var
#endif

        // *** FIX: Only free Value if it's NOT an alias AND NOT a standard local variable ***
        if (sym->value && !sym->is_alias && !sym->is_local_var) {
#ifdef DEBUG
             fprintf(stderr, "[DEBUG]     Freeing Value* at %p for non-alias, non-local symbol '%s'\n", (void*)sym->value, sym->name ? sym->name : "NULL");
#endif
             freeValue(sym->value);
             free(sym->value); // Free the Value struct itself
             sym->value = NULL; // Prevent dangling pointer in Symbol struct
        } else if (sym->value && (sym->is_alias || sym->is_local_var)) { // Log why it's skipped
#ifdef DEBUG
             fprintf(stderr, "[DEBUG]     Skipping freeValue for symbol '%s' (value=%p, alias=%d, local=%d)\n",
                     sym->name ? sym->name : "NULL", (void*)sym->value, sym->is_alias, sym->is_local_var);
#endif
        }
        // ... (free name, free symbol struct) ...
         if (sym->name) {
#ifdef DEBUG
            fprintf(stderr, "[DEBUG]     Freeing name '%s' at %p\n", sym->name, (void*)sym->name);
#endif
            free(sym->name);
        }
#ifdef DEBUG
        fprintf(stderr, "[DEBUG]     Freeing Symbol* at %p\n", (void*)sym);
#endif
        free(sym); // Free the Symbol struct itself
        sym = next;
    }
    localSymbols = NULL;
#ifdef DEBUG
    fprintf(stderr, "[DEBUG] Finished popping local env\n");
#endif
}

// Add this structure to snapshot and restore local environment safely
typedef struct SymbolEnvSnapshot {
    Symbol *head;
} SymbolEnvSnapshot;

void saveLocalEnv(SymbolEnvSnapshot *snap) {
    snap->head = localSymbols;
    localSymbols = NULL;
}

void restoreLocalEnv(SymbolEnvSnapshot *snap) {
    popLocalEnv();
    localSymbols = snap->head;
}

// Note: This is adapted from the diff provided. Ensure it integrates
// with your Value structure and makeChar/makeInt helpers.
Value evalSet(AST *node) {
    // ... (initial checks and variable declarations remain the same) ...
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_SET;
    v.set_val.set_size = 0;
    v.set_val.set_values = NULL;
    int capacity = 0;

    for (int i = 0; i < node->child_count; i++) {
        AST *element = node->children[i];
        if (element->type == AST_SUBRANGE) {
            Value start = eval(element->left);
            Value end = eval(element->right);

            // --- Modification Start: Allow single-char strings ---
            bool start_is_ordinal = (start.type == TYPE_INTEGER || (start.type == TYPE_STRING && start.s_val && strlen(start.s_val) == 1));
            bool end_is_ordinal = (end.type == TYPE_INTEGER || (end.type == TYPE_STRING && end.s_val && strlen(end.s_val) == 1));
            // Check for type compatibility (both int or both char-like string)
            bool types_compatible = (start.type == TYPE_INTEGER && end.type == TYPE_INTEGER) ||
                                    ((start.type == TYPE_STRING && start_is_ordinal) && (end.type == TYPE_STRING && end_is_ordinal));

            if (!start_is_ordinal || !end_is_ordinal || !types_compatible) {
                 fprintf(stderr, "Runtime error: Set range elements must be compatible ordinals (integer or single char). Start: %s, End: %s\n",
                         varTypeToString(start.type), varTypeToString(end.type));
                 EXIT_FAILURE_HANDLER();
            }

            // Get ordinal value (integer value or ASCII value of char from string)
            long long start_ord = (start.type == TYPE_INTEGER) ? start.i_val : start.s_val[0];
            long long end_ord   = (end.type == TYPE_INTEGER) ? end.i_val : end.s_val[0];
            // --- Modification End ---

            if (start_ord > end_ord) {
               // Empty range is valid
            } else {
               for (long long val = start_ord; val <= end_ord; val++) {
                   // Check duplicates, resize, add ordinal 'val'
                   // ... (duplicate check, resize, and add logic remains the same, using v.set_val.*) ...
                    int exists = 0;
                    for(int k = 0; k < v.set_val.set_size; k++) { if (v.set_val.set_values[k] == val) { exists = 1; break; } }
                    if (exists) continue;
                    if (v.set_val.set_size >= capacity) {
                        capacity = (capacity == 0) ? 8 : capacity * 2;
                        long long *new_values = realloc(v.set_val.set_values, capacity * sizeof(long long));
                        if (!new_values) { /* error */ EXIT_FAILURE_HANDLER(); }
                        v.set_val.set_values = new_values;
                    }
                    v.set_val.set_values[v.set_val.set_size++] = val;
                }
            }
        } else { // Single element
            Value elemVal = eval(element);
            // --- Modification Start: Allow single-char strings ---
            bool elem_is_ordinal = (elemVal.type == TYPE_INTEGER || (elemVal.type == TYPE_STRING && elemVal.s_val && strlen(elemVal.s_val) == 1));

            if (!elem_is_ordinal) {
                 fprintf(stderr, "Runtime error: Set elements must be ordinal type (integer or single char). Got %s\n", varTypeToString(elemVal.type));
                 EXIT_FAILURE_HANDLER();
            }

            long long elem_ord = (elemVal.type == TYPE_INTEGER) ? elemVal.i_val : elemVal.s_val[0];
            // --- Modification End ---

            // Check duplicates, resize, add ordinal 'elem_ord'
            // ... (duplicate check, resize, and add logic remains the same, using v.set_val.*) ...
            int exists = 0;
            for(int k = 0; k < v.set_val.set_size; k++) { if (v.set_val.set_values[k] == elem_ord) { exists = 1; break; } }
            if (exists) continue;
            if (v.set_val.set_size >= capacity) {
                 capacity = (capacity == 0) ? 8 : capacity * 2;
                 long long *new_values = realloc(v.set_val.set_values, capacity * sizeof(long long));
                 if (!new_values) { /* error */ EXIT_FAILURE_HANDLER(); }
                 v.set_val.set_values = new_values;
            }
            v.set_val.set_values[v.set_val.set_size++] = elem_ord;
        }
    }
    return v;
}

Value executeProcedureCall(AST *node) {
    // --- Verify Input Node ---
    if (!node || (node->type != AST_PROCEDURE_CALL && node->type != AST_FUNCTION_DECL) || !node->token) {
         fprintf(stderr, "Internal Error: Invalid AST node passed to executeProcedureCall.\n");
         // Optionally dump AST node details if possible
         EXIT_FAILURE_HANDLER();
    }
    // --- End Verify ---

    // Handle Built-in procedures first (User's original logic)
    if (isBuiltin(node->token->value)) {
        Value retVal = executeBuiltinProcedure(node);
#ifdef DEBUG
        fprintf(stderr, "DEBUG: Builtin procedure '%s' returned type %s\n",
                node->token->value, varTypeToString(retVal.type));
#endif
        return retVal;
    }

    // Look up user-defined procedure (User's original logic)
    Procedure *proc = procedure_table;
    // Use strcasecmp for case-insensitive lookup based on how names are stored/looked up
    char *lowerProcName = strdup(node->token->value);
    if (!lowerProcName) { fprintf(stderr,"FATAL: strdup failed for proc name lookup\n"); EXIT_FAILURE_HANDLER(); }
    for(int i=0; lowerProcName[i]; i++){ lowerProcName[i] = (char)tolower((unsigned char)lowerProcName[i]); }

    while (proc) {
        if (proc->name && strcmp(proc->name, lowerProcName) == 0) // Compare lowercase
            break;
        proc = proc->next;
    }
    free(lowerProcName); // Free the temp lowercase name

    if (!proc || !proc->proc_decl) { // Check proc and its declaration AST
        fprintf(stderr, "Runtime error: routine '%s' not found or declaration missing.\n", node->token->value);
        EXIT_FAILURE_HANDLER();
    }

    // Get expected number of parameters FROM THE DECLARATION
    int num_params = proc->proc_decl->child_count;

    // --- ADDED: Debug Check before accessing node->children ---
#ifdef DEBUG
    fprintf(stderr, "[DEBUG EXEC_PROC] ENTERING: Node %p (%s '%s'), Expecting %d params.\n",
            (void*)node, astTypeToString(node->type), node->token->value, num_params);
    fprintf(stderr, "[DEBUG EXEC_PROC]            AST Node State: child_count=%d, children_ptr=%p\n",
            node->child_count, (void*)node->children);
#endif

    // Defensive check: Does the CALL node's child count match expected params?
    if (node->child_count != num_params) {
         fprintf(stderr, "Runtime error: Argument count mismatch for call to '%s'. Expected %d, got %d.\n",
                 proc->name, num_params, node->child_count);
         // This indicates a parser error attaching arguments
         EXIT_FAILURE_HANDLER(); // Exit on mismatch
    }
    // Check if children pointer is NULL when arguments are expected
    if (num_params > 0 && node->children == NULL) {
         fprintf(stderr, "CRITICAL ERROR: Procedure '%s' expects %d params, but AST node->children pointer is NULL before argument evaluation!\n",
                 proc->name, num_params);
         dumpAST(node, 0); // Dump the node state *at execution time*
         dumpAST(proc->proc_decl, 0);
         EXIT_FAILURE_HANDLER();
    }
    // --- END ADDED Debug Check ---

    // Allocate temporary storage for evaluated/copied VALUE parameter values
    Value *arg_values = NULL;
    if (num_params > 0) {
        arg_values = malloc(sizeof(Value) * num_params);
        if (!arg_values) {
             fprintf(stderr, "FATAL: malloc failed for arg_values in executeProcedureCall\n");
             EXIT_FAILURE_HANDLER();
        }
        // Initialize memory - set all types to VOID initially
        for (int i = 0; i < num_params; i++) {
             arg_values[i].type = TYPE_VOID; // Mark as unused initially
        }
    }

    // --- Stage 1: Evaluate arguments for VALUE parameters (User's loop structure) ---
    for (int i = 0; i < num_params; i++) {
        AST *paramNode = proc->proc_decl->children[i]; // Formal parameter definition
        if (!paramNode) { /* Error checking */ fprintf(stderr, "Missing formal param %d\n", i); EXIT_FAILURE_HANDLER(); }

        if (paramNode->by_ref) { // VAR parameter
            // Leave arg_values[i] as TYPE_VOID; we handle VAR in the next loop using the AST node.
            arg_values[i].type = TYPE_VOID;
        } else { // VALUE parameter
             // --- ADDED: Check index and children pointer AGAIN before access ---
             if (i >= node->child_count || node->children == NULL) { // Check against actual args provided
                 fprintf(stderr, "CRITICAL ERROR: Trying to access actual argument node->children[%d], but child_count=%d or children=%p\n",
                         i, node->child_count, (void*)node->children);
                 dumpAST(node,0);
                 EXIT_FAILURE_HANDLER();
             }
             AST* actualArgNode = node->children[i]; // Get the AST node for the i-th argument in the call
             if (!actualArgNode) {
                 fprintf(stderr, "CRITICAL ERROR: Actual argument node at index %d is NULL for call to '%s'.\n", i, proc->name);
                 dumpAST(node,0);
                 EXIT_FAILURE_HANDLER();
             }
             // --- END ADDED Check ---
#ifdef DEBUG
            fprintf(stderr, "[DEBUG EXEC_PROC] Evaluating value parameter %d (AST Type: %s)\n", i, astTypeToString(actualArgNode->type));
#endif
            Value actualVal = eval(actualArgNode); // Evaluate the actual argument expression
#ifdef DEBUG
            fprintf(stderr, "[DEBUG EXEC_PROC] Arg %d evaluated to type %s\n", i, varTypeToString(actualVal.type));
#endif

            // User's code used makeCopyOfValue here - keep that intention
            arg_values[i] = makeCopyOfValue(&actualVal);
#ifdef DEBUG
            fprintf(stderr, "[DEBUG EXEC_PROC] Copied arg %d value (type %s)\n", i, varTypeToString(arg_values[i].type));
#endif

            // --- ADDED: Free the value returned by eval, as makeCopyOfValue made its own copy ---
            freeValue(&actualVal);
        }
    } // End Stage 1 Loop

    // --- Stage 2: Setup new scope and bind parameters (User's loop structure) ---
    SymbolEnvSnapshot snapshot;
    saveLocalEnv(&snapshot); // Save current environment state

    for (int i = num_params - 1; i >= 0; i--) { // Iterate backwards
        AST *paramNode = proc->proc_decl->children[i]; // Formal parameter definition (VAR_DECL)
        // Check structure (basic)
        if (!paramNode || paramNode->type != AST_VAR_DECL || paramNode->child_count < 1 || !paramNode->children[0] || !paramNode->children[0]->token) { /* error */ }

        char *paramName = paramNode->children[0]->token->value; // Formal name (e.g., "s")
        VarType ptype = paramNode->var_type;                 // Formal type (e.g., TYPE_RECORD)
        AST *type_def = paramNode->right;                    // Link to type definition (e.g., RECORD_TYPE node)

        if (paramNode->by_ref) { // Handle VAR parameter (User's logic)
#ifdef DEBUG
            fprintf(stderr, "[DEBUG EXEC_PROC] Binding VAR parameter '%s'\n", paramName);
#endif
            // Get the AST node passed as the argument
             AST* actualArgNode = node->children[i];
             if (!actualArgNode) { /* error */ }

            // Check if the argument is a valid variable reference
            if (actualArgNode->type != AST_VARIABLE && actualArgNode->type != AST_FIELD_ACCESS && actualArgNode->type != AST_ARRAY_ACCESS) { // Check added based on user code
                fprintf(stderr, "Runtime error: var parameter must be a variable reference, field, or array element.\n");
                EXIT_FAILURE_HANDLER();
            }

            // User's logic to find the symbol in the caller's scope
            // Assumes simple variable for now
             char *argVarName = NULL;
             if (actualArgNode->type == AST_VARIABLE && actualArgNode->token) {
                 argVarName = actualArgNode->token->value;
             } else {
                 // TODO: Extend lookup for fields/arrays based on argLValueNode
                  fprintf(stderr, "Warning: VAR parameter lookup not fully implemented for fields/arrays.\n");
                  // Use a placeholder or try to extract base name if possible
                  // This part needs findSymbolForLValue implementation from previous thoughts
                   argVarName = "?complex_lvalue?"; // Placeholder
             }

             if (!argVarName) { /* Error */ }

             Symbol *callerSym = lookupSymbolIn(snapshot.head, argVarName); // User's lookup function
             if (!callerSym) {
                 fprintf(stderr, "Runtime error: variable '%s' not declared (for var parameter '%s').\n", argVarName, paramName);
                 EXIT_FAILURE_HANDLER();
             }
             if (!callerSym->value) {
                  fprintf(stderr, "CRITICAL ERROR: Caller symbol '%s' for VAR parameter '%s' has NULL value pointer.\n", callerSym->name, paramName);
                  EXIT_FAILURE_HANDLER();
             }
             // Optional type check
             if (callerSym->type != ptype) {
                   fprintf(stderr, "Runtime error: Type mismatch for VAR parameter '%s'. Expected %s, got %s for variable '%s'.\n", paramName, varTypeToString(ptype), varTypeToString(callerSym->type), callerSym->name);
                   EXIT_FAILURE_HANDLER();
             }


            // Insert local symbol for the parameter
            insertLocalSymbol(paramName, ptype, type_def, false);
            Symbol *localSym = lookupLocalSymbol(paramName);
            if (!localSym || !localSym->value) { /* Error */ }

            // Free the default value created by insertLocalSymbol
            freeValue(localSym->value);
            free(localSym->value);

            // Alias: Make local symbol point to the caller's value
            localSym->value = callerSym->value;
            localSym->is_alias = true;
#ifdef DEBUG
            fprintf(stderr, "[DEBUG EXEC_PROC] Aliased VAR parameter '%s' to caller symbol '%s'\n", paramName, callerSym->name);
#endif

        } else { // Handle VALUE parameter (User's logic + freeValue fix)
#ifdef DEBUG
             fprintf(stderr, "[DEBUG EXEC_PROC] Inserting value parameter '%s' (type %s)\n", paramName, varTypeToString(ptype));
#endif
            // --- LINE 188 Context ---
            insertLocalSymbol(paramName, ptype, type_def, false); // Creates local symbol 's', initialized with default value
            Symbol *sym = lookupLocalSymbol(paramName);
            if (!sym) { /* Error */ }
            sym->is_alias = false; // It's a copy

            // Check if the copied value exists in arg_values
            if (arg_values[i].type == TYPE_VOID) { // Check if value wasn't prepared
                 fprintf(stderr, "CRITICAL ERROR: Value for parameter '%s' (index %d) was not evaluated/copied correctly.\n", paramName, i);
                 EXIT_FAILURE_HANDLER();
            }

            // Assign the deep-copied value from arg_values[i] to the local symbol
#ifdef DEBUG
            fprintf(stderr, "[DEBUG EXEC_PROC] Updating symbol '%s' with copied value (type %s from arg_values[%d])\n", paramName, varTypeToString(arg_values[i].type), i);
#endif
            updateSymbol(paramName, arg_values[i]); // updateSymbol makes the final copy

            // --- ADDED: Free the intermediate copy stored in arg_values[i] ---
            freeValue(&arg_values[i]);
            arg_values[i].type = TYPE_VOID; // Mark as freed/processed
        }
    } // End Stage 2 Loop

    // Free the temporary argument array structure (User's original placement)
    if (arg_values) {
        free(arg_values);
    }

    // --- Stage 3: Execute Body & Handle Return (User's original logic) ---
    Value retVal = makeVoid(); // Default for procedures
    if (proc->proc_decl->type == AST_FUNCTION_DECL) {
        AST *returnTypeDefNode = proc->proc_decl->right;
        if (!returnTypeDefNode) { /* Error */ }
        VarType retType = returnTypeDefNode->var_type;

        // Setup 'result' variable and function name alias
        insertLocalSymbol("result", retType, returnTypeDefNode, false);
        Symbol *resSym = lookupLocalSymbol("result");
        if (!resSym) { /* Error */ }
        resSym->is_alias = false;

        insertLocalSymbol(proc->name, retType, returnTypeDefNode, false);
        Symbol *funSym = lookupLocalSymbol(proc->name);
         if (!funSym) { /* Error */ }
        if(funSym->value) { freeValue(funSym->value); free(funSym->value); } // Free default
        funSym->value = resSym->value; // Alias to result's value
        funSym->is_alias = true;

        current_function_symbol = funSym; // Set global tracker

        // Execute function body
        if (!proc->proc_decl->extra) { /* Error: Missing function body */ }
        executeWithScope(proc->proc_decl->extra, false); // Execute BLOCK in 'extra'

        // Get result value AFTER execution
        Symbol* finalResultSym = lookupLocalSymbol("result"); // Look up 'result' again
        if (!finalResultSym || !finalResultSym->value) { /* Error retrieving result */ }
        else {
             retVal = makeCopyOfValue(finalResultSym->value); // Copy the final result
        }

        // Restore environment now
        restoreLocalEnv(&snapshot);
        current_function_symbol = NULL; // Clear tracker
        return retVal; // Return the copied result

    } else { // Procedure
        // Execute procedure body
        if (!proc->proc_decl->right) { /* Error: Missing procedure body */ }
        executeWithScope(proc->proc_decl->right, false); // Execute BLOCK in 'right'

        restoreLocalEnv(&snapshot); // Restore environment
        return makeVoid(); // Procedures return void
    }
} // End executeProcedureCall

// New function to process local declarations within a scope
void processLocalDeclarations(AST* declarationsNode) {
    if (!declarationsNode || declarationsNode->type != AST_COMPOUND) {
        // Should be an AST_COMPOUND node containing decls, or NULL/empty compound if none
        if (declarationsNode && declarationsNode->type != AST_NOOP) { // Ignore NOOP from empty decls section
             fprintf(stderr, "Warning: Expected COMPOUND node for local declarations, got %s\n",
                     declarationsNode ? astTypeToString(declarationsNode->type) : "NULL");
        }
        return; // Nothing to process
    }

    // Iterate through all declaration nodes (CONST_DECL, TYPE_DECL, VAR_DECL...)
    for (int i = 0; i < declarationsNode->child_count; i++) {
        AST *declNode = declarationsNode->children[i];
        if (!declNode) continue;

        switch (declNode->type) {
            case AST_CONST_DECL: {
                 const char *constName = declNode->token->value;
                 DEBUG_PRINT("[DEBUG_LOCALS] Processing local CONST_DECL: %s\n", constName);
                 Value constVal = eval(declNode->left);
                 Symbol *sym = insertLocalSymbol(constName, constVal.type, declNode->right, false);

                 if (sym && sym->value) {
                     freeValue(sym->value);
                     *sym->value = makeCopyOfValue(&constVal);
                     sym->is_const = true;
#ifdef DEBUG
                     fprintf(stderr, "[DEBUG_LOCALS] Set is_const=TRUE for local constant '%s'\n", constName);
#endif
                 } // ... error handling ...
                 freeValue(&constVal);
                 break;
             }
            case AST_VAR_DECL: {
                // Process local variable declarations
                AST *typeNode = declNode->right; // The type definition node
                for (int j = 0; j < declNode->child_count; j++) {
                    AST *varNode = declNode->children[j]; // The VARIABLE node
                    const char *varName = varNode->token->value;
                    DEBUG_PRINT("[DEBUG_LOCALS] Processing local VAR_DECL: %s\n", varName);

                    // Insert into local symbol table, mark as variable
                    Symbol *sym = insertLocalSymbol(varName, declNode->var_type, typeNode, true);

                    // Value initialization happens within insertLocalSymbol via makeValueForType
                    if (!sym || !sym->value) {
                        fprintf(stderr, "Error: Failed to insert or initialize local variable '%s'.\n", varName);
                        // Handle error
                    }
                     // Handle fixed-length string size if needed (might be redundant if insertLocalSymbol does it)
                     if (sym && sym->type == TYPE_STRING && typeNode && typeNode->right) {
                          // ... (code to evaluate length and set sym->value->max_length) ...
                          // This logic might be better placed entirely within insertLocalSymbol
                     }
                }
                break;
            }
            case AST_TYPE_DECL: {
                // Handling local TYPE declarations is complex.
                // Standard Pascal often doesn't allow types truly local *only*
                // to the procedure scope; they might be visible outwards.
                // For simplicity, we can IGNORE local type declarations for now.
                // Global types are already handled by the parser/linkUnit.
                DEBUG_PRINT("[DEBUG_LOCALS] Skipping local TYPE_DECL: %s\n", declNode->token->value);
                break;
            }
            case AST_PROCEDURE_DECL:
            case AST_FUNCTION_DECL: {
                // Handle nested procedures/functions if you implement them later
                DEBUG_PRINT("[DEBUG_LOCALS] Skipping nested PROCEDURE/FUNCTION: %s\n", declNode->token->value);
                break;
            }
            default:
                // Ignore other node types that might appear in the declarations compound node
                break;
        }
    }
}

char *enumValueToString(Type *enum_type, int value) {
    if (!enum_type) return strdup("<invalid>");
    if (value < 0 || value >= enum_type->member_count) return strdup("<out-of-range>");
    return strdup(enum_type->members[value]);
}

char *charToString(char c) {
    char *str = malloc(2);
    if (!str) {
        fprintf(stderr, "Memory allocation error.\n");
        EXIT_FAILURE_HANDLER();
    }
    str[0] = c;
    str[1] = '\0';
    return str;
}

Value eval(AST *node) {
    if (!node)
        return makeInt(0);

    if (node->type == AST_FORMATTED_EXPR) {
        // Evaluate the inner expression.
        Value val = eval(node->left);

        // Extract the formatting info stored in node->token->value.
        int width = 0, decimals = -1;
        sscanf(node->token->value, "%d,%d", &width, &decimals);

        char buf[DEFAULT_STRING_CAPACITY];

        if (val.type == TYPE_REAL) {
            if (decimals >= 0)
                snprintf(buf, sizeof(buf), "%*.*f", width, decimals, val.r_val);
            else
                snprintf(buf, sizeof(buf), "%*f", width, val.r_val);
        } else if (val.type == TYPE_INTEGER) {
            snprintf(buf, sizeof(buf), "%*lld", width, val.i_val);
        } else if (val.type == TYPE_STRING) {
            snprintf(buf, sizeof(buf), "%*s", width, val.s_val);
        } else {
            // For other types, simply use a default conversion.
            snprintf(buf, sizeof(buf), "%*s", width, "???");
        }

        return makeString(buf);
    }

    switch (node->type) {
        case AST_ARRAY_ACCESS: {
            Value arrVal = eval(node->left);

            if (arrVal.type == TYPE_ARRAY) {
                if (node->child_count != arrVal.dimensions) {
                    printf("===== Dumping Full AST Tree START ======\n");
                    dumpASTFromRoot(node);
                    printf("===== Dumping Full AST Tree END ======\n");
                    fprintf(stderr, "Runtime error: Expected %d index(es), got %d.\n",
                            arrVal.dimensions, node->child_count);
                    EXIT_FAILURE_HANDLER();
                }

                int *indices = malloc(sizeof(int) * arrVal.dimensions);
                if (!indices) {
                    fprintf(stderr, "Memory allocation error in array access.\n");
                    EXIT_FAILURE_HANDLER();
                }

                for (int i = 0; i < node->child_count; i++) {
                    if (!node->children[i]) {
                        fprintf(stderr, "Runtime error: NULL index expression in AST_ARRAY_ACCESS.\n");
                        dumpASTFromRoot(node);
                        EXIT_FAILURE_HANDLER();
                    }
                    
                    Value idxVal = eval(node->children[i]);
                    if (idxVal.type != TYPE_INTEGER) {
                        fprintf(stderr, "Runtime error: Array index must be an integer.\n");
                        free(indices);
                        EXIT_FAILURE_HANDLER();
                    }
                    indices[i] = (int)idxVal.i_val;
                }

                int offset = computeFlatOffset(&arrVal, indices);

                // Add explicit bounds check for offset
                int total_size = 1;
                for (int i = 0; i < arrVal.dimensions; i++) {
                    total_size *= (arrVal.upper_bounds[i] - arrVal.lower_bounds[i] + 1);
                }

                if (offset < 0 || offset >= total_size) {
                    fprintf(stderr, "Computed array offset %d is out of bounds (0..%d).\n", offset, total_size - 1);
                    free(indices);
                    EXIT_FAILURE_HANDLER();
                }

                Value result = arrVal.array_val[offset];

                free(indices);
                return result;
            }

            else if (arrVal.type == TYPE_STRING) {
                if (node->child_count < 1) {
                    fprintf(stderr, "Runtime error: String access missing index expression.\n");
                    EXIT_FAILURE_HANDLER();
                }

                Value indexVal = eval(node->children[0]);
                if (indexVal.type != TYPE_INTEGER) {
                    fprintf(stderr, "Runtime error: String index must be an integer.\n");
                    EXIT_FAILURE_HANDLER();
                }

                long long idx = indexVal.i_val;
                if (!arrVal.s_val) {
                    fprintf(stderr, "Runtime error: Null string access.\n");
                    EXIT_FAILURE_HANDLER();
                }

                int len = (int)strlen(arrVal.s_val);
                if (idx < 1 || idx > len) {
                    printf("===== AST Dump START =====\n");
                    dumpASTFromRoot(node);
                    printf("===== AST Dump END =====\n");
                    printf("===== Symbol Table(s) Dump START =====\n");
                    dumpSymbolTable();
                    printf("===== Symbol Table(s) Dump END =====\n");
                    fprintf(stderr, "Runtime error: String index %lld out of bounds [1..%d].\n", idx, len);
                    EXIT_FAILURE_HANDLER();
                }

                char selected = arrVal.s_val[idx - 1];
                return makeChar(selected);
            }

            else if (arrVal.type == TYPE_CHAR) {
                // Optional: allow 'char[1]' style access, e.g., 'c[1]' => c
                if (node->child_count != 1) {
                    fprintf(stderr, "Runtime error: CHAR type only supports single index access.\n");
                    EXIT_FAILURE_HANDLER();
                }

                Value indexVal = eval(node->children[0]);
                if (indexVal.type != TYPE_INTEGER || indexVal.i_val != 1) {
                    fprintf(stderr, "Runtime error: CHAR index must be exactly 1.\n");
                    EXIT_FAILURE_HANDLER();
                }

                return arrVal; // Already a char, return as-is
            }

            else {
                fprintf(stderr, "Runtime error: Attempted array access on non-array variable.\n");
                EXIT_FAILURE_HANDLER();
            }
        }
        case AST_ARRAY_LITERAL: {
#ifdef DEBUG
            fprintf(stderr, "[DEBUG] Evaluating AST_ARRAY_LITERAL\n");
#endif
            // The type definition should be linked via node->right,
            // which was set in parseArrayInitializer referencing the
            // typeNode parsed in constDeclaration.
            AST* typeNode = node->right;
            if (!typeNode) {
                fprintf(stderr, "Runtime error: Missing type definition for array literal.\n");
                dumpASTFromRoot(node); // Dump AST for context
                EXIT_FAILURE_HANDLER();
            }

            // Resolve type reference if needed
            AST* actualArrayTypeNode = typeNode;
            if (actualArrayTypeNode->type == AST_TYPE_REFERENCE) {
                 actualArrayTypeNode = lookupType(actualArrayTypeNode->token->value);
                 if (!actualArrayTypeNode) {
                      fprintf(stderr, "Runtime error: Could not resolve array type reference '%s' for literal.\n", typeNode->token->value);
                       EXIT_FAILURE_HANDLER();
                 }
            }

            if (!actualArrayTypeNode || actualArrayTypeNode->type != AST_ARRAY_TYPE) {
                 fprintf(stderr, "Runtime error: Invalid type node associated with array literal. Expected ARRAY_TYPE, got %s.\n",
                         actualArrayTypeNode ? astTypeToString(actualArrayTypeNode->type) : "NULL");
                 dumpASTFromRoot(node); // Dump AST for context
                 EXIT_FAILURE_HANDLER();
            }

            // --- Extract bounds and element type from the actualArrayTypeNode ---
            int dimensions = actualArrayTypeNode->child_count;
            if (dimensions <= 0) { /* error handling */ }

            int *lower_bounds = malloc(sizeof(int) * dimensions);
            int *upper_bounds = malloc(sizeof(int) * dimensions);
            if (!lower_bounds || !upper_bounds) { /* error handling */ }

            int expected_size = 1;
            for (int dim = 0; dim < dimensions; dim++) {
                 AST *subrange = actualArrayTypeNode->children[dim];
                 if (!subrange || subrange->type != AST_SUBRANGE) { /* error handling */ }

                 // --- Evaluate bounds and get ordinal values ---
                 Value low_val = eval(subrange->left);
                 Value high_val = eval(subrange->right);
                 long long low_ord = 0, high_ord = 0; // <<< Initialize to 0

                 // Check type of lower bound
                 if (low_val.type == TYPE_INTEGER) low_ord = low_val.i_val;
                 else if (low_val.type == TYPE_ENUM) low_ord = low_val.enum_val.ordinal;
                 else if (low_val.type == TYPE_CHAR) low_ord = low_val.c_val;
                 // Add other ordinal types if needed (Boolean, Byte, Word)
                 else {
                     fprintf(stderr, "Runtime error: Invalid type (%s) for lower bound of array constant.\n", varTypeToString(low_val.type));
                     free(lower_bounds); free(upper_bounds); EXIT_FAILURE_HANDLER();
                 }

                 // Check type of upper bound
                 if (high_val.type == TYPE_INTEGER) high_ord = high_val.i_val;
                 else if (high_val.type == TYPE_ENUM) high_ord = high_val.enum_val.ordinal;
                 else if (high_val.type == TYPE_CHAR) high_ord = high_val.c_val;
                 // Add other ordinal types if needed
                 else {
                      fprintf(stderr, "Runtime error: Invalid type (%s) for upper bound of array constant.\n", varTypeToString(high_val.type));
                      free(lower_bounds); free(upper_bounds); EXIT_FAILURE_HANDLER();
                 }
                 // --- End bound evaluation ---

                 lower_bounds[dim] = (int)low_ord; // <<< Use calculated ordinal
                 upper_bounds[dim] = (int)high_ord; // <<< Use calculated ordinal

                 if (lower_bounds[dim] > upper_bounds[dim]) { /* error handling */ }
                 expected_size *= (upper_bounds[dim] - lower_bounds[dim] + 1);
            }

            // Determine element type (existing logic should be okay)
            AST* elemTypeNode = actualArrayTypeNode->right;
            // ... (rest of element type determination logic) ...
            VarType elementType = elemTypeNode->var_type;

            // --- Check number of initializers --- (existing logic)
            int provided_size = node->child_count;
            if (provided_size != expected_size) { // <<< This check should now work correctly
                fprintf(stderr, "Runtime error: Incorrect number of initializers for constant array. Expected %d, got %d.\n", expected_size, provided_size);
                free(lower_bounds); free(upper_bounds);
                EXIT_FAILURE_HANDLER();
            }

            // --- Create the Array Value --- (existing logic)
            Value v = makeArrayND(dimensions, lower_bounds, upper_bounds, elementType, elemTypeNode);

            // --- Fill the array --- (existing logic, ensure type check handles enums if needed)
            for (int i = 0; i < provided_size; i++) {
                Value elemVal = eval(node->children[i]);
                // Adjust type check if necessary, e.g., allow assigning integer const to byte array
                if (elemVal.type != elementType) {
                     if (!(((elementType == TYPE_BYTE) || (elementType == TYPE_WORD)) && elemVal.type == TYPE_INTEGER)) {
                        // Handle case where element type is enum and initializer is identifier like 'Red'
                        if(!(elementType == TYPE_ENUM && elemVal.type == TYPE_ENUM)) {
                           fprintf(stderr, "Runtime error: Type mismatch in constant array initializer element %d. Expected %s, got %s.\n",
                                   i + 1, varTypeToString(elementType), varTypeToString(elemVal.type));
                           // Cleanup needed before exit
                           freeValue(&v);
                           free(lower_bounds); free(upper_bounds);
                           EXIT_FAILURE_HANDLER();
                        }
                     }
                }
                v.array_val[i] = makeCopyOfValue(&elemVal);
                freeValue(&elemVal); // Free temporary value from eval
            }

            free(lower_bounds);
            free(upper_bounds);
            return v;
        } // End case AST_ARRAY_LITERAL
        case AST_BOOLEAN:
            return makeBoolean((node->token->type == TOKEN_TRUE) ? 1 : 0);
        case AST_NUMBER:
             if (node->token->type == TOKEN_INTEGER_CONST) {
                 return makeInt(atoi(node->token->value));
             } else if (node->token->type == TOKEN_HEX_CONST) {
                 return makeInt(atoi(node->token->value));
             } else if (node->token->type == TOKEN_REAL_CONST) {
                 return makeReal(strtod(node->token->value, NULL));
             }
             break;
        case AST_STRING:
#ifdef DEBUG
    DEBUG_PRINT("[DEBUG] eval AST_STRING: token value='%s'\n", node->token->value);
#endif
            return makeString(node->token->value);
        case AST_VARIABLE: {
            Symbol *sym = lookupSymbol(node->token->value);
            if (!sym || !sym->value) {
                fprintf(stderr, "Runtime error: variable '%s' not declared or uninitialized.\n", node->token->value);
                dumpASTFromRoot(node);
                dumpSymbolTable();
                EXIT_FAILURE_HANDLER();
            }

            Value val = makeCopyOfValue(sym->value);  // Copy for safety
            if (val.type == TYPE_STRING && val.s_val == NULL)
                val.s_val = strdup("");

            setTypeAST(node, val.type);
            return val;
        }
        case AST_FIELD_ACCESS: {
            Value recVal = eval(node->left);
            if (recVal.type != TYPE_RECORD) {
                fprintf(stderr, "Runtime error: field access on non-record type.\n");
                EXIT_FAILURE_HANDLER();
            }
            FieldValue *fv = recVal.record_val;
            const char *targetField = node->token->value;

            while (fv) {
                if (strcmp(fv->name, targetField) == 0) {
                    return fv->value;
                }
                fv = fv->next;
            }

            fprintf(stderr, "Runtime error: field '%s' not found.\n", targetField);
            EXIT_FAILURE_HANDLER();
        }
        case AST_BINARY_OP: {
                    Value left = eval(node->left);
                    Value right = eval(node->right);
                    TokenType op = node->token->type;

                    // --- Handle IN operator FIRST (Set membership) ---
                    if (op == TOKEN_IN) {
                        if (right.type != TYPE_SET) {
                            fprintf(stderr, "Runtime error: Right operand of 'in' must be a set.\n");
                            EXIT_FAILURE_HANDLER();
                        }
                        bool left_is_ordinal_compatible = (left.type == TYPE_INTEGER ||
                                                           left.type == TYPE_ENUM ||
                                                           left.type == TYPE_CHAR ||
                                                           (left.type == TYPE_STRING && left.s_val && strlen(left.s_val) == 1));
                        if (!left_is_ordinal_compatible) {
                            fprintf(stderr, "Runtime error: Left operand of 'in' must be an ordinal type (integer, char, enum). Got %s\n", varTypeToString(left.type));
                            EXIT_FAILURE_HANDLER();
                        }
                        long long left_ord;
                        if (left.type == TYPE_INTEGER) left_ord = left.i_val;
                        else if (left.type == TYPE_ENUM) left_ord = left.enum_val.ordinal;
                        else if (left.type == TYPE_CHAR) left_ord = left.c_val;
                        else left_ord = left.s_val[0]; // Single-char string case

                        int found = 0;
                        if (right.set_val.set_values != NULL) {
                            for (int i = 0; i < right.set_val.set_size; i++) {
                                if (right.set_val.set_values[i] == left_ord) {
                                    found = 1;
                                    break;
                                }
                            }
                        }
                        return makeBoolean(found); // Use makeBoolean consistently
                    } // --- End IN operator ---

                    // --- Handle specific promotions for comparisons ---
                    // Note: This promotion happens *before* the main type-specific logic blocks below.
                    // It simplifies comparisons by trying to bring operands to a common comparable type (Integer).
                    if (op == TOKEN_EQUAL || op == TOKEN_NOT_EQUAL || op == TOKEN_LESS ||
                        op == TOKEN_LESS_EQUAL || op == TOKEN_GREATER || op == TOKEN_GREATER_EQUAL)
                    {
                        // Promote CHAR vs INTEGER or CHAR vs CHAR to INTEGER for comparison
                        if (left.type == TYPE_CHAR && right.type == TYPE_INTEGER) {
                            left = makeInt((long long)left.c_val);
                        } else if (left.type == TYPE_INTEGER && right.type == TYPE_CHAR) {
                            right = makeInt((long long)right.c_val);
                        } else if (left.type == TYPE_CHAR && right.type == TYPE_CHAR) {
                            left = makeInt((long long)left.c_val);
                            right = makeInt((long long)right.c_val);
                        }
                        // Promote ENUM vs INTEGER or ENUM vs ENUM (same type) to INTEGER for comparison
                         else if (left.type == TYPE_ENUM && right.type == TYPE_INTEGER) {
                             left = makeInt((long long)left.enum_val.ordinal);
                         } else if (left.type == TYPE_INTEGER && right.type == TYPE_ENUM) {
                             right = makeInt((long long)right.enum_val.ordinal);
                         }
                         // <<< This is where the previous ENUM vs ENUM comparison was,
                         //     but we need the original types later, so we remove this specific promotion block
                         //     and handle enum vs enum comparison explicitly below. >>>
                    }

                    // Promote non-comparison ENUM operands to INTEGER if not interacting with REAL/STRING/BOOLEAN
                     // (Be careful with this - might hide errors if types are truly incompatible)
                     // Let's comment this out for now and rely on explicit type handling below.
                    /*
                    if (left.type != TYPE_REAL && right.type != TYPE_REAL && left.type != TYPE_STRING && right.type != TYPE_STRING && left.type != TYPE_BOOLEAN && right.type != TYPE_BOOLEAN && op != TOKEN_SLASH) {
                        if (left.type == TYPE_ENUM) left = makeInt(left.enum_val.ordinal);
                        if (right.type == TYPE_ENUM) right = makeInt(right.enum_val.ordinal);
                    }
                    */

                    // --- Short-circuit boolean LOGICAL ops (AND, OR) ---
                    if (op == TOKEN_AND || op == TOKEN_OR) { // Added TOKEN_XOR potentially later
                        // Expect boolean or integer operands for logical ops
                        bool left_bool = (left.type == TYPE_BOOLEAN || left.type == TYPE_INTEGER);
                        bool right_bool = (right.type == TYPE_BOOLEAN || right.type == TYPE_INTEGER);
                        if (!left_bool || !right_bool) {
                             fprintf(stderr, "Runtime error: Operands for %s must be boolean or integer. Left: %s, Right: %s\n",
                                     tokenTypeToString(op), varTypeToString(left.type), varTypeToString(right.type));
                             EXIT_FAILURE_HANDLER();
                        }
                        long long left_ival = (left.type == TYPE_BOOLEAN) ? left.i_val : left.i_val; // Assume integer uses i_val
                        long long right_ival = (right.type == TYPE_BOOLEAN) ? right.i_val : right.i_val;

                        if (op == TOKEN_AND) return makeBoolean((left_ival != 0) && (right_ival != 0));
                        if (op == TOKEN_OR) return makeBoolean((left_ival != 0) || (right_ival != 0));
                        // Add XOR if implemented: return makeBoolean((left_ival != 0) ^ (right_ival != 0));
                    } // --- End logical ops ---


                    // --- Start of type-specific handling ---

                    // 1. INTEGER math & comparison (if both operands are currently INTEGER type after potential promotion)
                    if (left.type == TYPE_INTEGER && right.type == TYPE_INTEGER && op != TOKEN_SLASH) {
                        long long a = left.i_val, b = right.i_val;
                        switch (op) {
                            // Arithmetic (result is Integer)
                            case TOKEN_PLUS:   return makeInt(a + b);
                            case TOKEN_MINUS:  return makeInt(a - b);
                            case TOKEN_MUL:    return makeInt(a * b);
                            case TOKEN_INT_DIV:
                                if (b == 0) { fprintf(stderr,"Runtime error: Division by zero.\n"); EXIT_FAILURE_HANDLER(); }
                                return makeInt(a / b); // Integer division
                            case TOKEN_MOD:
                                if (b == 0) { fprintf(stderr,"Runtime error: Modulo by zero.\n"); EXIT_FAILURE_HANDLER(); }
                                return makeInt(a % b);
                            // Comparisons (result is Boolean)
                            case TOKEN_GREATER:       return makeBoolean(a > b);
                            case TOKEN_GREATER_EQUAL: return makeBoolean(a >= b);
                            case TOKEN_EQUAL:         return makeBoolean(a == b);
                            case TOKEN_NOT_EQUAL:     return makeBoolean(a != b);
                            case TOKEN_LESS:          return makeBoolean(a < b);
                            case TOKEN_LESS_EQUAL:    return makeBoolean(a <= b);
                            // Operators invalid for Integer after potential promotions
                            case TOKEN_SLASH: // Should have been promoted to REAL if '/' was used
                            case TOKEN_DOTDOT: // Not a binary operator in expressions
                            case TOKEN_AND: case TOKEN_OR: // Handled above
                                fprintf(stderr, "Internal error: Operator %s should not operate on two integers here.\n", tokenTypeToString(op));
                                EXIT_FAILURE_HANDLER();
                            default:
                                fprintf(stderr, "Runtime error: Unhandled operator '%s' for INTEGER operands.\n", tokenTypeToString(op));
                                EXIT_FAILURE_HANDLER();
                        }
                    }

                    // 2. STRING concatenation or comparison
                    else if (left.type == TYPE_STRING || right.type == TYPE_STRING || left.type == TYPE_CHAR || right.type == TYPE_CHAR) {
                         // Promote Char to String for operations
                         char temp_left[2] = {0}, temp_right[2] = {0}; // Buffers for char-to-string
                         const char *left_s = left.s_val, *right_s = right.s_val;

                         if (left.type == TYPE_CHAR) {
                             temp_left[0] = left.c_val; left_s = temp_left;
                         } else if (left.type != TYPE_STRING) {
                              fprintf(stderr, "Runtime error: Incompatible left operand type %s for string operation with op %s.\n", varTypeToString(left.type), tokenTypeToString(op));
                              EXIT_FAILURE_HANDLER();
                         }

                         if (right.type == TYPE_CHAR) {
                             temp_right[0] = right.c_val; right_s = temp_right;
                         } else if (right.type != TYPE_STRING) {
                               fprintf(stderr, "Runtime error: Incompatible right operand type %s for string operation with op %s.\n", varTypeToString(right.type), tokenTypeToString(op));
                               EXIT_FAILURE_HANDLER();
                         }

                         // Handle NULL strings safely
                         if (!left_s) left_s = "";
                         if (!right_s) right_s = "";

                         if (op == TOKEN_PLUS) { // Concatenation
                             int len_left = (int)strlen(left_s);
                             int len_right = (int)strlen(right_s);
                             // Allocate sufficient buffer size (+1 for null terminator)
                             size_t buffer_size = len_left + len_right + 1;
                             char *result = malloc(buffer_size);
                             if (!result) { /* Mem alloc error */ EXIT_FAILURE_HANDLER(); }

                             // Copy the left part first
                             strcpy(result, left_s); // strcpy null-terminates
                             strncat(result, right_s, len_right);

                             Value out = makeString(result); // makeString copies 'result'
                             free(result);                  // Free the temporary buffer
                             return out;
                         }
                         else if (op == TOKEN_EQUAL || op == TOKEN_NOT_EQUAL || op == TOKEN_LESS ||
                                  op == TOKEN_LESS_EQUAL || op == TOKEN_GREATER || op == TOKEN_GREATER_EQUAL)
                         { // Comparison
                             int cmp = strcmp(left_s, right_s);
                             switch (op) {
                                 case TOKEN_EQUAL:         return makeBoolean(cmp == 0);
                                 case TOKEN_NOT_EQUAL:     return makeBoolean(cmp != 0);
                                 case TOKEN_LESS:          return makeBoolean(cmp < 0);
                                 case TOKEN_LESS_EQUAL:    return makeBoolean(cmp <= 0);
                                 case TOKEN_GREATER:       return makeBoolean(cmp > 0);
                                 case TOKEN_GREATER_EQUAL: return makeBoolean(cmp >= 0);
                                 default: break; // Should not happen
                             }
                         } else {
                              fprintf(stderr, "Runtime error: Operator %s not supported for STRING/CHAR operands.\n", tokenTypeToString(op));
                              EXIT_FAILURE_HANDLER();
                         }
                    }

                    // 3. ENUM comparison (This should be the primary comparison logic now)
                    else if (left.type == TYPE_ENUM && right.type == TYPE_ENUM) {
                         const char* left_name = left.enum_val.enum_name;
                         const char* right_name = right.enum_val.enum_name;
                         bool types_match = (!left_name || !right_name || strcmp(left_name, right_name) == 0);

                         if (!types_match && (op != TOKEN_EQUAL && op != TOKEN_NOT_EQUAL)) {
                              // Error on ordered comparison between different enum types
                              fprintf(stderr, "Runtime error: Cannot perform ordered comparison on different ENUM types ('%s' vs '%s').\n", left_name ? left_name : "?", right_name ? right_name : "?");
                              EXIT_FAILURE_HANDLER();
                         }

                         switch (op) {
                             case TOKEN_EQUAL:
                                 // Types must match (or names missing) AND ordinals must match
                                 return makeBoolean(types_match && (left.enum_val.ordinal == right.enum_val.ordinal));
                             case TOKEN_NOT_EQUAL:
                                 // Types differ OR ordinals differ
                                 return makeBoolean(!types_match || (left.enum_val.ordinal != right.enum_val.ordinal));
                             case TOKEN_LESS:
                                 return makeBoolean(types_match && (left.enum_val.ordinal < right.enum_val.ordinal));
                             case TOKEN_LESS_EQUAL:
                                 return makeBoolean(types_match && (left.enum_val.ordinal <= right.enum_val.ordinal));
                             case TOKEN_GREATER:
                                 return makeBoolean(types_match && (left.enum_val.ordinal > right.enum_val.ordinal));
                             case TOKEN_GREATER_EQUAL:
                                 return makeBoolean(types_match && (left.enum_val.ordinal >= right.enum_val.ordinal));
                             default:
                                 fprintf(stderr, "Runtime error: Unsupported operator %s for ENUM types.\n", tokenTypeToString(op));
                                 EXIT_FAILURE_HANDLER();
                         }
                    }

                    // 4. BOOLEAN comparison (=, <>)
                    else if (left.type == TYPE_BOOLEAN && right.type == TYPE_BOOLEAN &&
                             (op == TOKEN_EQUAL || op == TOKEN_NOT_EQUAL))
                    {
                         switch (op) {
                             case TOKEN_EQUAL:     return makeBoolean(left.i_val == right.i_val);
                             case TOKEN_NOT_EQUAL: return makeBoolean(left.i_val != right.i_val);
                             default: break; // Should not happen
                         }
                    }

                    // 5. REAL / Mixed Numeric / '/' operations (Final numeric catch-all)
                    // This block handles Real-Real, Real-Int, Int-Real, and Int-Int with '/'
                    else if ((left.type == TYPE_REAL || left.type == TYPE_INTEGER) &&
                             (right.type == TYPE_REAL || right.type == TYPE_INTEGER))
                    {
                         // Promote Integer to double for calculation
                         double a = (left.type == TYPE_REAL) ? left.r_val : (double)left.i_val;
                         double b = (right.type == TYPE_REAL) ? right.r_val : (double)right.i_val;

                         switch (op) {
                             // Arithmetic ops -> return REAL
                             case TOKEN_PLUS:           return makeReal(a + b);
                             case TOKEN_MINUS:          return makeReal(a - b);
                             case TOKEN_MUL:            return makeReal(a * b);
                             case TOKEN_SLASH: // Division always results in REAL
                                 if (b == 0.0) { fprintf(stderr,"Runtime error: Division by zero.\n"); EXIT_FAILURE_HANDLER(); }
                                 return makeReal(a / b);

                             // Comparison ops -> return BOOLEAN
                             case TOKEN_GREATER:        return makeBoolean(a > b);
                             case TOKEN_GREATER_EQUAL:  return makeBoolean(a >= b);
                             case TOKEN_EQUAL:          return makeBoolean(a == b); // Note: floating point equality issues possible
                             case TOKEN_NOT_EQUAL:      return makeBoolean(a != b);
                             case TOKEN_LESS:           return makeBoolean(a < b);
                             case TOKEN_LESS_EQUAL:     return makeBoolean(a <= b);

                             // Operators invalid for REALs
                             case TOKEN_INT_DIV:
                             case TOKEN_MOD:
                             case TOKEN_DOTDOT:
                             case TOKEN_AND:
                             case TOKEN_OR:
                                  fprintf(stderr, "Runtime error: Operator %s not valid for REAL/Mixed operands.\n", tokenTypeToString(op));
                                  EXIT_FAILURE_HANDLER();
                             default:
                                  fprintf(stderr, "Runtime error: Unhandled numeric operator '%s' in REAL/Mixed block.\n", tokenTypeToString(op));
                                  EXIT_FAILURE_HANDLER();
                         }
                    }

                    // --- Fallback / Error ---
                    // If none of the above specific type combinations matched
                    fprintf(stderr, "Internal error or unsupported operand types: Binary operation fell through. Op: %s, Left: %s, Right: %s\n",
                            tokenTypeToString(op), varTypeToString(left.type), varTypeToString(right.type));
                    EXIT_FAILURE_HANDLER();

                } // End case AST_BINARY_OP // End case AST_BINARY_OP
        case AST_SET: // Add case to handle set evaluation
            return evalSet(node);
        case AST_UNARY_OP: {
            Value val = eval(node->left);
            if (node->token->type == TOKEN_PLUS)
                return val;
            else if (node->token->type == TOKEN_MINUS) {
                if (val.type == TYPE_INTEGER)
                    return makeInt(-val.i_val);
                else
                    return makeReal(-val.r_val);
            } else if (node->token->type == TOKEN_NOT)
                return makeBoolean(val.i_val == 0);
            break;
        }
        case AST_PROCEDURE_CALL:
            return executeProcedureCall(node);
        case AST_ENUM_VALUE: {
            Value v;
            setTypeValue(&v, TYPE_ENUM); // Set type first

            // --- MODIFICATION START ---
            // Retrieve the actual type name, not the value name.
            // Assume the parser links the enum value node (node)
            // to its type definition node via node->right.

            AST* type_def_node = node->right; // Get potential link to type def
            const char* type_name_str = NULL;

            // Handle potential type references
            if (type_def_node && type_def_node->type == AST_TYPE_REFERENCE) {
                type_def_node = type_def_node->right; // Follow the reference link
            }

            // Get the type name from the definition node's token
            if (type_def_node && type_def_node->token && type_def_node->token->value) {
                type_name_str = type_def_node->token->value;
            } else {
                // Fallback if type info isn't properly linked (Parser issue?)
                // This might indicate a problem earlier in parsing.
                fprintf(stderr, "Warning: Could not determine type name for enum value '%s' during eval. AST node dump follows:\n", node->token->value);
                // dumpAST(node, 0); // Optional: Add more debug info if needed
                type_name_str = "<unknown_enum>"; // Assign a placeholder
            }

            // Allocate and assign the *type* name
            v.enum_val.enum_name = strdup(type_name_str);
            if (!v.enum_val.enum_name) {
                fprintf(stderr, "Memory allocation failed for enum name in eval.\n");
                EXIT_FAILURE_HANDLER();
            }
            // --- MODIFICATION END ---

            // Ordinal value is correctly set by the parser in i_val
            v.enum_val.ordinal = node->i_val;
            return v;
        } // End case AST_ENUM_VALUE
        default:
            break;
    }
    return makeInt(0);
}

bool valueMatchesLabel(Value caseVal, AST *label) {
    if (!label) return false; // Safety check

    if (label->type == AST_SUBRANGE) {
        // Ensure case value and range bounds are compatible ordinals (e.g., integer or char)
        Value low = eval(label->left);
        Value high = eval(label->right);

        // Allow Integer range
        if (caseVal.type == TYPE_INTEGER && low.type == TYPE_INTEGER && high.type == TYPE_INTEGER) {
            return caseVal.i_val >= low.i_val && caseVal.i_val <= high.i_val;
        }
        // Allow Char range (compare char values directly)
        else if (caseVal.type == TYPE_CHAR && low.type == TYPE_CHAR && high.type == TYPE_CHAR) {
             return caseVal.c_val >= low.c_val && caseVal.c_val <= high.c_val;
        }
        // Add other ordinal range checks if needed (e.g., Byte, Word)

        // Incompatible types for range check
        return false;

    } else { // Single label comparison
        Value labelVal = eval(label); // Evaluate the label (e.g., dirRight, 'A', 5)

        // Compare based on the type of the case expression value
        switch (caseVal.type) {
            case TYPE_ENUM:
                // Case expression is ENUM, label must also be ENUM
                if (labelVal.type == TYPE_ENUM) {
                    // Optional: Add stricter check using enum_name if desired:
                    // if (caseVal.enum_val.enum_name && labelVal.enum_val.enum_name &&
                    //     strcmp(caseVal.enum_val.enum_name, labelVal.enum_val.enum_name) == 0)
                    return caseVal.enum_val.ordinal == labelVal.enum_val.ordinal;
                    // else return false; // Mismatched enum types
                }
                break; // Enum vs non-enum fails

            case TYPE_INTEGER:
            case TYPE_BYTE:  // Treat Byte/Word as integers for case
            case TYPE_WORD:
                // Case expression is Integer-like, label must be Integer-like
                if (labelVal.type == TYPE_INTEGER || labelVal.type == TYPE_BYTE || labelVal.type == TYPE_WORD) {
                    return caseVal.i_val == labelVal.i_val;
                }
                 // Allow matching Integer Case value against a Char label (using Ord(char))
                 else if (labelVal.type == TYPE_CHAR) {
                     return caseVal.i_val == (long long)labelVal.c_val;
                 }
                break; // Integer vs other types fails
            case TYPE_CHAR:
                // Case expression is CHAR
                 if (labelVal.type == TYPE_CHAR) {
                     // Label is CHAR - Direct comparison
                     return caseVal.c_val == labelVal.c_val;
                 }
                 else if (labelVal.type == TYPE_STRING && labelVal.s_val && strlen(labelVal.s_val) == 1) {
                     // <<< FIX: Label is single-char STRING - Compare case char to string's first char
                     return caseVal.c_val == labelVal.s_val[0];
                 }
                 else if (labelVal.type == TYPE_INTEGER) {
                      // Allow matching Char Case value against an Integer label (Ordinal match)
                      return (long long)caseVal.c_val == labelVal.i_val;
                 }
                 break; // Char vs other types fails
             case TYPE_BOOLEAN:
                 // Case expression is BOOLEAN, label must be BOOLEAN
                 if (labelVal.type == TYPE_BOOLEAN) {
                     return caseVal.i_val == labelVal.i_val; // Booleans use i_val (0/1)
                 }
                 break; // Boolean vs non-boolean fails

            // Add other ordinal types if needed

            default:
                // Non-ordinal type used in case expression - error or return false
                // fprintf(stderr, "Warning: Non-ordinal type %s used in CASE expression.\n", varTypeToString(caseVal.type));
                return false;
        }

        // If we fall through, types were incompatible
        return false;
    }
}

static void processDeclarations(AST *decl, bool is_global_block) {
    for (int i = 0; i < decl->child_count; i++) {
        AST *d = decl->children[i];
        if (d->type != AST_VAR_DECL) continue;

        for (int j = 0; j < d->child_count; j++) {
            AST *varNode = d->children[j];
            const char *varname = varNode->token->value;

            if (is_global_block) {
                insertGlobalSymbol(varname, d->var_type, d->right);
                DEBUG_PRINT("[DEBUG] insertGlobalSymbol('%s', type=%s)\n", varname, varTypeToString(d->var_type));
            } else {
                insertLocalSymbol(varname, d->var_type, d->right, true);
                DEBUG_PRINT("[DEBUG] insertLocalSymbol('%s', type=%s)\n", varname, varTypeToString(d->var_type));
            }

            Symbol *sym = lookupSymbol(varname);
            if (!sym) {
                fprintf(stderr, "Internal error: Symbol '%s' not found after insertion.\n", varname);
                EXIT_FAILURE_HANDLER();
            }

            if (sym->value == NULL) {
                sym->value = malloc(sizeof(Value));
                if (!sym->value) {
                    fprintf(stderr, "Memory allocation failed for symbol value\n");
                    EXIT_FAILURE_HANDLER();
                }
                *sym->value = makeValueForType(sym->type, d->right);
            }

            if (d->var_type == TYPE_RECORD && d->right) {
                *sym->value = makeRecord(createEmptyRecord(d->right));
            }

            else if (d->var_type == TYPE_ARRAY && d->right) {
                AST *typeDefinitionNode = d->right; // This could be ARRAY_TYPE or TYPE_REFERENCE
                AST *actualArrayTypeNode = NULL;    // <<< DECLARE THE VARIABLE HERE

                // Resolve if it's a reference
                if (typeDefinitionNode->type == AST_TYPE_REFERENCE) {
                    // Assuming the parser correctly links the reference to the actual definition
                    // via the 'right' pointer of the AST_TYPE_REFERENCE node.
                    actualArrayTypeNode = typeDefinitionNode->right;
                } else if (typeDefinitionNode->type == AST_ARRAY_TYPE) {
                    // It's an inline definition
                    actualArrayTypeNode = typeDefinitionNode;
                } else {
                    // Error: Unexpected node type
                    fprintf(stderr, "Internal error: Unexpected node type (%s) for array variable '%s'. Expected ARRAY_TYPE or TYPE_REFERENCE.\n",
                                     astTypeToString(typeDefinitionNode->type),
                                     d->children[0]->token->value);
                    dumpASTFromRoot(d);
                    EXIT_FAILURE_HANDLER();
                }

                // Check if we successfully found/resolved the AST_ARRAY_TYPE node
                if (!actualArrayTypeNode || actualArrayTypeNode->type != AST_ARRAY_TYPE) {
                    fprintf(stderr, "Internal error: Failed to find or resolve AST_ARRAY_TYPE node for '%s'. Found %s instead.\n",
                                     d->children[0]->token->value,
                                     actualArrayTypeNode ? astTypeToString(actualArrayTypeNode->type) : "NULL");
                    dumpASTFromRoot(d); // Dump AST for context
                    EXIT_FAILURE_HANDLER();
                }


                // Get dimensions and bounds from the children of the AST_ARRAY_TYPE node
                int dimensions = actualArrayTypeNode->child_count;
                if (dimensions <= 0) {
                     fprintf(stderr, "Runtime error: Array declaration has no dimensions for '%s'.\n", d->children[0]->token->value);
                     EXIT_FAILURE_HANDLER();
                }
                int *lower_bounds = malloc(sizeof(int) * dimensions);
                int *upper_bounds = malloc(sizeof(int) * dimensions);
                 if (!lower_bounds || !upper_bounds) {
                     fprintf(stderr, "Memory allocation failed for array bounds\n");
                     EXIT_FAILURE_HANDLER();
                 }

                for (int dim = 0; dim < dimensions; dim++) {
                    AST *subrange = actualArrayTypeNode->children[dim];
                    if (!subrange || subrange->type != AST_SUBRANGE) {
                         fprintf(stderr, "Internal error: Expected AST_SUBRANGE in array type for '%s'.\n", d->children[0]->token->value);
                         free(lower_bounds);
                         free(upper_bounds);
                         EXIT_FAILURE_HANDLER();
                    }
                    lower_bounds[dim] = (int)eval(subrange->left).i_val;
                    upper_bounds[dim] = (int)eval(subrange->right).i_val;
                     if (lower_bounds[dim] > upper_bounds[dim]) {
                         fprintf(stderr, "Runtime error: Array lower bound (%d) > upper bound (%d) for dimension %d of '%s'.\n",
                                 lower_bounds[dim], upper_bounds[dim], dim + 1, d->children[0]->token->value);
                         free(lower_bounds);
                         free(upper_bounds);
                         EXIT_FAILURE_HANDLER();
                     }
                }

                // Get the element type node from the RIGHT pointer of the AST_ARRAY_TYPE node
                VarType elemType = TYPE_VOID;
                AST *elemTypeNode = actualArrayTypeNode->right; // Correct node for element type

                if (!elemTypeNode) { // Check if element type node exists
                    fprintf(stderr, "Runtime error: Array element type definition is missing for '%s'.\n", d->children[0]->token->value);
                    free(lower_bounds);
                    free(upper_bounds);
                    EXIT_FAILURE_HANDLER();
                }

                // Determine VarType based on the element type node
                 if (elemTypeNode->type == AST_VARIABLE && elemTypeNode->token) {
                     const char *elemTypeStr = elemTypeNode->token->value;
                     if (strcasecmp(elemTypeStr, "integer") == 0) elemType = TYPE_INTEGER;
                     else if (strcasecmp(elemTypeStr, "real") == 0) elemType = TYPE_REAL;
                     else if (strcasecmp(elemTypeStr, "string") == 0) elemType = TYPE_STRING;
                     else if (strcasecmp(elemTypeStr, "char") == 0) elemType = TYPE_CHAR;
                     else if (strcasecmp(elemTypeStr, "boolean") == 0) elemType = TYPE_BOOLEAN;
                     // ... add other built-in types ...
                     else {
                        AST* userTypeDefinition = lookupType(elemTypeStr); // Check if it's a named type
                        if(userTypeDefinition) {
                           elemType = userTypeDefinition->var_type;
                           elemTypeNode = userTypeDefinition; // Use the actual definition node
                        } else {
                           fprintf(stderr, "Runtime error: Unknown array element type '%s' for variable '%s'.\n", elemTypeStr, d->children[0]->token->value);
                           free(lower_bounds);
                           free(upper_bounds);
                           EXIT_FAILURE_HANDLER();
                        }
                     }
                 } else if (elemTypeNode->type == AST_TYPE_REFERENCE && elemTypeNode->token) {
                     AST* userTypeDefinition = lookupType(elemTypeNode->token->value);
                     if(userTypeDefinition) {
                        elemType = userTypeDefinition->var_type;
                        elemTypeNode = userTypeDefinition; // Use the actual definition node
                     } else {
                        fprintf(stderr, "Runtime error: Undefined array element type '%s' for variable '%s'.\n", elemTypeNode->token->value, d->children[0]->token->value);
                        free(lower_bounds);
                        free(upper_bounds);
                        EXIT_FAILURE_HANDLER();
                     }
                 } else if (elemTypeNode->type == AST_RECORD_TYPE) { // Handle arrays of anonymous records
                    elemType = TYPE_RECORD;
                    // elemTypeNode already points to the record definition
                 } else if (elemTypeNode->type == AST_ARRAY_TYPE) { // Handle arrays of anonymous arrays
                     elemType = TYPE_ARRAY;
                     // elemTypeNode already points to the array definition
                 }
                 else {
                    fprintf(stderr, "Runtime error: Invalid array element type definition structure for '%s'. Node type: %s\n",
                            d->children[0]->token->value, astTypeToString(elemTypeNode->type));
                     free(lower_bounds);
                     free(upper_bounds);
                    EXIT_FAILURE_HANDLER();
                 }

                // Ensure sym->value is allocated before dereferencing
                if (sym->value == NULL) {
                     sym->value = malloc(sizeof(Value));
                     if (!sym->value) {
                         fprintf(stderr, "Memory allocation failed for symbol value\n");
                         EXIT_FAILURE_HANDLER();
                     }
                     sym->value->type = TYPE_ARRAY;
                }

                // Call makeArrayND with resolved info
                *sym->value = makeArrayND(dimensions, lower_bounds, upper_bounds, elemType, elemTypeNode);

                free(lower_bounds);
                free(upper_bounds);
            }
            else if (d->var_type == TYPE_STRING && d->right && d->right->right) {
                if (d->right->right && d->right->right->token &&
                    d->right->right->token->type == TOKEN_INTEGER_CONST) {
                    sym->value->max_length = atoi(d->right->right->token->value);
                }

#ifdef DEBUG
                printf("[DEBUG] String %s has Length = %d\n", sym->name, sym->value->max_length);
#endif
                if (sym->value->s_val && strlen(sym->value->s_val) > sym->value->max_length)
                    sym->value->s_val[sym->value->max_length] = '\0';
            }
        }
    }
}

//ExecStatus executeWithScope(AST *node, bool is_global_scope) { // Example new signature
void executeWithScope(AST *node, bool is_global_scope)  {
    if (!node)
        return;
    DEBUG_PRINT(">> Executing AST node: type=%s, token='%s'\n",
                astTypeToString(node->type), node->token ? node->token->value : "NULL");
    switch (node->type) {
        case AST_BREAK: // <<< ADD THIS CASE
            // Check if inside a loop? (This check is complex and often omitted,
            // relying on the loop handling logic to reset the flag)
            DEBUG_PRINT("[DEBUG] Break statement encountered.\n");
            break_requested = 1; // Set the global flag
            // Using the flag approach, this node itself does nothing else.
            // The loop checking the flag will handle the exit.
            break; // Break out of the switch statement
        case AST_PROGRAM:
            GlobalASTRoot = node;
            executeWithScope(node->right, true);
            break;
        case AST_ASSIGN: {
             // Evaluate the right-hand side expression FIRST.
             // 'val' might hold heap-allocated data (string, record, array).
             Value val = eval(node->right);

             // --- MODIFICATION START: Determine LValue type and handle appropriately ---

             if (node->left->type == AST_FIELD_ACCESS) {
                 // --- Handle Record Field Assignment ---

                 // 1. Find the base variable symbol for the record
                 AST* baseVarNode = node->left->left; // Start with node left of '.'
                 // Traverse up if the left side is complex (e.g., array[i].field)
                 // This simplified traversal might need enhancement for deeper nesting.
                 while (baseVarNode && baseVarNode->type != AST_VARIABLE) {
                     if (baseVarNode->left) baseVarNode = baseVarNode->left;
                     else {
                         fprintf(stderr, "Runtime error: Cannot find base variable for field assignment.\n");
                         freeValue(&val); // Free RHS value before exiting
                         EXIT_FAILURE_HANDLER();
                     }
                 }
                  if (!baseVarNode || baseVarNode->type != AST_VARIABLE || !baseVarNode->token) { // Added token check
                     fprintf(stderr, "Runtime error: Could not determine base variable for field assignment.\n");
                     freeValue(&val); // Free RHS value before exiting
                     EXIT_FAILURE_HANDLER();
                 }

                 Symbol *recSym = lookupSymbol(baseVarNode->token->value);
                 if (!recSym || !recSym->value || recSym->value->type != TYPE_RECORD) {
                     fprintf(stderr, "Runtime error: Base variable '%s' is not a record for field assignment.\n", baseVarNode->token->value);
                     freeValue(&val); // Free RHS value before exiting
                     EXIT_FAILURE_HANDLER();
                 }
                  if (recSym->is_const) { // Check if base record is constant
                      fprintf(stderr, "Runtime error: Cannot assign to field of constant record '%s'.\n", recSym->name);
                      freeValue(&val);
                      EXIT_FAILURE_HANDLER();
                  }

                 // 2. Find the target FieldValue within the *symbol's* record data
                 FieldValue *targetField = recSym->value->record_val;
                  const char *targetFieldName = node->left->token ? node->left->token->value : NULL; // Field name from the AST_FIELD_ACCESS node's token
                  if (!targetFieldName) { /* Should not happen if parser is correct */ EXIT_FAILURE_HANDLER(); }

                 while (targetField) {
                     if (targetField->name && strcmp(targetField->name, targetFieldName) == 0) {
                         break; // Found the field
                     }
                     targetField = targetField->next;
                 }

                 if (!targetField) {
                     fprintf(stderr, "Runtime error: Field '%s' not found in record '%s' for assignment.\n", targetFieldName, recSym->name);
                     freeValue(&val); // Free RHS value before exiting
                     EXIT_FAILURE_HANDLER();
                 }

                 // 3. Check type compatibility (using targetField->value.type and val.type)
                 if (targetField->value.type != val.type) {
                     // Allow compatible assignments like INTEGER to REAL, CHAR to STRING etc.
                     bool compatible = false;
                     if (targetField->value.type == TYPE_REAL && val.type == TYPE_INTEGER) compatible = true;
                     else if (targetField->value.type == TYPE_STRING && val.type == TYPE_CHAR) compatible = true; // Allow char to string
                     else if (targetField->value.type == TYPE_CHAR && val.type == TYPE_STRING && val.s_val && strlen(val.s_val)==1) compatible = true; // Allow single-char string to char
                     // Add other compatible pairs as needed (e.g., Integer to Byte/Word if desired)

                     if (!compatible) {
                          fprintf(stderr, "Runtime error: Type mismatch assigning to field '%s'. Expected %s, got %s.\n",
                                  targetFieldName, varTypeToString(targetField->value.type), varTypeToString(val.type));
                          freeValue(&val); // Free RHS value before exiting
                          EXIT_FAILURE_HANDLER();
                     }
                 }

                 // 4. Free the *current* value stored in the field BEFORE assigning/copying the new one.
                 freeValue(&targetField->value); // Frees heap data held by the current field value (e.g., old string)

                 // 5. Assign the new value (handle complex types with deep copies)
                 if (val.type == TYPE_STRING) {
                     // Use makeString which handles strdup and NULL checks
                     targetField->value = makeString(val.s_val);
                     // Free the temporary RHS value 'val' *after* its content is copied
                     freeValue(&val);
                 } else if (val.type == TYPE_RECORD) {
                     targetField->value = makeRecord(copyRecord(val.record_val)); // copyRecord performs deep copy
                     freeValue(&val); // Free the temporary RHS value 'val'
                 } else if (val.type == TYPE_ARRAY) {
                     targetField->value = makeCopyOfValue(&val); // makeCopyOfValue performs deep copy
                     freeValue(&val); // Free the temporary RHS value 'val'
                 } else {
                     // For simple types (int, real, char, bool, enum, etc.),
                     // direct assignment of the struct is sufficient (shallow copy of Value struct).
                     targetField->value = val;
                      // For simple types, 'val' is just a struct copy, no deep data to free via freeValue(&val).
                      // Type promotion (like int to real) should be handled above during assignment.
                      // Set the correct type if promotion occurred.
                      if (targetField->value.type == TYPE_REAL && val.type == TYPE_INTEGER) {
                          targetField->value.r_val = (double)val.i_val; // Assign converted value
                          targetField->value.type = TYPE_REAL; // Ensure type is REAL
                      }
                      // Handle char -> string assignment type
                      else if (targetField->value.type == TYPE_STRING && val.type == TYPE_CHAR) {
                            // makeString already handled this, type should be STRING
                            targetField->value.type = TYPE_STRING;
                      }
                       // Handle single-char string -> char assignment type
                       else if (targetField->value.type == TYPE_CHAR && val.type == TYPE_STRING) {
                            targetField->value.c_val = val.s_val[0]; // Assign char value
                            targetField->value.type = TYPE_CHAR; // Ensure type is CHAR
                            // We still need to free the temporary val from eval
                            freeValue(&val);
                       }

                 }

             } else if (node->left->type == AST_ARRAY_ACCESS) {
                  // --- Handle Array Element Assignment ---

                  // 1. Find the base variable symbol for the array/string
                  AST* baseVarNodeArr = node->left->left; // Start with node left of '['
                  // Simplified traversal (improve if needed for complex bases like record.array[i])
                  while(baseVarNodeArr && baseVarNodeArr->type != AST_VARIABLE) {
                      if (baseVarNodeArr->left) baseVarNodeArr = baseVarNodeArr->left;
                      else { /* Error */ fprintf(stderr, "Runtime error: Cannot find base variable for array/string assignment.\n"); freeValue(&val); EXIT_FAILURE_HANDLER(); }
                  }
                  if (!baseVarNodeArr || baseVarNodeArr->type != AST_VARIABLE || !baseVarNodeArr->token) { /* Error */ fprintf(stderr, "Runtime error: Could not determine base variable for array/string assignment.\n"); freeValue(&val); EXIT_FAILURE_HANDLER(); }

                  Symbol *sym = lookupSymbol(baseVarNodeArr->token->value);
                  if (!sym || !sym->value || (sym->value->type != TYPE_ARRAY && sym->value->type != TYPE_STRING)) {
                      fprintf(stderr, "Runtime error: Base variable '%s' is not an array or string for assignment.\n", baseVarNodeArr->token->value);
                      freeValue(&val); EXIT_FAILURE_HANDLER();
                  }
                   if (sym->is_const) { // Check if base array/string is constant
                       fprintf(stderr, "Runtime error: Cannot assign to element of constant '%s'.\n", sym->name);
                       freeValue(&val); EXIT_FAILURE_HANDLER();
                   }


                  // 2. Handle String Index Assignment
                  if (sym->value->type == TYPE_STRING) {
                      if (node->left->child_count != 1) { fprintf(stderr, "Runtime error: String assignment requires exactly one index.\n"); freeValue(&val); EXIT_FAILURE_HANDLER(); }

                      Value idxVal = eval(node->left->children[0]);
                      if(idxVal.type != TYPE_INTEGER) { fprintf(stderr, "Runtime error: String index must be an integer.\n"); freeValue(&val); EXIT_FAILURE_HANDLER(); }
                      long long idx = idxVal.i_val;

                      int len = (sym->value->s_val) ? (int)strlen(sym->value->s_val) : 0;
                      // Check bounds (Pascal uses 1-based indexing)
                      if (idx < 1 || idx > len) {
                          // Allow assigning to position len+1 only if assigning a non-empty char/string? Standard Pascal differs.
                          // Delphi/FPC might allow extending. For simplicity, error for now if index > len.
                           dumpASTFromRoot(node); dumpSymbolTable();
                          fprintf(stderr, "Runtime error: String index %lld out of bounds [1..%d] for assignment.\n", idx, len);
                          freeValue(&val); EXIT_FAILURE_HANDLER();
                      }

                      char char_to_assign = '\0';
                      if (val.type == TYPE_CHAR) {
                          char_to_assign = val.c_val;
                      } else if (val.type == TYPE_STRING && val.s_val && strlen(val.s_val) == 1) { // Allow single-char string
                          char_to_assign = val.s_val[0];
                      } else {
                          fprintf(stderr, "Runtime error: Assignment to string index requires a char or single-character string.\n");
                          freeValue(&val); EXIT_FAILURE_HANDLER();
                      }

                      // Ensure string is mutable (should be if not const)
                      if (!sym->value->s_val) { /* Should not happen if initialized */ }
                      else { sym->value->s_val[idx - 1] = char_to_assign; } // Assign char

                      freeValue(&val); // Free the temporary RHS value

                  }
                  // 3. Handle Array Element Assignment
                  else if (sym->value->type == TYPE_ARRAY) {
                      if (!node->left->children || node->left->child_count != sym->value->dimensions) {
                           fprintf(stderr, "Runtime error: Incorrect number of indices for array '%s'. Expected %d, got %d.\n", sym->name, sym->value->dimensions, node->left->child_count);
                           freeValue(&val); EXIT_FAILURE_HANDLER();
                      }

                      // Calculate index offset
                      int *indices = malloc(sizeof(int) * sym->value->dimensions);
                      if (!indices) { /* Mem error */ freeValue(&val); EXIT_FAILURE_HANDLER(); }
                      for (int i = 0; i < node->left->child_count; i++) {
                          Value idxVal = eval(node->left->children[i]);
                          if (idxVal.type != TYPE_INTEGER) { /* Type error */ free(indices); freeValue(&val); EXIT_FAILURE_HANDLER(); }
                          indices[i] = (int)idxVal.i_val;
                      }
                      int offset = computeFlatOffset(sym->value, indices); // Use symbol's bounds

                      // Bounds check offset
                      int total_size = 1;
                      for (int i = 0; i < sym->value->dimensions; i++) { total_size *= (sym->value->upper_bounds[i] - sym->value->lower_bounds[i] + 1); }
                      if (offset < 0 || offset >= total_size) { /* Bounds error */ fprintf(stderr, "Runtime error: Array index out of bounds during assignment (offset %d, size %d).\n", offset, total_size); free(indices); freeValue(&val); EXIT_FAILURE_HANDLER(); }

                      // Check type compatibility (optional but recommended)
                      VarType elementType = sym->value->element_type;
                      if (elementType != val.type) {
                          // Add compatibility checks if needed (e.g., int to real)
                          bool compatible = false;
                           if (elementType == TYPE_REAL && val.type == TYPE_INTEGER) compatible = true;
                          // Add more...
                          if (!compatible) {
                              fprintf(stderr, "Runtime error: Type mismatch assigning to array element. Expected %s, got %s.\n", varTypeToString(elementType), varTypeToString(val.type));
                              free(indices); freeValue(&val); EXIT_FAILURE_HANDLER();
                          }
                           // Perform conversion if compatible but different (e.g., int to real)
                           if (elementType == TYPE_REAL && val.type == TYPE_INTEGER) {
                               val.r_val = (double)val.i_val;
                               val.type = TYPE_REAL;
                           }
                      }

                      // Free existing element value BEFORE assigning new one
                      freeValue(&sym->value->array_val[offset]);

                      // Assign new value (deep copy complex types)
                      if (val.type == TYPE_STRING || val.type == TYPE_RECORD || val.type == TYPE_ARRAY) {
                          sym->value->array_val[offset] = makeCopyOfValue(&val);
                          freeValue(&val); // Free temporary RHS value as its content was copied
                      } else {
                          sym->value->array_val[offset] = val; // Simple type struct copy
                      }
                      free(indices);
                  }

             } else if (node->left->type == AST_VARIABLE) {
                 // --- Handle Simple Variable Assignment ---
                 Symbol *sym = lookupSymbol(node->left->token->value);
                 if (!sym) { /* Error */ fprintf(stderr, "Runtime error: variable '%s' not declared.\n", node->left->token->value); freeValue(&val); EXIT_FAILURE_HANDLER(); }
                  if (sym->is_const) { /* Error */ fprintf(stderr, "Runtime error: Cannot assign to constant '%s'.\n", sym->name); freeValue(&val); EXIT_FAILURE_HANDLER(); }

                 // Use updateSymbol which handles type checking, freeing old, copying new
                 updateSymbol(sym->name, val);

                 // Free the temporary RHS value AFTER updateSymbol has potentially copied its contents
                 freeValue(&val);

             } else {
                  // Invalid LValue type
                  fprintf(stderr, "Runtime error: Invalid lvalue node type in assignment: %s\n", astTypeToString(node->left->type));
                  freeValue(&val); // Free RHS value before exiting
                  EXIT_FAILURE_HANDLER();
             }

             // --- MODIFICATION END ---

             // assign_end label removed
             break; // Break from the main switch statement for AST_ASSIGN
         } // End case AST_ASSIGN
        case AST_CASE: {
                   // Evaluate the case expression
                   Value caseValue = eval(node->left);
                   int branchMatched = 0;
                   // Iterate over each case branch
                   for (int i = 0; i < node->child_count; i++) {
                       AST *branch = node->children[i];
                       int labelMatched = 0;
                       AST *labels = branch->left;  // May be compound or single label
                       if (labels->type == AST_COMPOUND) {
                           for (int j = 0; j < labels->child_count; j++) {
                               if (valueMatchesLabel(caseValue, labels->children[j])) {
                                   labelMatched = 1;
                                   break;
                               }
                           }
                       } else {
                           if (valueMatchesLabel(caseValue, labels)) {
                               labelMatched = 1;
                           }
                       }

                       if (labelMatched) {
                           executeWithScope(branch->right, false);
                           branchMatched = 1;
                           break;
                       }
                   }
                   // If no branch matched, execute the else branch if it exists.
                   if (!branchMatched && node->extra) {
                       executeWithScope(node->extra, false);
                   }
                   break;
        }
        case AST_BLOCK: {
            if (node->child_count >= 2) {
                AST *decl = node->children[0];
                bool is_global_block = node->is_global_scope;

                static bool global_symbols_inserted = false;
                if (!is_global_block || !global_symbols_inserted) {
                    processDeclarations(decl, is_global_block);
                    if (is_global_block)
                        global_symbols_inserted = true;
                }

                executeWithScope(node->children[1], is_global_block);
            }
            break;
        }
        case AST_COMPOUND:
            for (int i = 0; i < node->child_count; i++) {
                if (!node->children[i]) {
                    fprintf(stderr, "[BUG] AST_COMPOUND: child %d is NULL\n", i);
                    continue;
                }
                executeWithScope(node->children[i], false);
            }
            break;
        case AST_IF: {
            Value cond = eval(node->left);
            int is_true = (cond.type == TYPE_INTEGER || cond.type == TYPE_BOOLEAN) ? (cond.i_val != 0) : (cond.r_val != 0.0);
            if (is_true)
                executeWithScope(node->right, false);
            else if (node->extra)
                executeWithScope(node->extra, false);
            break;
        }
        case AST_WHILE: {
            while (1) {
                // Evaluate condition *before* checking break or executing body
                Value cond = eval(node->left); // <<< Evaluate condition here
                int is_true;
                if (cond.type == TYPE_REAL) {
                    is_true = (cond.r_val != 0.0);
                } else {
                    // Assume INTEGER or BOOLEAN (or other compatible ordinal)
                    is_true = (cond.i_val != 0);
                }

                if (!is_true) break; // Exit WHILE loop normally if condition is false

                // Condition is true, proceed to execute body
                break_requested = 0; // Reset flag *before* executing body
                executeWithScope(node->right, false); // Execute body

                // Check break flag *after* executing body
                if (break_requested) {
                    DEBUG_PRINT("[DEBUG] WHILE loop exiting due to break.\n");
                    break; // Exit the C 'while' loop due to break statement
                }
            }
             // Ensure flag is reset *after* the loop finishes, regardless of how it exited
             break_requested = 0;
             break; // Break from the switch case for AST_WHILE
        }
        case AST_REPEAT: {
            // No need for a separate 'repeat_broken' flag when using the global flag
            do {
                break_requested = 0; // Reset global flag before executing body statements
                executeWithScope(node->left, false); // Execute body statements
                
                // Check if break occurred within the body
                if (break_requested) {
                    DEBUG_PRINT("[DEBUG] REPEAT loop body exited due to break.\n");
                    break; // Exit the C 'do-while' loop immediately
                }
                
                // Evaluate the UNTIL condition *after* the body and break check
                Value cond = eval(node->right); // <<< Evaluate condition here
                int is_true; // <<< Use the result of eval
                if (cond.type == TYPE_REAL) {
                    is_true = (cond.r_val != 0.0);
                } else {
                    // Assume INTEGER or BOOLEAN (or other compatible ordinal)
                    is_true = (cond.i_val != 0);
                }
                
                if (is_true) // <<< Check the evaluated condition
                    break; // Exit REPEAT loop normally based on UNTIL condition
                
            } while (1); // The C loop condition is always true, logic is inside
            
            // Ensure flag is reset after the loop
            break_requested = 0;
            break; // Break from the switch case for AST_REPEAT
        }
        case AST_FOR_TO:
        case AST_FOR_DOWNTO: {
            // <<< CHANGE START: Get loop variable from children[0] >>>
            if (node->child_count < 1 || !node->children[0] || node->children[0]->type != AST_VARIABLE || !node->children[0]->token) {
                 fprintf(stderr, "Internal error: Invalid AST structure for FOR loop variable.\n");
                 dumpASTFromRoot(node); // Dump AST for context
                 EXIT_FAILURE_HANDLER();
            }
            AST* loopVarNode = node->children[0];
            const char *var_name = loopVarNode->token->value;
            // <<< CHANGE END >>>

            Value start_val = eval(node->left); // Start value from left (Correct)
            Value end_val = eval(node->right);   // End value from right (Correct)
            int step = (node->type == AST_FOR_TO) ? 1 : -1;

            Symbol *sym = lookupSymbol(var_name); // Use var_name fetched above
            if (!sym) {
                fprintf(stderr,"Runtime error: Loop variable %s not found\n", var_name);
                EXIT_FAILURE_HANDLER();
            }

            // Initial Assignment (using updateSymbol for type coercion)
            updateSymbol(var_name, start_val); // Use var_name

            // Determine Loop End Condition Value (compatible with loop var type)
            long long end_condition_val;
            VarType loop_var_type = sym->type;

            // --- Determine end condition value based on loop variable type ---
            // (This logic remains the same)
            if (loop_var_type == TYPE_CHAR) {
                 if(end_val.type == TYPE_CHAR) end_condition_val = end_val.c_val;
                 else if(end_val.type == TYPE_STRING && end_val.s_val && strlen(end_val.s_val)==1) end_condition_val = end_val.s_val[0];
                 else { /* Error: Incompatible end value */ fprintf(stderr, "Incompatible end value type %s for CHAR loop\n", varTypeToString(end_val.type)); EXIT_FAILURE_HANDLER(); }
            } else if (loop_var_type == TYPE_INTEGER || loop_var_type == TYPE_BYTE || loop_var_type == TYPE_WORD || loop_var_type == TYPE_ENUM || loop_var_type == TYPE_BOOLEAN) {
                 if(end_val.type == loop_var_type || end_val.type == TYPE_INTEGER || end_val.type == TYPE_ENUM) // Allow comparing Enum loop var with Int end val
                    end_condition_val = (end_val.type == TYPE_ENUM) ? end_val.enum_val.ordinal : end_val.i_val;
                 else { /* Error: Incompatible end value */ fprintf(stderr, "Incompatible end value type %s for %s loop\n", varTypeToString(end_val.type), varTypeToString(loop_var_type)); EXIT_FAILURE_HANDLER(); }
             } else { /* Error: Invalid loop variable type */ fprintf(stderr, "Invalid loop variable type: %s\n", varTypeToString(loop_var_type)); EXIT_FAILURE_HANDLER(); }


            // --- Main loop execution using C while(1) ---
            while (1) {
                // Read current value of loop variable
                Value current = *sym->value;

                // --- Loop Condition Check (using loop_var_type) ---
                long long current_condition_val;
                 if (loop_var_type == TYPE_CHAR) current_condition_val = current.c_val;
                 else if (loop_var_type == TYPE_ENUM) current_condition_val = current.enum_val.ordinal; // Use ordinal for comparison
                 else current_condition_val = current.i_val; // Assume other ordinals use i_val

                bool loop_finished = (node->type == AST_FOR_TO) ? (current_condition_val > end_condition_val)
                                                                : (current_condition_val < end_condition_val);
                if (loop_finished) break; // Exit FOR loop normally
                // --- End Loop Condition Check ---

                // Reset break flag before executing body
                break_requested = 0;

                executeWithScope(node->extra, false); // Execute loop body (from extra - Correct)

                // Check break flag after executing body
                if (break_requested) {
                     DEBUG_PRINT("[DEBUG] FOR loop exiting due to break.\n");
                     break; // Exit the C 'while(1)' loop
                }

                // --- Calculate and Assign Next Value ---
                // Re-read value again, as it might have been changed in the loop body
                current = *sym->value;
                long long next_ordinal;
                Value next_val = makeInt(0); // Initialize temporary value holder

                // (Logic for calculating next_ordinal and creating next_val remains the same)
                if (loop_var_type == TYPE_CHAR) {
                     if (current.type != TYPE_CHAR) { /* Error */ fprintf(stderr, "Loop variable %s changed type mid-loop\n", var_name); EXIT_FAILURE_HANDLER(); }
                     next_ordinal = current.c_val + step;
                     next_val = makeChar((char)next_ordinal);
                } else if (loop_var_type == TYPE_INTEGER || loop_var_type == TYPE_BYTE || loop_var_type == TYPE_WORD || loop_var_type == TYPE_ENUM || loop_var_type == TYPE_BOOLEAN) {
                     // Use ordinal for enum calculation
                     long long current_ordinal = (loop_var_type == TYPE_ENUM) ? current.enum_val.ordinal : current.i_val;
                     if ((loop_var_type != TYPE_ENUM && current.type != loop_var_type) || (loop_var_type == TYPE_ENUM && current.type != TYPE_ENUM)) { /* Error */ fprintf(stderr, "Loop variable %s changed type mid-loop\n", var_name); EXIT_FAILURE_HANDLER(); }
                     next_ordinal = current_ordinal + step; // Use ordinal for calculation

                     // Create the correct Value type
                     if (loop_var_type == TYPE_ENUM) {
                         next_val = makeEnum(current.enum_val.enum_name, (int)next_ordinal); // Recreate Enum value
                     } else {
                         next_val = makeInt(next_ordinal); // Create Integer/Byte/Word/Bool value
                         next_val.type = loop_var_type; // Ensure correct type is set
                     }
                } else {
                     fprintf(stderr, "Runtime error: Invalid FOR loop variable type '%s' during update.\n", varTypeToString(loop_var_type));
                     EXIT_FAILURE_HANDLER();
                }

                // Use updateSymbol for the assignment back to the loop variable
                updateSymbol(var_name, next_val); // Use var_name

                // Free temporary resources from makeEnum if necessary
                if (next_val.type == TYPE_ENUM && next_val.enum_val.enum_name) {
                    free(next_val.enum_val.enum_name);
                    next_val.enum_val.enum_name = NULL;
                }
                 // --- End Calculate and Assign ---

            } // end C while(1)

            // Ensure flag is reset after the loop finishes
            break_requested = 0;

            break; // Break from the switch case for AST_FOR_TO/DOWNTO
        }

        case AST_WRITE:
        case AST_WRITELN: {
            FILE *output = stdout;
            int startIndex = 0;
            if (node->child_count > 0) {
                Value firstArg = eval(node->children[0]);
                if (firstArg.type == TYPE_FILE && firstArg.f_val != NULL) {
                    output = firstArg.f_val;
                    startIndex = 1;
                }
            }

            for (int i = startIndex; i < node->child_count; i++) {
                Value val = eval(node->children[i]);

                if (node->type == AST_FORMATTED_EXPR) {
                    int width, decimals;
                    sscanf(val.s_val, "%d,%d", &width, &decimals);
                    if (val.type == TYPE_INTEGER) {
                        if (decimals >= 0)
                            fprintf(output, "%*.*lld", width, decimals, val.i_val);
                        else
                            fprintf(output, "%*lld", width, val.i_val);
                    } else if (val.type == TYPE_REAL) {
                        if (decimals >= 0)
                            fprintf(output, "%*.*f", width, decimals, val.r_val);
                        else
                            fprintf(output, "%*f", width, val.r_val);
                    } else if (val.type == TYPE_STRING || val.type == TYPE_CHAR) {
                        fprintf(output, "%*s", width, val.s_val);
                    } else {
                        fprintf(output, "%*s", width, "???");
                    }

                } else {
                    if (val.type == TYPE_INTEGER)
                        fprintf(output, "%lld", val.i_val);
                    else if (val.type == TYPE_REAL)
                        fprintf(output, "%f", val.r_val);
                    else if (val.type == TYPE_BOOLEAN)
                        fprintf(output, "%s", (val.i_val != 0) ? "true" : "false");
                    else if (val.type == TYPE_STRING)
                        fprintf(output, "%s", val.s_val);
                    else if (val.type == TYPE_CHAR)
                        fputc(val.c_val, output);
                    else if (val.type == TYPE_RECORD)
                        fprintf(output, "[record]");
                }
            }

            if (node->type == AST_WRITELN)
                fprintf(output, "\n");
            fflush(stdout); // Force the output buffer to be written to the terminal
            break;
        }

        case AST_READLN: {
            FILE *input = stdin;
            int startIndex = 0;
            // Check for optional file argument
            if (node->child_count > 0) {
                 Value firstArg = eval(node->children[0]);
                 if (firstArg.type == TYPE_FILE && firstArg.f_val != NULL) {
                      input = firstArg.f_val;
                      startIndex = 1;
                 }
                 // NOTE: Potential leak if firstArg was string/record/array from eval.
                 // Should ideally freeValue(&firstArg) IF it wasn't a simple type or the file used.
                 // Complex to handle correctly without knowing type beforehand. Assume simple or file for now.
            }

            // Loop to read into variable arguments
            for (int i = startIndex; i < node->child_count; i++) {
                AST *target_lvalue_node = node->children[i];
                if (!target_lvalue_node) {fprintf(stderr,"NULL LValue node in READLN\n"); EXIT_FAILURE_HANDLER();}

                char buffer[DEFAULT_STRING_CAPACITY];

                // Read raw input line using fgets
                if (fgets(buffer, sizeof(buffer), input) == NULL) {
                    if (feof(input)) buffer[0] = '\0';
                    else { fprintf(stderr,"Read error during READLN\n"); buffer[0] = '\0'; }
                }
                buffer[strcspn(buffer, "\n\r")] = 0; // Remove trailing newline/CR

                // --- Determine target type BEFORE creating value ---
                // This is essential for correct conversion from string buffer.
                // Requires looking up the type of the lvalue target. (Simplified below)
                VarType targetType = TYPE_VOID; // Default / Unknown
                // --- Placeholder: Determine target type (needs proper implementation) ---
                 if (target_lvalue_node->type == AST_VARIABLE) {
                     Symbol* targetSym = lookupSymbol(target_lvalue_node->token->value);
                     if(targetSym) targetType = targetSym->type;
                 } else if (target_lvalue_node->type == AST_FIELD_ACCESS) {
                     // Need to find field definition to get type - complex
                     // For now, assume STRING based on example context
                     targetType = TYPE_STRING;
                 } else if (target_lvalue_node->type == AST_ARRAY_ACCESS) {
                     // Need to find array definition and element type - complex
                     // For now, assume STRING based on example context
                      targetType = TYPE_STRING;
                 }
                 // --- End Placeholder ---


                // --- Convert buffer to appropriate Value type ---
                Value newValue;
                memset(&newValue, 0, sizeof(Value)); // Initialize

                switch(targetType) {
                    case TYPE_STRING:
                        newValue = makeString(buffer);
                        break;
                    case TYPE_INTEGER:
                        newValue = makeInt(atoll(buffer)); // Use atoll for long long
                        break;
                    case TYPE_REAL:
                        newValue = makeReal(atof(buffer));
                        break;
                    case TYPE_CHAR:
                        newValue = makeChar(buffer[0]); // Take first char
                        break;
                    case TYPE_BOOLEAN:
                        // Simplified: treat non-zero integer string as true
                        newValue = makeBoolean(atoi(buffer) != 0);
                        break;
                    // Add other target types as needed
                    default:
                         fprintf(stderr, "Runtime error: Cannot readln into variable of type %s\n", varTypeToString(targetType));
                         EXIT_FAILURE_HANDLER();
                         break; // Keep compiler happy
                }
                // ---

                // --- Assign the new value using the corrected helper function ---
                #ifdef DEBUG
                fprintf(stderr, "[DEBUG READLN] Assigning buffer content '%s' (as type %s) to lvalue node type %s\n",
                        buffer, varTypeToString(newValue.type), astTypeToString(target_lvalue_node->type));
                #endif
                assignValueToLValue(target_lvalue_node, newValue); // This now does deep copy
                // ---

                // --- Free the temporary newValue struct's content ---
                // since assignValueToLValue made its own deep copy.
                freeValue(&newValue);
                // ---
            }

            // Consume rest of line if no variable arguments were processed
            if (node->child_count == startIndex) {
                #ifdef DEBUG
                fprintf(stderr, "[DEBUG READLN] Consuming rest of line from %s (no variable args).\n", (input == stdin) ? "stdin" : "file");
                #endif
                int c;
                while ((c = fgetc(input)) != '\n' && c != EOF); // Read until newline or EOF
            }
            break; // Break from switch case AST_READLN
        } // End case AST_READLN

        case AST_READ: {
            FILE *input = stdin;
            int startIndex = 0;
            if (node->child_count > 0) {
                Value firstArg = eval(node->children[0]);
                if (firstArg.type == TYPE_FILE && firstArg.f_val != NULL) {
                    input = firstArg.f_val;
                    startIndex = 1;
                }
            }
            for (int i = startIndex; i < node->child_count; i++) {
                AST *target = node->children[i];
                char buffer[DEFAULT_STRING_CAPACITY];
                if (fscanf(input, "%254s", buffer) != 1) {
                    fprintf(stderr, "Runtime error: unable to read input from file.\n");
                    EXIT_FAILURE_HANDLER();
                }
                if (target->type == AST_FIELD_ACCESS) {
                    Value recVal = eval(target->left);
                    if (recVal.type != TYPE_RECORD) {
                        fprintf(stderr, "Runtime error: field access on non-record type.\n");
                        EXIT_FAILURE_HANDLER();
                    }
                    FieldValue *fv = recVal.record_val;
                    int found = 0;
                    while (fv) {
                        if (strcmp(fv->name, target->token->value) == 0) {
                            found = 1;
                            if (fv->value.type == TYPE_INTEGER)
                                fv->value = makeInt(atoi(buffer));
                            else if (fv->value.type == TYPE_REAL)
                                fv->value = makeReal(atof(buffer));
                            else if (fv->value.type == TYPE_STRING)
                                fv->value = makeString(buffer);
                            break;
                        }
                        fv = fv->next;
                    }
                    if (!found) {
                        fprintf(stderr, "Runtime error: field '%s' not found in record.\n", target->token->value);
                        EXIT_FAILURE_HANDLER();
                    }
                } else {
                    Symbol *sym = lookupSymbol(target->token->value);
                    if (!sym) {
                        fprintf(stderr, "Runtime error: variable '%s' not declared.\n", target->token->value);
                        EXIT_FAILURE_HANDLER();
                    }
                    if (sym->type == TYPE_INTEGER)
                        updateSymbol(target->token->value, makeInt(atoi(buffer)));
                    else if (sym->type == TYPE_REAL)
                        updateSymbol(target->token->value, makeReal(atof(buffer)));
                    else if (sym->type == TYPE_STRING)
                        updateSymbol(target->token->value, makeString(buffer));
                    else if (sym->type == TYPE_CHAR) {
                        char ch = (buffer[0] != '\0') ? buffer[0] : ' ';
                        updateSymbol(target->token->value, makeChar(ch));
                    }
                }
            }
            break;
        }

        case AST_PROCEDURE_CALL:
            (void)executeProcedureCall(node);
            break;
        case AST_NOOP:
        default:
            break;
    }
}

int computeFlatOffset(Value *array, int *indices) {
    int offset = 0;
    int multiplier = 1;

    for (int i = array->dimensions - 1; i >= 0; i--) {
        int idx = indices[i];
        int lb = array->lower_bounds[i];
        int ub = array->upper_bounds[i];

        if (idx < lb || idx > ub) {
            fprintf(stderr, "Runtime error: Index %d out of bounds [%d..%d] in dimension %d.\n",
                    idx, lb, ub, i + 1);
            EXIT_FAILURE_HANDLER();
        }

        offset += (idx - lb) * multiplier;
        multiplier *= (ub - lb + 1);
    }

    return offset;
}

Value makeCopyOfValue(Value *src) {
    Value v;
    v = *src;  // shallow copy to start

    switch (src->type) {
        case TYPE_STRING:
            if (src->s_val) {
                size_t len = strlen(src->s_val);
                v.s_val = malloc(len + 1);
                if (!v.s_val) {
                    fprintf(stderr, "Memory allocation failed in makeCopyOfValue (string)\n");
                    EXIT_FAILURE_HANDLER();
                }
                strcpy(v.s_val, src->s_val);
            } else {
                v.s_val = NULL;
            }
            break;
        case TYPE_ENUM:
            v.enum_val.enum_name = src->enum_val.enum_name ? strdup(src->enum_val.enum_name) : NULL;
            break;
        case TYPE_RECORD: {
            FieldValue *head = NULL, *tail = NULL;
            for (FieldValue *cur = src->record_val; cur; cur = cur->next) {
                FieldValue *copy = malloc(sizeof(FieldValue));
                copy->name = strdup(cur->name);
                copy->value = makeCopyOfValue(&cur->value);
                copy->next = NULL;
                if (tail)
                    tail->next = copy;
                else
                    head = copy;
                tail = copy;
            }
            v.record_val = head;
            break;
        }
        case TYPE_ARRAY: {
            int total = 1;
            v.lower_bounds = malloc(sizeof(int) * src->dimensions);
            v.upper_bounds = malloc(sizeof(int) * src->dimensions);
            for (int i = 0; i < src->dimensions; i++) {
                v.lower_bounds[i] = src->lower_bounds[i];
                v.upper_bounds[i] = src->upper_bounds[i];
                total *= (v.upper_bounds[i] - v.lower_bounds[i] + 1);
            }
            v.array_val = malloc(sizeof(Value) * total);
            for (int i = 0; i < total; i++) {
                v.array_val[i] = makeCopyOfValue(&src->array_val[i]);
            }
            break;
        }
        case TYPE_CHAR:
            // Already handled by shallow copy
            break;
        default:
            // ints, bools, reals etc. are already safely copied by value
            break;
    }

    return v;
}
