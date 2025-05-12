#include "interpreter.h"
#include "builtin.h"
#include "symbol.h"
#include "utils.h"
#include "globals.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#define PASCAL_DEFAULT_FLOAT_PRECISION 6

// Helper function to get the ordinal value from a Value struct
// Returns true on success, false if the type is not ordinal. Stores value in out_ord.
static bool getOrdinalValue(Value val, long long *out_ord) {
    if (!out_ord) return false; // Safety check

    switch (val.type) {
        case TYPE_INTEGER:
        case TYPE_BYTE:
        case TYPE_WORD:
        case TYPE_BOOLEAN: // Ord(False)=0, Ord(True)=1 stored in i_val
            *out_ord = val.i_val;
            return true;
        case TYPE_CHAR:
            *out_ord = (long long)val.c_val;
            return true;
        case TYPE_ENUM:
            *out_ord = (long long)val.enum_val.ordinal;
            return true;
        case TYPE_STRING: // Allow single-char strings
            if (val.s_val && strlen(val.s_val) == 1) {
                *out_ord = (long long)val.s_val[0];
                return true;
            }
            // Fall through if not single-char string
        default:
            return false; // Not a recognized ordinal type or valid single-char string
    }
}

// Helper to check if an ordinal value exists in a set's values array
static bool setContainsOrdinal(const Value* setVal, long long ordinal) {
    if (!setVal || setVal->type != TYPE_SET || !setVal->set_val.set_values) {
        return false;
    }
    for (int i = 0; i < setVal->set_val.set_size; i++) {
        if (setVal->set_val.set_values[i] == ordinal) {
            return true;
        }
    }
    return false;
}

// Helper to add an ordinal value to a result set, handling allocation and duplicates.
// Modifies the resultVal directly. Assumes resultVal is already TYPE_SET.
// Uses max_length field to track allocated capacity.
static void addOrdinalToResultSet(Value* resultVal, long long ordinal) {
    if (!resultVal || resultVal->type != TYPE_SET) return;

    // Avoid adding duplicates
    if (setContainsOrdinal(resultVal, ordinal)) {
        return;
    }

    // Check capacity and reallocate if necessary
    // Using max_length to store capacity here. Initialize resultVal->max_length appropriately before first call.
    if (resultVal->set_val.set_size >= resultVal->max_length) {
        int new_capacity = (resultVal->max_length == 0) ? 8 : resultVal->max_length * 2;
        long long* new_values = realloc(resultVal->set_val.set_values, sizeof(long long) * new_capacity);
        if (!new_values) {
            fprintf(stderr, "FATAL: realloc failed in addOrdinalToResultSet\n");
            // Consider freeing partially built set before exiting? Complex.
            EXIT_FAILURE_HANDLER();
        }
        resultVal->set_val.set_values = new_values;
        resultVal->max_length = new_capacity; // Update capacity store
    }

    // Add the new element
    resultVal->set_val.set_values[resultVal->set_val.set_size] = ordinal;
    resultVal->set_val.set_size++;
}

// Helper function to map 0-15 to ANSI FG codes (30-37 standard, 90-97 bright)
static int map16FgColorToAnsi(int colorCode, bool isBold) {
    colorCode %= 16;
    if (isBold || (colorCode >= 8 && colorCode <= 15)) { // Use bright codes if bold OR color is 8-15
        // Map 8-15 to 90-97
        return 90 + (colorCode % 8);
    } else {
        // Map 0-7 to 30-37
        return 30 + (colorCode % 8);
    }
}

// Helper function to map 0-7 to ANSI BG codes (40-47)
static int map16BgColorToAnsi(int colorCode) {
    // Standard BG only uses 0-7
    return 40 + (colorCode % 8);
}

Value evalSet(AST *node) {
    // ... (initial checks and variable declarations remain the same) ...
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_SET;
    // Using max_length to track capacity. Initialize.
    v.max_length = 0; // Capacity
    v.set_val.set_size = 0; // Current size
    v.set_val.set_values = NULL; // Pointer to elements


    for (int i = 0; i < node->child_count; i++) {
        AST *element = node->children[i];
        if (!element) continue; // Safety check

        if (element->type == AST_SUBRANGE) {
            if (!element->left || !element->right) { /* Error: Invalid subrange node */ continue; }

            Value startVal = eval(element->left);
            Value endVal = eval(element->right);
            long long start_ord, end_ord;

            // --- MODIFIED: Use helper functions for ordinal check and value extraction ---
            bool start_ok = getOrdinalValue(startVal, &start_ord);
            bool end_ok = getOrdinalValue(endVal, &end_ord);

            if (!start_ok || !end_ok) {
                 fprintf(stderr, "Runtime error: Set range bounds must be ordinal types. Got Start=%s, End=%s\n",
                         varTypeToString(startVal.type), varTypeToString(endVal.type));
                 // Cleanup temporary evaluated values before exiting
                 freeValue(&startVal);
                 freeValue(&endVal);
                 // Cleanup partially built set 'v'
                 freeValue(&v);
                 EXIT_FAILURE_HANDLER();
            }

            // Basic compatibility check: For now, allow mixing if convertible to ordinals.
            // Stricter checks might compare startVal.type and endVal.type if needed.

            if (start_ord > end_ord) {
               // Empty range is valid, do nothing
            } else {
               // Iterate and add ordinals to the set
               for (long long val_ord = start_ord; val_ord <= end_ord; val_ord++) {
                   // addOrdinalToResultSet handles duplicates and allocation
                   addOrdinalToResultSet(&v, val_ord);
                }
            }
            // Free temporary values evaluated for bounds
            freeValue(&startVal);
            freeValue(&endVal);
            // --- END MODIFICATION ---

        } else { // Single element
            Value elemVal = eval(element);
            long long elem_ord;

            // --- MODIFIED: Use helper functions ---
            bool elem_ok = getOrdinalValue(elemVal, &elem_ord);

            if (!elem_ok) {
                 fprintf(stderr, "Runtime error: Set elements must be ordinal type. Got %s\n", varTypeToString(elemVal.type));
                 // Cleanup temporary evaluated value before exiting
                 freeValue(&elemVal);
                 // Cleanup partially built set 'v'
                 freeValue(&v);
                 EXIT_FAILURE_HANDLER();
            }

            // Add the ordinal value to the set
            addOrdinalToResultSet(&v, elem_ord);
            // Free temporary value evaluated for element
            freeValue(&elemVal);
            // --- END MODIFICATION ---
        }
    } // End for loop iterating through set constructor elements

    // Optional: Trim excess capacity if desired
    // if (v.set_val.set_size > 0 && v.set_val.set_size < v.max_length) {
    //     long long* final_values = realloc(v.set_val.set_values, sizeof(long long) * v.set_val.set_size);
    //     if (final_values) {
    //          v.set_val.set_values = final_values;
    //          v.max_length = v.set_val.set_size;
    //      } // Ignore realloc failure for trimming
    // } else if (v.set_val.set_size == 0) { // If set ended up empty
    //      free(v.set_val.set_values);
    //      v.set_val.set_values = NULL;
    //      v.max_length = 0;
    // }


    return v; // Return the constructed set Value
}

Value executeProcedureCall(AST *node) {
    // --- Verify Input Node ---
    if (!node || (node->type != AST_PROCEDURE_CALL && node->type != AST_FUNCTION_DECL) || !node->token) {
        fprintf(stderr, "Internal Error: Invalid AST node passed to executeProcedureCall.\n");
        EXIT_FAILURE_HANDLER();
    }
    // --- End Verify ---

    // Handle Built-in procedures first
    if (isBuiltin(node->token->value)) {
        Value retVal = executeBuiltinProcedure(node);
#ifdef DEBUG
        fprintf(stderr, "DEBUG: Builtin procedure '%s' returned type %s\n",
                node->token->value, varTypeToString(retVal.type));
#endif
        return retVal;
    }

    // Look up user-defined procedure
    Procedure *proc = procedure_table;
    char *lowerProcName = strdup(node->token->value);
    if (!lowerProcName) { fprintf(stderr,"FATAL: strdup failed for proc name lookup\n"); EXIT_FAILURE_HANDLER(); }
    for(int i=0; lowerProcName[i]; i++){ lowerProcName[i] = (char)tolower((unsigned char)lowerProcName[i]); }

    while (proc) {
        if (proc->name && strcmp(proc->name, lowerProcName) == 0)
            break;
        proc = proc->next;
    }
    free(lowerProcName);

    if (!proc || !proc->proc_decl) {
        fprintf(stderr, "Runtime error: routine '%s' not found or declaration missing.\n", node->token->value);
        EXIT_FAILURE_HANDLER();
    }

    int num_params = proc->proc_decl->child_count;

#ifdef DEBUG
    fprintf(stderr, "[DEBUG EXEC_PROC] ENTERING: Node %p (%s '%s'), Expecting %d params.\n",
            (void*)node, astTypeToString(node->type), node->token->value, num_params);
    fprintf(stderr, "[DEBUG EXEC_PROC]            AST Node State: child_count=%d, children_ptr=%p\n",
            node->child_count, (void*)node->children);
#endif

    if (node->child_count != num_params) {
        fprintf(stderr, "Runtime error: Argument count mismatch for call to '%s'. Expected %d, got %d.\n",
                proc->name, num_params, node->child_count);
        EXIT_FAILURE_HANDLER();
    }
    if (num_params > 0 && node->children == NULL) {
        fprintf(stderr, "CRITICAL ERROR: Procedure '%s' expects %d params, but AST node->children pointer is NULL before argument evaluation!\n",
                proc->name, num_params);
        dumpAST(node, 0);
        dumpAST(proc->proc_decl, 0);
        EXIT_FAILURE_HANDLER();
    }

    Value *arg_values = NULL;
    if (num_params > 0) {
        arg_values = malloc(sizeof(Value) * num_params);
        if (!arg_values) {
            fprintf(stderr, "FATAL: malloc failed for arg_values in executeProcedureCall\n");
            EXIT_FAILURE_HANDLER();
        }
        for (int i = 0; i < num_params; i++) {
            arg_values[i].type = TYPE_VOID;
        }
    }

    for (int i = 0; i < num_params; i++) {
        AST *paramNode = proc->proc_decl->children[i];
        if (!paramNode) { fprintf(stderr, "Missing formal param %d\n", i); EXIT_FAILURE_HANDLER(); }

        if (paramNode->by_ref) {
            arg_values[i].type = TYPE_VOID;
        } else {
            if (i >= node->child_count || node->children == NULL) {
                fprintf(stderr, "CRITICAL ERROR: Trying to access actual argument node->children[%d], but child_count=%d or children=%p\n",
                        i, node->child_count, (void*)node->children);
                dumpAST(node,0);
                EXIT_FAILURE_HANDLER();
            }
            AST* actualArgNode = node->children[i];
            if (!actualArgNode) {
                fprintf(stderr, "CRITICAL ERROR: Actual argument node at index %d is NULL for call to '%s'.\n", i, proc->name);
                dumpAST(node,0);
                EXIT_FAILURE_HANDLER();
            }
#ifdef DEBUG
            fprintf(stderr, "[DEBUG EXEC_PROC] Evaluating value parameter %d (AST Type: %s)\n", i, astTypeToString(actualArgNode->type));
#endif
            Value actualVal = eval(actualArgNode);
#ifdef DEBUG
            fprintf(stderr, "[DEBUG EXEC_PROC] Arg %d evaluated to type %s\n", i, varTypeToString(actualVal.type));
#endif
            arg_values[i] = makeCopyOfValue(&actualVal);
#ifdef DEBUG
            fprintf(stderr, "[DEBUG EXEC_PROC] Copied arg %d value (type %s)\n", i, varTypeToString(arg_values[i].type));
#endif
            freeValue(&actualVal);
        }
    }

    SymbolEnvSnapshot snapshot;
    saveLocalEnv(&snapshot);

    for (int i = num_params - 1; i >= 0; i--) {
        AST *paramNode = proc->proc_decl->children[i];
        if (!paramNode || paramNode->type != AST_VAR_DECL || paramNode->child_count < 1 || !paramNode->children[0] || !paramNode->children[0]->token) {
             fprintf(stderr, "Internal Error: Invalid formal parameter AST structure for param %d of '%s'.\n", i, proc->name);
             EXIT_FAILURE_HANDLER();
        }

        char *paramName = paramNode->children[0]->token->value;
        VarType ptype = paramNode->var_type;
        AST *type_def = paramNode->right;

        if (paramNode->by_ref) {
            AST* actualArgNode = node->children[i]; // AST node for the argument passed
             if (!actualArgNode) {
                 fprintf(stderr, "CRITICAL ERROR: Actual argument AST node is NULL for VAR parameter '%s' (index %d).\n", paramName, i);
                 EXIT_FAILURE_HANDLER();
             }

            // Check if the argument is a valid variable reference for VAR param
            if (actualArgNode->type != AST_VARIABLE && actualArgNode->type != AST_FIELD_ACCESS && actualArgNode->type != AST_ARRAY_ACCESS) {
                fprintf(stderr, "Runtime error: var parameter '%s' must be a variable reference, field, or array element. Got %s.\n", paramName, astTypeToString(actualArgNode->type));
                EXIT_FAILURE_HANDLER();
            }

            char *argVarName = NULL;
            // For simple variables, the name is directly in the token.
            // For field/array access, argVarName will be more complex to extract if needed for error messages,
            // but resolveLValueToPtr will handle finding the actual symbol/memory.
            // Here, we primarily need argVarName for the lookup.
            if (actualArgNode->type == AST_VARIABLE && actualArgNode->token && actualArgNode->token->value) {
                argVarName = actualArgNode->token->value;
            } else if (actualArgNode->type == AST_FIELD_ACCESS && actualArgNode->left && actualArgNode->left->token && actualArgNode->token) {
                // For error messages, construct a name like "recordVar.fieldName"
                // This is just for a better error message; the core lookup will use the base name.
                // For now, we'll just use the field name or a generic if the base is complex.
                // This part can be improved for more descriptive errors for complex LValues.
                argVarName = actualArgNode->token->value; // Fallback to field name for messages
            } else if (actualArgNode->type == AST_ARRAY_ACCESS && actualArgNode->left && actualArgNode->left->token) {
                 argVarName = actualArgNode->left->token->value; // Base name of the array
            }
             else {
                 fprintf(stderr, "Runtime error: Could not determine variable name for VAR parameter '%s'. Actual argument AST type: %s.\n", paramName, astTypeToString(actualArgNode->type));
                 EXIT_FAILURE_HANDLER();
            }
            
            if (!argVarName) { // Should be caught by above, but defensive
                fprintf(stderr, "CRITICAL ERROR: argVarName is NULL for VAR parameter '%s'.\n", paramName);
                EXIT_FAILURE_HANDLER();
            }

            // --- MODIFIED AND ENHANCED LOOKUP FOR VAR PARAMETERS ---
            #ifdef DEBUG
            fprintf(stderr, "[DEBUG EXEC_PROC_VAR] Binding VAR parameter '%s'. Actual argument LValue is (token: '%s', AST type: %s).\n",
                    paramName, argVarName, astTypeToString(actualArgNode->type));
            fprintf(stderr, "[DEBUG EXEC_PROC_VAR]   snapshot.head (caller's localSymbols) is: %p\n", (void*)snapshot.head);
            fprintf(stderr, "[DEBUG EXEC_PROC_VAR]   globalSymbols is: %p\n", (void*)globalSymbols);
            fflush(stderr);
            #endif

            Symbol *callerSym = NULL;
            // If actualArgNode is a simple variable, look it up by its name.
            // For complex LValues (field/array), resolveLValueToPtr gets the target Value*,
            // and the symbol is the one owning that Value*. This is more complex.
            // The current `lookupSymbolIn` and `lookupGlobalSymbol` expect a simple name.
            // For `BallPixels` (AST_VARIABLE), `argVarName` will be "ballpixels".

            if (snapshot.head) { // Check caller's local scope first
                callerSym = lookupSymbolIn(snapshot.head, argVarName);
                #ifdef DEBUG
                fprintf(stderr, "[DEBUG EXEC_PROC_VAR]   Lookup in snapshot.head for '%s' found: %p\n", argVarName, (void*)callerSym);
                if (callerSym) dumpSymbol(callerSym); else fprintf(stderr, "    (Not found in snapshot.head for '%s')\n", argVarName);
                fflush(stderr);
                #endif
            }

            if (!callerSym) { // If not in caller's locals, or if caller was global (snapshot.head is NULL)
                callerSym = lookupGlobalSymbol(argVarName); // This uses argVarName ("ballpixels")
                #ifdef DEBUG
                fprintf(stderr, "[DEBUG EXEC_PROC_VAR]   Lookup in globalSymbols for '%s' found: %p\n", argVarName, (void*)callerSym);
                if (callerSym) dumpSymbol(callerSym); else fprintf(stderr, "    (Not found in globalSymbols for '%s')\n", argVarName);
                fflush(stderr);
                #endif
            }
            // --- END MODIFIED LOOKUP ---

            if (!callerSym) {
                fprintf(stderr, "Runtime error: variable '%s' (passed as VAR parameter '%s') not declared in accessible scope.\n", argVarName, paramName);
                #ifdef DEBUG
                fprintf(stderr, "\n--- BEGIN SYMBOL TABLE DUMP (from executeProcedureCall VAR param error) ---\n");
                dumpSymbolTable();
                fprintf(stderr, "--- END SYMBOL TABLE DUMP ---\n");
                fflush(stderr);
                #endif
                EXIT_FAILURE_HANDLER();
            }
            if (!callerSym->value) {
                fprintf(stderr, "CRITICAL ERROR: Caller symbol '%s' for VAR parameter '%s' has NULL value pointer.\n", callerSym->name, paramName);
                EXIT_FAILURE_HANDLER();
            }
            
            // Type Compatibility Check for VAR parameters
            // This needs to be more robust for structural types like arrays/records.
            // For now, simple VarType comparison.
            bool types_compatible_for_var = (callerSym->type == ptype);
            if (!types_compatible_for_var) {
                // Special case: Allow passing an array of byte if the formal param is also array of byte,
                // even if their named types are different but structure matches.
                // This requires a deeper type check.
                if (callerSym->type == TYPE_ARRAY && ptype == TYPE_ARRAY &&
                    callerSym->value && callerSym->value->element_type == TYPE_BYTE &&
                    type_def && type_def->type == AST_ARRAY_TYPE && type_def->right && type_def->right->var_type == TYPE_BYTE) {
                    // Basic check for element type byte, could extend to dimensions/bounds
                    types_compatible_for_var = true;
                     #ifdef DEBUG
                     fprintf(stderr, "[DEBUG EXEC_PROC_VAR] Allowing VAR param type mismatch due to array of byte compatibility for '%s'. Formal: %s, Actual: %s\n", paramName, varTypeToString(ptype), varTypeToString(callerSym->type));
                     #endif
                } else {
                    fprintf(stderr, "Runtime error: Type mismatch for VAR parameter '%s'. Expected %s, got %s for variable '%s'.\n", paramName, varTypeToString(ptype), varTypeToString(callerSym->type), callerSym->name);
                    EXIT_FAILURE_HANDLER();
                }
            }


            insertLocalSymbol(paramName, ptype, type_def, false); // is_variable_declaration is false for params
            Symbol *localSym = lookupLocalSymbol(paramName);
            if (!localSym || !localSym->value) {
                fprintf(stderr, "CRITICAL ERROR: Failed to insert/find local symbol for VAR parameter '%s'.\n", paramName);
                EXIT_FAILURE_HANDLER();
            }

            freeValue(localSym->value); // Free the default value created by insertLocalSymbol
            free(localSym->value);      // Free the Value struct itself

            localSym->value = callerSym->value; // Alias: Make local symbol point to the caller's Value struct
            localSym->is_alias = true;
#ifdef DEBUG
            fprintf(stderr, "[DEBUG EXEC_PROC] Aliased VAR parameter '%s' to caller symbol '%s' (Value* %p)\n", paramName, callerSym->name, (void*)localSym->value);
#endif
        } else { // Handle VALUE parameter
#ifdef DEBUG
            fprintf(stderr, "[DEBUG EXEC_PROC] Inserting value parameter '%s' (type %s)\n", paramName, varTypeToString(ptype));
#endif
            insertLocalSymbol(paramName, ptype, type_def, false);
            Symbol *sym = lookupLocalSymbol(paramName);
            if (!sym) {
                fprintf(stderr, "CRITICAL ERROR: Failed to insert/find local symbol for VALUE parameter '%s'.\n", paramName);
                EXIT_FAILURE_HANDLER();
            }
            sym->is_alias = false;

            if (arg_values[i].type == TYPE_VOID && ptype != TYPE_VOID) { // Check if value wasn't prepared
                fprintf(stderr, "CRITICAL ERROR: Value for parameter '%s' (index %d, type %s) was not evaluated/copied correctly (is TYPE_VOID).\n", paramName, i, varTypeToString(ptype));
                EXIT_FAILURE_HANDLER();
            }
#ifdef DEBUG
            fprintf(stderr, "[DEBUG EXEC_PROC] Updating symbol '%s' with copied value (type %s from arg_values[%d])\n", paramName, varTypeToString(arg_values[i].type), i);
#endif
            updateSymbol(paramName, arg_values[i]); // updateSymbol makes the final copy and handles types

            // The value from arg_values[i] has been deep copied by updateSymbol.
            // We now need to free the intermediate copy stored in arg_values[i].
            // freeValue(&arg_values[i]); // This was correct.
            // updateSymbol is now responsible for handling the input `val` it receives.
            // If updateSymbol consumes `val` (e.g., takes ownership of its s_val), then arg_values[i] is "empty".
            // If updateSymbol makes its own copy from `val`, then `val` (arg_values[i]) needs freeing here.
            // Assuming updateSymbol makes a copy, so free arg_values[i]
             freeValue(&arg_values[i]); // Original was correct if updateSymbol copies.
            arg_values[i].type = TYPE_VOID; // Mark as freed/processed
        }
    } // End Stage 2 Loop

    if (arg_values) {
        free(arg_values);
    }

    // --- Stage 3: Execute Body & Handle Return ---
    Value retVal = makeVoid();
    if (proc->proc_decl->type == AST_FUNCTION_DECL) {
        AST *returnTypeDefNode = proc->proc_decl->right;
        if (!returnTypeDefNode) { fprintf(stderr, "Internal Error: Function '%s' missing return type definition node.\n", proc->name); EXIT_FAILURE_HANDLER(); }
        VarType retType = returnTypeDefNode->var_type;

        insertLocalSymbol("result", retType, returnTypeDefNode, false);
        Symbol *resSym = lookupLocalSymbol("result");
        if (!resSym) { fprintf(stderr, "Internal Error: Could not create 'result' symbol for function '%s'.\n", proc->name); EXIT_FAILURE_HANDLER(); }
        resSym->is_alias = false;

        insertLocalSymbol(proc->name, retType, returnTypeDefNode, false);
        Symbol *funSym = lookupLocalSymbol(proc->name);
        if (!funSym) { fprintf(stderr, "Internal Error: Could not create function name alias symbol for '%s'.\n", proc->name); EXIT_FAILURE_HANDLER(); }
        if(funSym->value) { freeValue(funSym->value); free(funSym->value); }
        funSym->value = resSym->value;
        funSym->is_alias = true;

        current_function_symbol = funSym;

        if (!proc->proc_decl->extra) { fprintf(stderr, "Internal Error: Function '%s' missing body (extra node).\n", proc->name); EXIT_FAILURE_HANDLER(); }
        executeWithScope(proc->proc_decl->extra, false);

        Symbol* finalResultSym = lookupLocalSymbol("result");
        if (!finalResultSym || !finalResultSym->value) { fprintf(stderr, "Internal Error: Could not retrieve 'result' value for function '%s'.\n", proc->name); EXIT_FAILURE_HANDLER(); }
        else {
            retVal = makeCopyOfValue(finalResultSym->value);
        }

        restoreLocalEnv(&snapshot);
        current_function_symbol = NULL;
        return retVal;

    } else { // Procedure
        if (!proc->proc_decl->right) { fprintf(stderr, "Internal Error: Procedure '%s' missing body (right node).\n", proc->name); EXIT_FAILURE_HANDLER(); }
        executeWithScope(proc->proc_decl->right, false);
        restoreLocalEnv(&snapshot);
        return makeVoid();
    }
}

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
        // Evaluate the inner expression (the value to be formatted)
        Value val_to_format = eval(node->left);

        // Extract width and decimals from the formatting token
        int width = 0, decimals = -1;
        if (node->token && node->token->value) {
            sscanf(node->token->value, "%d,%d", &width, &decimals);
        } else {
             // Handle missing token case if necessary (shouldn't happen if parser is correct)
             fprintf(stderr,"Warning: Missing formatting token in AST_FORMATTED_EXPR node %p during eval.\n", (void*)node);
        }

        char buf[DEFAULT_STRING_CAPACITY]; // Ensure this is large enough
        buf[0] = '\0'; // Initialize buffer

        // --- Apply Formatting Based on Type of val_to_format ---

        if (val_to_format.type == TYPE_REAL) {
            if (decimals >= 0) { // :width:decimals
                snprintf(buf, sizeof(buf), "%*.*f", width, decimals, val_to_format.r_val);
            } else { // :width only
                int effective_precision = PASCAL_DEFAULT_FLOAT_PRECISION;
                 // Use %g which is often closer to Pascal's default behavior than %f or %E alone
                snprintf(buf, sizeof(buf), "%*.*E", width, effective_precision, val_to_format.r_val); // Use %E
            }
        } else if (val_to_format.type == TYPE_INTEGER || val_to_format.type == TYPE_BYTE || val_to_format.type == TYPE_WORD) {
            snprintf(buf, sizeof(buf), "%*lld", width, val_to_format.i_val);
        } else if (val_to_format.type == TYPE_STRING) {
            const char* source_str = val_to_format.s_val ? val_to_format.s_val : "";
            int len = (int)strlen(source_str);
            int precision = len;
            if (width > 0 && width < len) {
                precision = width; // Truncate
            }
            snprintf(buf, sizeof(buf), "%*.*s", width, precision, source_str);
        } else if (val_to_format.type == TYPE_BOOLEAN) {
             const char* bool_str = val_to_format.i_val ? "TRUE" : "FALSE";
             int len = (int)strlen(bool_str);
             int precision = len;
             if (width > 0 && width < len) {
                 precision = width; // Truncate
             }
             snprintf(buf, sizeof(buf), "%*.*s", width, precision, bool_str);
        } else if (val_to_format.type == TYPE_CHAR) {
             snprintf(buf, sizeof(buf), "%*c", width, val_to_format.c_val);
        }
        // Add other types (e.g., Enum) if needed
        else {
             // Default for unhandled types during formatting
             snprintf(buf, sizeof(buf), "%*s", width, "?"); // Use "?" or similar
        }

        // Create the final string value
        Value result = makeString(buf);

        // Free the temporary inner value that was formatted
        freeValue(&val_to_format);

        // Return the Value containing the formatted string
        return result;

    } // End of the if (node->type == AST_FORMATTED_EXPR) block

    switch (node->type) {
        case AST_ARRAY_ACCESS: {
            #ifdef DEBUG
            fprintf(stderr, "[DEBUG EVAL] Evaluating AST_ARRAY_ACCESS.\n");
            #endif

            // --- Evaluate the base variable/expression (e.g., 's' or 'myArr') ---
            Value baseVal = eval(node->left);
            Value result = makeVoid(); // Default result

            // --- Handle Based on Base Type ---
            if (baseVal.type == TYPE_ARRAY) {
                 // --- Array Element Evaluation Logic ---
                 #ifdef DEBUG
                 fprintf(stderr, "[DEBUG EVAL ARR_ACCESS] Base is ARRAY. Resolving element pointer.\n");
                 #endif
                 // 1. Resolve the lvalue to get a pointer to the element's Value struct
                 //    We call resolveLValueToPtr here because it handles multi-dim index calculation
                 //    and bounds checking needed for *reading* the correct element.
                 Value* elementPtr = resolveLValueToPtr(node); // Pass the original ARRAY_ACCESS node
                 if (!elementPtr) {
                      // Error should have been reported by resolveLValueToPtr or sub-evals
                      fprintf(stderr, "Runtime error: Failed to resolve array element pointer during evaluation.\n");
                      freeValue(&baseVal); // Free base value evaluated earlier
                      return makeVoid(); // Return void/error value
                 }

                 // 2. Return a DEEP COPY of the value found at that pointer
                 #ifdef DEBUG
                 fprintf(stderr, "[DEBUG EVAL ARR_ACCESS] Array element resolved. Type: %s. Returning copy.\n", varTypeToString(elementPtr->type));
                 #endif
                 result = makeCopyOfValue(elementPtr);

                 // Ensure copied strings are not NULL
                 if (result.type == TYPE_STRING && result.s_val == NULL) {
                     result.s_val = strdup("");
                     if (!result.s_val) { /* Malloc error */
                         freeValue(&baseVal);
                         EXIT_FAILURE_HANDLER();
                     }
                 }
                 // --- End Array Element Logic ---

            } else if (baseVal.type == TYPE_STRING) {
                // --- NEW: String Element Evaluation Logic (No resolveLValueToPtr needed here) ---
                #ifdef DEBUG
                fprintf(stderr, "[DEBUG EVAL ARR_ACCESS] Base is STRING. Evaluating index for read access.\n");
                #endif
                // 1. Check index count (must be 1 for string)
                if (node->child_count != 1) {
                     fprintf(stderr, "Runtime error: String indexing requires exactly one index.\n");
                     freeValue(&baseVal);
                     EXIT_FAILURE_HANDLER(); // Exit on structural error
                }
                // 2. Evaluate index expression
                Value indexVal = eval(node->children[0]);
                if (indexVal.type != TYPE_INTEGER) {
                    fprintf(stderr, "Runtime error: String index must be an integer.\n");
                    freeValue(&indexVal);
                    freeValue(&baseVal);
                    EXIT_FAILURE_HANDLER(); // Exit on type error
                }
                long long idx_ll = indexVal.i_val;
                freeValue(&indexVal); // Free evaluated index value

                // 3. Bounds check (Pascal strings are 1-based)
                const char *str_ptr = baseVal.s_val ? baseVal.s_val : "";
                size_t len = strlen(str_ptr);
                if (idx_ll < 1 || (size_t)idx_ll > len) {
                     fprintf(stderr, "Runtime error: String index (%lld) out of bounds [1..%zu].\n", idx_ll, len);
                     freeValue(&baseVal);
                     EXIT_FAILURE_HANDLER(); // Exit on bounds error
                }
                size_t idx_0based = (size_t)idx_ll - 1; // Convert to 0-based index

                // 4. Extract character and return as TYPE_CHAR Value
                result = makeChar(str_ptr[idx_0based]);
                #ifdef DEBUG
                fprintf(stderr, "[DEBUG EVAL ARR_ACCESS] String index %lld resolved to char '%c'.\n", idx_ll, result.c_val);
                #endif
                // --- End NEW String Logic ---

            } else {
                // --- Error: Indexing applied to non-array/non-string type ---
                fprintf(stderr, "Runtime error: Cannot apply indexing to type %s.\n", varTypeToString(baseVal.type));
                freeValue(&baseVal);
                EXIT_FAILURE_HANDLER(); // Exit on type error
            }

            // Free the evaluated base value (string, array struct copy, etc.)
            freeValue(&baseVal);

            return result; // Return the evaluated element (char or copied array element)
        } // End case AST_ARRAY_ACCESS
        case AST_NIL:
                 #ifdef DEBUG
                 fprintf(stderr, "[DEBUG EVAL] Evaluating AST_NIL.\n");
                 #endif
                 return makeNil(); // Return a Value representing nil (Type POINTER, ptr_val NULL)
        case AST_DEREFERENCE: {
#ifdef DEBUG
            fprintf(stderr, "[DEBUG EVAL] Evaluating AST_DEREFERENCE (Not Yet Implemented).\n");
#endif
            // --- Implementation needed later ---
            // 1. Evaluate node->left (should be a pointer value)
            Value ptrVal = eval(node->left);
            // 2. Check if ptrVal.type is TYPE_POINTER
            if (ptrVal.type != TYPE_POINTER) {
                fprintf(stderr, "Runtime error: Cannot dereference a non-pointer type (%s).\n", varTypeToString(ptrVal.type));
                freeValue(&ptrVal); // Free the non-pointer value
                EXIT_FAILURE_HANDLER();
            }
            // 3. Check if pointer is nil (ptrVal.ptr_val == NULL)
            if (ptrVal.ptr_val == NULL) {
                fprintf(stderr, "Runtime error: Attempted to dereference a nil pointer.\n");
                freeValue(&ptrVal);
                // Do not free ptrVal here, as it's the nil pointer itself (no heap data)
                EXIT_FAILURE_HANDLER();
            }
            // 4. Return a *copy* of the value pointed to (*ptrVal.ptr_val)
            Value dereferencedValue = makeCopyOfValue(ptrVal.ptr_val);
            // 5. Free the pointer value obtained in step 1 (it was just the address container)
            //    (No need to free ptrVal itself if it was simple nil or came from symbol table direct access,
            //     but if eval(node->left) returned a allocated copy, it needs freeing.
            //     This depends on how variable lookup returns values. Assuming lookup doesn't return heap copy here).
            // Let's skip freeing ptrVal for now, assuming it's directly from symbol or nil.
            freeValue(&ptrVal);
            return dereferencedValue;
            // --- End Implementation ---
            // return makeVoid(); // Placeholder return
        }
        case AST_FORMATTED_EXPR: {
            // Evaluate the inner expression.
            Value val = eval(node->left); // Evaluate inner expr

            int width = 0;
            int decimals = -1; // Default precision
            char format_str_debug[64] = ""; // Buffer for format info debug print

            // Extract width/precision from node->token->value
            if (node->token && node->token->value) {
                 strncpy(format_str_debug, node->token->value, sizeof(format_str_debug)-1);
                 format_str_debug[sizeof(format_str_debug)-1] = '\0';
                 sscanf(node->token->value, "%d,%d", &width, &decimals);
            } else {
                 width = 0; decimals = -1;
                 strcpy(format_str_debug, "<MISSING_TOKEN>");
                 fprintf(stderr,"Warning: Missing formatting token in AST_FORMATTED_EXPR for node %p\n", (void*)node);
            }

            // Optional Debug Print
            #ifdef DEBUG
            fprintf(stderr, "[DEBUG EVAL_FMT] START Node %p. Inner type: %s. ", (void*)node, varTypeToString(val.type));
            if(val.type == TYPE_INTEGER || val.type == TYPE_BYTE || val.type == TYPE_WORD) fprintf(stderr, "Inner i_val: %lld. ", val.i_val);
            else if(val.type == TYPE_REAL) fprintf(stderr, "Inner r_val: %f. ", val.r_val);
            else if(val.type == TYPE_CHAR) fprintf(stderr, "Inner c_val: '%c'. ", val.c_val);
            else if(val.type == TYPE_BOOLEAN) fprintf(stderr, "Inner b_val: %lld (0=f,1=t). ", val.i_val);
            else if(val.type == TYPE_STRING) fprintf(stderr, "Inner s_val: '%s'. ", val.s_val ? val.s_val : "<NULL>");
            fprintf(stderr, "Format str read: '%s'. Parsed width=%d, decimals=%d.\n", format_str_debug, width, decimals);
            #endif

            char buf[DEFAULT_STRING_CAPACITY]; // Ensure this is large enough
            buf[0] = '\0'; // Initialize buffer to empty string

            // Check the type of the value to be formatted
            if (val.type == TYPE_REAL) {
                // <<< START Correction for Real Formatting >>>
                if (decimals >= 0) { // :width:decimals uses fixed point
                    snprintf(buf, sizeof(buf), "%*.*f", width, decimals, val.r_val);
                } else { // :width uses floating point or scientific, %g is often suitable
                    // Use PASCAL_DEFAULT_FLOAT_PRECISION (already defined)
                    // Ensure width is reasonable if not specified (optional, snprintf handles width=0)
                    if (width <= 0) width = PASCAL_DEFAULT_FLOAT_PRECISION + 7; // Example default width if none given
                    snprintf(buf, sizeof(buf), "%*.*g", width, PASCAL_DEFAULT_FLOAT_PRECISION, val.r_val); // Use %g
                }
                // <<< END Correction for Real Formatting >>>
            } else if (val.type == TYPE_INTEGER || val.type == TYPE_BYTE || val.type == TYPE_WORD) {
                snprintf(buf, sizeof(buf), "%*lld", width, val.i_val);
            } else if (val.type == TYPE_STRING) {
                // <<< START Correction for String Formatting >>>
                const char* source_str = val.s_val ? val.s_val : "";
                int len = (int)strlen(source_str);
                // Determine the number of characters to actually print (precision)
                int precision = len; // Default: print whole string
                // If width is specified and positive, it dictates the max characters
                if (width > 0 && width < len) {
                    precision = width; // Truncate to width
                }
                 // If width is 0 or negative, print the whole string (precision = len)
                 // If width > len, print the whole string (precision = len), padding handled by width field

                // Use width for padding, precision for max characters printed
                snprintf(buf, sizeof(buf), "%*.*s", width, precision, source_str);
                // <<< END Correction for String Formatting >>>
            } else if (val.type == TYPE_BOOLEAN) {
                 // <<< ADDED: Boolean Formatting >>>
                 const char* bool_str = val.i_val ? "TRUE" : "FALSE";
                 int len = (int)strlen(bool_str);
                 int precision = len;
                 if (width > 0 && width < len) { // Handle potential truncation
                     precision = width;
                 }
                 // Use width for padding, precision for max characters
                 snprintf(buf, sizeof(buf), "%*.*s", width, precision, bool_str);
                 // <<< END ADDED: Boolean Formatting >>>
            } else if (val.type == TYPE_CHAR) {
                 // <<< ADDED: Char Formatting >>>
                 // %c pads with spaces according to width
                 snprintf(buf, sizeof(buf), "%*c", width, val.c_val);
                 // <<< END ADDED: Char Formatting >>>
            }
             // Add formatting for other types like ENUM if desired
            else {
                 // Default '???' for truly unhandled types
#ifdef DEBUG
                 fprintf(stderr, "[DEBUG EVAL_FMT] Unhandled inner type %s for formatting, using '?' '?\\?'\n", varTypeToString(val.type)); // Escaped trigraph
#endif
                 snprintf(buf, sizeof(buf), "%*s", width, "?" "??"); // Default output
            }

            // Optional Debug Print
            #ifdef DEBUG
            fprintf(stderr, "[DEBUG EVAL_FMT] END Node %p. snprintf result buf: '%s'\n", (void*)node, buf);
            #endif

            // Free the evaluated inner value ('val')
            freeValue(&val);

            // Create the result string Value (makeString copies buf)
            Value result = makeString(buf);

            return result; // Return the Value containing the formatted string
        } // End case AST_FORMATTED_EXPR
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
                
                freeValue(&low_val);
                freeValue(&high_val);
                
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
#ifdef DEBUG
 if(sym->type == TYPE_ENUM) {
      fprintf(stderr, "[DEBUG EVAL VAR] Symbol '%s' found. Enum Name in Symbol Table (BEFORE COPY): '%s' (addr=%p)\n",
              sym->name,
              sym->value->enum_val.enum_name ? sym->value->enum_val.enum_name : "<NULL>",
              (void*)sym->value->enum_val.enum_name);
 }
 #endif

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
                    Value result = makeCopyOfValue(&fv->value);
                    freeValue(&recVal);
                    return result;                             
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
            Value result = makeVoid(); // Initialize result placeholder

            // --- Stage 1: Determine effective types for operation dispatch ---
            // We use these 'dispatch' types to decide which block of logic to enter (integer, real, string, etc.)
            VarType dispatchLeftType = left.type;
            VarType dispatchRightType = right.type;

            // Promote Byte/Word/Bool/CHAR to Integer *for dispatching* arithmetic/comparison logic
            if (dispatchLeftType == TYPE_BYTE || dispatchLeftType == TYPE_WORD || dispatchLeftType == TYPE_BOOLEAN || dispatchLeftType == TYPE_CHAR) {
                 dispatchLeftType = TYPE_INTEGER;
            }
            if (dispatchRightType == TYPE_BYTE || dispatchRightType == TYPE_WORD || dispatchRightType == TYPE_BOOLEAN || dispatchRightType == TYPE_CHAR) {
                 dispatchRightType = TYPE_INTEGER;
            }

            // --- Stage 2: Handle Operators based on types ---

            // Handle specific operators first that have unique type rules or don't fit the general pattern easily
            // --- Handle comparisons involving POINTER or NIL ---
            // This check must come before other general type combination checks.
            if ((left.type == TYPE_POINTER || left.type == TYPE_NIL) &&
                (right.type == TYPE_POINTER || right.type == TYPE_NIL)) {
                // Both operands are pointer-like types (POINTER or NIL)
                bool comparison_result = false;

                if (op == TOKEN_EQUAL) {
                     // Check if both are NIL, OR one is POINTER (and its ptr_val is NULL) and the other is NIL,
                     // OR both are POINTER and their ptr_val are equal.
                     comparison_result = (left.type == TYPE_NIL && right.type == TYPE_NIL) ||
                                         (left.type == TYPE_POINTER && left.ptr_val == NULL && right.type == TYPE_NIL) ||
                                         (left.type == TYPE_NIL && right.type == TYPE_POINTER && right.ptr_val == NULL) ||
                                         (left.type == TYPE_POINTER && right.type == TYPE_POINTER && left.ptr_val == right.ptr_val);
                } else if (op == TOKEN_NOT_EQUAL) {
                    // Not equal is the negation of the equality check.
                    comparison_result = !((left.type == TYPE_NIL && right.type == TYPE_NIL) ||
                                          (left.type == TYPE_POINTER && left.ptr_val == NULL && right.type == TYPE_NIL) ||
                                          (left.type == TYPE_NIL && right.type == TYPE_POINTER && right.ptr_val == NULL) ||
                                          (left.type == TYPE_POINTER && right.type == TYPE_POINTER && left.ptr_val == right.ptr_val));
                } else {
                    // Other operators (<, <=, >, >=, +, -, etc.) are invalid for pointer/nil comparison
                    freeValue(&left); freeValue(&right);
                    fprintf(stderr, "Runtime error: Invalid operator '%s' for pointer or nil comparison.\n", tokenTypeToString(op));
                    EXIT_FAILURE_HANDLER();
                }

                freeValue(&left); freeValue(&right); // Free the temporary operand values
                return makeBoolean(comparison_result); // Return the boolean result as a Value
            } else if (op == TOKEN_SHL || op == TOKEN_SHR) {
                // --- End POINTER/NIL handling ---
                // Requires integer-like types. Use original types for value access.
                // Check if original types are integer-compatible (Integer, Byte, Word)
                if (!((left.type == TYPE_INTEGER || left.type == TYPE_BYTE || left.type == TYPE_WORD) &&
                      (right.type == TYPE_INTEGER || right.type == TYPE_BYTE || right.type == TYPE_WORD))) {
                     fprintf(stderr,"Runtime error: Operands for SHL/SHR must be integer types. Got %s and %s\n", varTypeToString(left.type), varTypeToString(right.type));
                     freeValue(&left); freeValue(&right); EXIT_FAILURE_HANDLER();
                 }
                 long long l_int = left.i_val; // Assumes i_val holds byte/word/int value
                 long long r_int = right.i_val;

                 // Basic check for negative shift amount
                 if (r_int < 0) {
                      fprintf(stderr, "Runtime error: Shift amount cannot be negative.\n");
                      freeValue(&left); freeValue(&right); EXIT_FAILURE_HANDLER();
                 }
                 // Note: More robust checks might involve bit width (e.g., PASCAL_INT_BITS)

                 if (op == TOKEN_SHL) result = makeInt(l_int << r_int); else result = makeInt(l_int >> r_int);
            }
            else if (op == TOKEN_IN) {
                // Uses original types: Left must be ordinal, Right must be SET
                if (right.type != TYPE_SET) { fprintf(stderr,"Runtime error: Right operand of IN must be a set. Got %s\n", varTypeToString(right.type)); freeValue(&left); freeValue(&right); EXIT_FAILURE_HANDLER(); }

                // Check if left operand is an ordinal type
                bool left_is_ordinal = (left.type==TYPE_INTEGER || left.type==TYPE_ENUM || left.type==TYPE_CHAR || (left.type==TYPE_STRING && left.s_val && strlen(left.s_val)==1) || left.type == TYPE_BYTE || left.type == TYPE_WORD || left.type == TYPE_BOOLEAN);
                if (!left_is_ordinal) { fprintf(stderr,"Runtime error: Left operand of IN must be an ordinal type. Got %s\n", varTypeToString(left.type)); freeValue(&left); freeValue(&right); EXIT_FAILURE_HANDLER(); }

                long long left_ord = 0; // Determine ordinal value based on the original type
                 if (left.type == TYPE_INTEGER || left.type == TYPE_BYTE || left.type == TYPE_WORD || left.type == TYPE_BOOLEAN) left_ord = left.i_val;
                 else if (left.type == TYPE_ENUM) left_ord = left.enum_val.ordinal;
                 else if (left.type == TYPE_CHAR) left_ord = left.c_val;
                 else if (left.type == TYPE_STRING) left_ord = left.s_val[0]; // Assumes single char string checked by left_is_ordinal

                 // Search for the ordinal value in the set
                 int found = 0;
                 if (right.set_val.set_values) {
                     for(int i=0; i < right.set_val.set_size; i++) {
                         if(right.set_val.set_values[i] == left_ord) {
                             found = 1;
                             break;
                         }
                     }
                 }
                 result = makeBoolean(found);
            }
            else if (op == TOKEN_AND || op == TOKEN_OR) {
                 // Handle bitwise for integer-like types, logical for boolean (using original types)
                 if ((left.type == TYPE_INTEGER || left.type == TYPE_BYTE || left.type == TYPE_WORD) &&
                     (right.type == TYPE_INTEGER || right.type == TYPE_BYTE || right.type == TYPE_WORD))
                 { // Bitwise operation
                       long long l_int = left.i_val; // Assumes i_val stores value
                       long long r_int = right.i_val;
                       if (op == TOKEN_AND) result = makeInt(l_int & r_int); else result = makeInt(l_int | r_int);
                 }
                 else if (left.type == TYPE_BOOLEAN && right.type == TYPE_BOOLEAN)
                 { // Logical operation for booleans
                       // Note: Pascal has short-circuit evaluation, this simple version evaluates both operands first.
                       if (op == TOKEN_AND) result = makeBoolean(left.i_val && right.i_val); else result = makeBoolean(left.i_val || right.i_val);
                 }
                 else { // Invalid combination for AND/OR
                     fprintf(stderr, "Runtime error: Invalid operands for %s. Left: %s, Right: %s\n", tokenTypeToString(op), varTypeToString(left.type), varTypeToString(right.type));
                     freeValue(&left); freeValue(&right); EXIT_FAILURE_HANDLER();
                 }
            } else if (left.type == TYPE_POINTER && right.type == TYPE_POINTER &&
                       (op == TOKEN_EQUAL || op == TOKEN_NOT_EQUAL))
                       {
               #ifdef DEBUG
               fprintf(stderr, "[DEBUG EVAL_BINARY_OP] Comparing pointers: left=%p, right=%p\n", (void*)left.ptr_val, (void*)right.ptr_val);
               #endif
               bool are_equal = (left.ptr_val == right.ptr_val);
               if (op == TOKEN_EQUAL) {
                   result = makeBoolean(are_equal);
               } else { // TOKEN_NOT_EQUAL
                   result = makeBoolean(!are_equal);
               }
                           // Result type is BOOLEAN
            }
        
            // --- Stage 3: Handle remaining operators (+, -, *, /, comparisons) using type combination checks ---
            else {
                // --- Check 1: Integer/Ordinal Operations ---
                // Use dispatch types (Byte/Word/Bool promoted to Integer)
                // Exclude real division '/'
                if (dispatchLeftType == TYPE_INTEGER && dispatchRightType == TYPE_INTEGER && op != TOKEN_SLASH) {
                    // Get values, treating bool/char as ordinals if needed for comparison/arithmetic
                    long long a = 0; // Get left value as integer/ordinal
                     if (left.type == TYPE_INTEGER || left.type == TYPE_BYTE || left.type == TYPE_WORD || left.type == TYPE_BOOLEAN) a = left.i_val;
                     else if (left.type == TYPE_CHAR) a = left.c_val;
                     else {/* This path shouldn't be reached if dispatchLeftType is INTEGER */ fprintf(stderr,"Internal error: Type mismatch in integer op block (left=%s)\n", varTypeToString(left.type)); EXIT_FAILURE_HANDLER(); }

                    long long b = 0; // Get right value as integer/ordinal
                     if (right.type == TYPE_INTEGER || right.type == TYPE_BYTE || right.type == TYPE_WORD || right.type == TYPE_BOOLEAN) b = right.i_val;
                     else if (right.type == TYPE_CHAR) b = right.c_val;
                     else {/* This path shouldn't be reached if dispatchRightType is INTEGER */ fprintf(stderr,"Internal error: Type mismatch in integer op block (right=%s)\n", varTypeToString(right.type)); EXIT_FAILURE_HANDLER(); }

                    // Perform integer arithmetic or comparison
                    switch (op) {
                        case TOKEN_PLUS: result = makeInt(a + b); break;
                        case TOKEN_MINUS: result = makeInt(a - b); break;
                        case TOKEN_MUL: result = makeInt(a * b); break;
                        case TOKEN_INT_DIV: if(b==0){fprintf(stderr,"Runtime error: Division by zero (DIV)\n"); EXIT_FAILURE_HANDLER();} result = makeInt(a / b); break;
                        case TOKEN_MOD: if(b==0){fprintf(stderr,"Runtime error: Division by zero (MOD)\n"); EXIT_FAILURE_HANDLER();} result = makeInt(a % b); break;
                        // Comparisons result in Boolean
                        case TOKEN_GREATER: result = makeBoolean(a > b); break;
                        case TOKEN_GREATER_EQUAL: result = makeBoolean(a >= b); break;
                        case TOKEN_EQUAL: result = makeBoolean(a == b); break;
                        case TOKEN_NOT_EQUAL: result = makeBoolean(a != b); break;
                        case TOKEN_LESS: result = makeBoolean(a < b); break;
                        case TOKEN_LESS_EQUAL: result = makeBoolean(a <= b); break;
                        default: fprintf(stderr,"Unhandled op %s for INTEGER/Ordinal types\n", tokenTypeToString(op)); freeValue(&left); freeValue(&right); EXIT_FAILURE_HANDLER();
                    }
                    // Result type is INTEGER or BOOLEAN
                }
                // --- Check 2: Real Number Operations ---
                // Check if REAL is involved (using original types) OR if it's real division '/'
                // Operands must be promotable to Real (Int, Byte, Word, Bool, Char)
                else if ((left.type == TYPE_REAL || dispatchLeftType == TYPE_INTEGER || left.type == TYPE_CHAR) &&
                         (right.type == TYPE_REAL || dispatchRightType == TYPE_INTEGER || right.type == TYPE_CHAR) &&
                         (left.type == TYPE_REAL || right.type == TYPE_REAL || op == TOKEN_SLASH))
                {
                     // At least one operand is REAL, OR both are integer-like/char AND operator is '/'
                     double a = 0.0, b = 0.0; // Promote values to double

                     // Promote Left Operand
                     if (left.type == TYPE_REAL) a = left.r_val;
                     else if (left.type == TYPE_INTEGER || left.type == TYPE_BYTE || left.type == TYPE_WORD || left.type == TYPE_BOOLEAN) a = (double)left.i_val;
                     else if (left.type == TYPE_CHAR) a = (double)left.c_val;
                     else { /* Invalid type for real op */ goto unsupported_operands_label; } // Should not happen based on outer condition

                     // Promote Right Operand
                     if (right.type == TYPE_REAL) b = right.r_val;
                     else if (right.type == TYPE_INTEGER || right.type == TYPE_BYTE || right.type == TYPE_WORD || right.type == TYPE_BOOLEAN) b = (double)right.i_val;
                     else if (right.type == TYPE_CHAR) b = (double)right.c_val;
                     else { /* Invalid type for real op */ goto unsupported_operands_label; } // Should not happen

                     // Perform real arithmetic or comparison
                     switch(op){
                         case TOKEN_PLUS: result=makeReal(a+b); break; case TOKEN_MINUS: result=makeReal(a-b); break;
                         case TOKEN_MUL: result=makeReal(a*b); break; case TOKEN_SLASH: if(b==0.0){fprintf(stderr,"Runtime error: Division by zero (/)\n"); EXIT_FAILURE_HANDLER();} result=makeReal(a/b); break;
                         // Comparisons result in Boolean
                         case TOKEN_GREATER: result=makeBoolean(a>b); break; case TOKEN_GREATER_EQUAL: result=makeBoolean(a>=b); break;
                         case TOKEN_EQUAL: result=makeBoolean(a==b); break; case TOKEN_NOT_EQUAL: result=makeBoolean(a!=b); break;
                         case TOKEN_LESS: result=makeBoolean(a<b); break; case TOKEN_LESS_EQUAL: result=makeBoolean(a<=b); break;
                         default: fprintf(stderr,"Unhandled op %s for REAL/Mixed types\n", tokenTypeToString(op)); freeValue(&left); freeValue(&right); EXIT_FAILURE_HANDLER();
                     }
                     // Result type is REAL or BOOLEAN
                }
                // --- Check 3: String/Char Operations ---
                // Check using original types. Must involve at least one String/Char.
                // Operator must be '+' or a comparison operator.
                else if ((left.type == TYPE_STRING || left.type == TYPE_CHAR) || (right.type == TYPE_STRING || right.type == TYPE_CHAR))
                {
                     // Ensure the other operand (if not string/char) is compatible for '+' (none) or comparison (string/char only)
                     bool types_valid_for_op = false;
                     if (op == TOKEN_PLUS && (left.type == TYPE_STRING || left.type == TYPE_CHAR) && (right.type == TYPE_STRING || right.type == TYPE_CHAR)) {
                         types_valid_for_op = true; // Concatenation
                     } else if ((op == TOKEN_EQUAL || op == TOKEN_NOT_EQUAL || op == TOKEN_LESS || op == TOKEN_LESS_EQUAL || op == TOKEN_GREATER || op == TOKEN_GREATER_EQUAL) &&
                                (left.type == TYPE_STRING || left.type == TYPE_CHAR) && (right.type == TYPE_STRING || right.type == TYPE_CHAR)) {
                         types_valid_for_op = true; // Comparison
                     }

                     if (!types_valid_for_op) {
                          goto unsupported_operands_label; // Operation not valid for these types
                     }

                     // --- Perform String/Char Operation ---
                     char tl_buf[2]={0}, tr_buf[2]={0}; // Buffers for potential char->string conversion
                     const char *ls = NULL, *rs = NULL;

                     // Get left string pointer or convert char
                     if(left.type == TYPE_CHAR) { tl_buf[0] = left.c_val; ls = tl_buf; }
                     else if (left.type == TYPE_STRING) { ls = left.s_val; }
                     else { /* Should have been caught by types_valid_for_op */ goto unsupported_operands_label; }

                     // Get right string pointer or convert char
                     if(right.type == TYPE_CHAR) { tr_buf[0] = right.c_val; rs = tr_buf; }
                     else if (right.type == TYPE_STRING) { rs = right.s_val; }
                     else { /* Should have been caught by types_valid_for_op */ goto unsupported_operands_label; }

                     // Handle NULL string pointers safely (treat as empty string)
                     if (!ls) ls = "";
                     if (!rs) rs = "";

                     if (op == TOKEN_PLUS) { // Concatenation
                         size_t len_l = strlen(ls); size_t len_r = strlen(rs);
                         size_t buf_size = len_l + len_r + 1;
                         char* concat_res = malloc(buf_size);
                         if (!concat_res) { fprintf(stderr,"Malloc failed for string concat\n"); EXIT_FAILURE_HANDLER(); }
                         strcpy(concat_res, ls); // Copy left part
                         strcat(concat_res, rs); // Append right part
                         result = makeString(concat_res); // makeString copies it again
                         free(concat_res); // Free intermediate buffer
                     }
                     else { // Comparison (already checked operator validity)
                         int cmp = strcmp(ls, rs);
                         switch (op) {
                             case TOKEN_EQUAL: result = makeBoolean(cmp == 0); break;
                             case TOKEN_NOT_EQUAL: result = makeBoolean(cmp != 0); break;
                             case TOKEN_LESS: result = makeBoolean(cmp < 0); break;
                             case TOKEN_LESS_EQUAL: result = makeBoolean(cmp <= 0); break;
                             case TOKEN_GREATER: result = makeBoolean(cmp > 0); break;
                             case TOKEN_GREATER_EQUAL: result = makeBoolean(cmp >= 0); break;
                             default: break; // Should not happen
                         }
                     }
                     // Result type is STRING or BOOLEAN
                }
                // --- Check 4: Enum/Enum Comparison ---
                // Must be the same enum type (approximated by name check), operator must be comparison.
                else if (left.type == TYPE_ENUM && right.type == TYPE_ENUM &&
                         (op == TOKEN_EQUAL || op == TOKEN_NOT_EQUAL || op == TOKEN_LESS || op == TOKEN_LESS_EQUAL || op == TOKEN_GREATER || op == TOKEN_GREATER_EQUAL))
                {
                     bool types_match = (!left.enum_val.enum_name || !right.enum_val.enum_name || strcmp(left.enum_val.enum_name, right.enum_val.enum_name)==0);
                     // Allow EQ/NE even if types mismatch, but other comparisons require match
                     if (!types_match && op != TOKEN_EQUAL && op != TOKEN_NOT_EQUAL) {
                          fprintf(stderr, "Runtime error: Cannot compare different enum types ('%s' vs '%s') with %s\n", left.enum_val.enum_name?:"?", right.enum_val.enum_name?:"?", tokenTypeToString(op));
                          freeValue(&left); freeValue(&right); EXIT_FAILURE_HANDLER();
                     }
                     int ord_l = left.enum_val.ordinal; int ord_r = right.enum_val.ordinal;
                     switch(op){
                         case TOKEN_EQUAL: result = makeBoolean(types_match && (ord_l == ord_r)); break; // Only true if types match AND ordinals match
                         case TOKEN_NOT_EQUAL: result = makeBoolean(!types_match || (ord_l != ord_r)); break; // True if types mismatch OR ordinals mismatch
                         case TOKEN_LESS: result = makeBoolean(types_match && (ord_l < ord_r)); break;
                         case TOKEN_LESS_EQUAL: result = makeBoolean(types_match && (ord_l <= ord_r)); break;
                         case TOKEN_GREATER: result = makeBoolean(types_match && (ord_l > ord_r)); break;
                         case TOKEN_GREATER_EQUAL: result = makeBoolean(types_match && (ord_l >= ord_r)); break;
                         default: break; // Should not happen
                     }
                     // Result type is BOOLEAN
                }
                // --- Check 5: Boolean/Boolean Comparison ---
                // Only allows EQ and NE. AND/OR handled earlier.
                else if (left.type == TYPE_BOOLEAN && right.type == TYPE_BOOLEAN && (op == TOKEN_EQUAL || op == TOKEN_NOT_EQUAL))
                {
                    switch(op){
                        case TOKEN_EQUAL: result = makeBoolean(left.i_val == right.i_val); break;
                        case TOKEN_NOT_EQUAL: result = makeBoolean(left.i_val != right.i_val); break;
                        default: break; // Should not happen
                    }
                    // Result type is BOOLEAN
                }
                // --- Check 6: ADDED Set Operations ---
                     else if (left.type == TYPE_SET && right.type == TYPE_SET) {
                         switch (op) {
                             case TOKEN_PLUS: // Union
                                 result = setUnion(left, right);
                                 break;
                             case TOKEN_MINUS: // Difference
                                 result = setDifference(left, right);
                                 break;
                             case TOKEN_MUL: // Intersection
                                 result = setIntersection(left, right);
                                 break;
                             // Optional: Add set comparisons like =, <>, <=, >= here if needed
                             default:
                                 fprintf(stderr, "Runtime error: Invalid operator '%s' for SET operands.\n", tokenTypeToString(op));
                                 // No need to free left/right here, happens below
                                 EXIT_FAILURE_HANDLER(); // Exit on invalid operator for sets
                         }
                         // Result type is SET (or BOOLEAN for comparisons if added)
                     }
                else {
unsupported_operands_label: // Target label for goto from other blocks if needed
                    fprintf(stderr, "Runtime error: Unsupported operand types for binary operator %s. Left: %s, Right: %s\n", tokenTypeToString(op), varTypeToString(left.type), varTypeToString(right.type));
                    freeValue(&left); freeValue(&right);
                    EXIT_FAILURE_HANDLER();
                }
            } // End else block for general operators (+, -, *, /, comparisons)

            // Free temporary operand values
            freeValue(&left);
            freeValue(&right);

            #ifdef DEBUG
            fprintf(stderr, "[DEBUG EVAL_BINARY_OP] Returning result: Type=%s", varTypeToString(result.type));
            if (result.type == TYPE_BOOLEAN || result.type == TYPE_INTEGER || result.type == TYPE_BYTE || result.type == TYPE_WORD) fprintf(stderr, ", i_val=%lld\n", result.i_val);
            else if (result.type == TYPE_REAL) fprintf(stderr, ", r_val=%f\n", result.r_val);
            else if (result.type == TYPE_CHAR) fprintf(stderr, ", c_val='%c'\n", result.c_val);
            else fprintf(stderr, "\n");
            #endif
            return result;

        } // End Case AST_BINARY_OP
        case AST_SET: // Add case to handle set evaluation
            return evalSet(node);
        case AST_UNARY_OP: {
            Value val = eval(node->left); // 'val' might contain allocated data
            Value result; // Temporary to hold the new value before returning

            if (node->token->type == TOKEN_PLUS) {
                // 'val' is being returned directly. Ownership is transferred.
                // No freeValue(&val) needed here.
                return val;
            } else if (node->token->type == TOKEN_MINUS) {
                if (val.type == TYPE_INTEGER) {
                    result = makeInt(-val.i_val);
                } else { // Assuming other numeric types (like REAL)
                    result = makeReal(-val.r_val);
                }
                // Now that we've used val.i_val or val.r_val to create 'result',
                // we must free the original 'val'.
                freeValue(&val);
                return result;
            } else if (node->token->type == TOKEN_NOT) {
                // Assuming val.i_val is used for boolean logic (0 for false, non-zero for true)
                result = makeBoolean(val.i_val == 0);
                // Free the original 'val' after creating 'result'.
                freeValue(&val);
                return result;
            }
            
            // If the token type was not PLUS, MINUS, or NOT (should ideally be caught by parser as an error)
            // and we reach this 'break', 'val' would be leaked.
            // To be safe, ensure 'val' is freed if none of the above conditions returned.
            freeValue(&val);
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
        Value low = eval(label->left); // <<< Memory might be allocated here
        Value high = eval(label->right); // <<< Memory might be allocated here

        // ADDED: Variables to hold comparison result
        bool match = false;

        // Allow Integer range
        if (caseVal.type == TYPE_INTEGER && low.type == TYPE_INTEGER && high.type == TYPE_INTEGER) {
            match = caseVal.i_val >= low.i_val && caseVal.i_val <= high.i_val;
        }
        // Allow Char range (compare char values directly)
        else if (caseVal.type == TYPE_CHAR && low.type == TYPE_CHAR && high.type == TYPE_CHAR) {
            match = caseVal.c_val >= low.c_val && caseVal.c_val <= high.c_val;
        }
        // Add other ordinal range checks if needed (e.g., Byte, Word)

        // Incompatible types for range check
        // return false; // Replaced by setting match = false

        // ADDED: Free the evaluated values for the bounds
        freeValue(&low);
        freeValue(&high);

        return match; // ADDED: Return the determined match result

    } else { // Single label comparison
        Value labelVal = eval(label); // Evaluate the label (e.g., dirRight, 'A', 5)

        // ADDED: Variable to hold comparison result
        bool match = false;

        // Compare based on the type of the case expression value
        switch (caseVal.type) {
            case TYPE_ENUM:
                // Case expression is ENUM, label must also be ENUM
                if (labelVal.type == TYPE_ENUM) {
                    // Optional: Add stricter check using enum_name if desired:
                    // if (caseVal.enum_val.enum_name && labelVal.enum_val.enum_name &&
                    //     strcmp(caseVal.enum_val.enum_name, labelVal.enum_val.enum_name) == 0)
                    match = caseVal.enum_val.ordinal == labelVal.enum_val.ordinal; // ADDED: Store result
                    // else return false; // Replaced by setting match = false
                }
                break; // Enum vs non-enum fails

            case TYPE_INTEGER:
            case TYPE_WORD: // Treat Byte/Word as integers for case
                    // Case expression is Integer-like, label must be Integer-like
                 if (labelVal.type == TYPE_INTEGER || labelVal.type == TYPE_BYTE || labelVal.type == TYPE_WORD) {
                    match = caseVal.i_val == labelVal.i_val; // ADDED: Store result
                 } else if (labelVal.type == TYPE_CHAR) {
                     // Allow matching Integer Case value against a Char label (using Ord(char))
                      match = caseVal.i_val == (long long)labelVal.c_val; // ADDED: Store result
                 }
                 break; // Integer vs other types fails
            case TYPE_CHAR:
                 // Case expression is CHAR
                 if (labelVal.type == TYPE_CHAR) {
                     // Label is CHAR - Direct comparison
                     match = caseVal.c_val == labelVal.c_val; // ADDED: Store result
                 } else if (labelVal.type == TYPE_STRING && labelVal.s_val && strlen(labelVal.s_val) == 1) {
                     // <<< FIX: Label is single-char STRING - Compare case char to string's first char
                     match = caseVal.c_val == labelVal.s_val[0]; // ADDED: Store result
                 } else if (labelVal.type == TYPE_INTEGER) {
                     // Allow matching Char Case value against an Integer label (Ordinal match)
                     match = (long long)caseVal.c_val == labelVal.i_val; // ADDED: Store result
                 }
                 break; // Char vs other types fails
                 case TYPE_BOOLEAN:
                 // Case expression is BOOLEAN, label must be BOOLEAN
                 if (labelVal.type == TYPE_BOOLEAN) {
                     match = caseVal.i_val == labelVal.i_val; // Booleans use i_val (0/1) // ADDED: Store result
                 }
             break; // Boolean vs non-boolean fails

            // Add other ordinal types if needed
            default:
                // Non-ordinal type used in case expression - error or return false
                // fprintf(stderr, "Warning: Non-ordinal type %s used in CASE expression.\n", varTypeToString(caseVal.type));
                match = false; // ADDED: Store result
        }

        freeValue(&labelVal);

        // If we fall through, types were incompatible
        return match; // ADDED: Return the determined match result
     }
}

static void processDeclarations(AST *decl, bool is_global_block) {
    // Check if decl is a valid COMPOUND node containing declarations
    if (!decl || decl->type != AST_COMPOUND) {
         if (decl && decl->type != AST_NOOP) { /* Warning or ignore */ }
         return; // Nothing to process
     }

    // Iterate through all top-level declaration nodes (VAR_DECL, CONST_DECL, TYPE_DECL...)
    for (int i = 0; i < decl->child_count; i++) {
        AST *d = decl->children[i]; // d is a VAR_DECL group, CONST_DECL, TYPE_DECL etc.
        if (!d) continue;

        // Handle VAR_DECL specifically
        if (d->type == AST_VAR_DECL) {
            // d->child_count > 0 holds the VARIABLE nodes (names)
            // d->right holds the type definition node (e.g., TYPE_REFERENCE to PInt)
            // d->var_type holds the resolved VarType enum (e.g., TYPE_POINTER)

            for (int j = 0; j < d->child_count; j++) {
                AST *varNode = d->children[j]; // The AST_VARIABLE node for the name (e.g., p1)
                if (!varNode || !varNode->token) continue; // Skip invalid variable nodes

                const char *varname = varNode->token->value; // e.g., "p1"
                VarType varType = d->var_type;         // e.g., TYPE_POINTER
                AST* typeDefNode = d->right;           // e.g., TYPE_REFERENCE("pint")

                // --- Insert the symbol ---
                // insertGlobal/LocalSymbol now handles allocation AND initialization via makeValueForType
                if (is_global_block) {
                    insertGlobalSymbol(varname, varType, typeDefNode);
                    #ifdef DEBUG
                    // Keep this print if desired
                    // fprintf(stderr, "[DEBUG] processDeclarations: Called insertGlobalSymbol('%s', type=%s)\n", varname, varTypeToString(varType));
                    #endif
                } else {
                    insertLocalSymbol(varname, varType, typeDefNode, true); // true indicates it's a local variable
                     #ifdef DEBUG
                    // fprintf(stderr, "[DEBUG] processDeclarations: Called insertLocalSymbol('%s', type=%s)\n", varname, varTypeToString(varType));
                    #endif
                }

                // --- REMOVED Redundant/Harmful Initialization Block ---
                // The symbol's value (including base_type_node for pointers)
                // is now fully initialized by insertGlobal/LocalSymbol -> makeValueForType.
                // No further initialization is needed here.

            } // End loop over variable names (j)
        } // End if VAR_DECL
        else if (d->type == AST_CONST_DECL) {
             // Constant evaluation and insertion happens during parsing (declarations function)
             // No processing needed here during execution walk?
             // If constants needed re-evaluation for locals, it would go here.
        }
        else if (d->type == AST_TYPE_DECL) {
            // Type declarations are handled during parsing (insertType)
            // No processing needed here during execution walk.
        }
        // Add handling for nested PROCEDURE/FUNCTION declarations if/when implemented
        else if (d->type == AST_PROCEDURE_DECL || d->type == AST_FUNCTION_DECL) {
             // Add to procedure table if not already done by parser?
             // Or handle scoping for nested calls if implemented.
             #ifdef DEBUG
             fprintf(stderr, "[DEBUG processDeclarations] Skipping nested procedure/function '%s'\n", d->token ? d->token->value : "?");
             #endif
        }
    } // End loop over declarations (i)
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
            executeWithScope(node->right, true);
            break;
        case AST_ASSIGN: {
            // 1. Evaluate Right-Hand Side
            Value rhs_val = eval(node->right);
            AST* lhsNode = node->left;
            Value* target_ptr = NULL; // Pointer to the Value to be modified
            const char* lhs_name = NULL; // Name if simple variable

            // --- Determine Assignment Type ---
            bool is_simple_var_assign = (lhsNode->type == AST_VARIABLE && lhsNode->token);
            bool is_string_index_assign = false;
            bool is_complex_lvalue_assign = false;

            if (!is_simple_var_assign) {
                // For complex LValues, we need the target pointer first to check for string index
                target_ptr = resolveLValueToPtr(lhsNode); // Find memory location for array[i], record.f, p^ etc.
                if (!target_ptr) { /* Error handled in resolve */ freeValue(&rhs_val); EXIT_FAILURE_HANDLER(); }

                // Check if it's specifically a string index assignment
                if (lhsNode->type == AST_ARRAY_ACCESS && target_ptr->type == TYPE_STRING && lhsNode->child_count == 1) {
                    is_string_index_assign = true;
                } else {
                    // It's another complex LValue (array element, record field, dereferenced pointer)
                    is_complex_lvalue_assign = true;
                }
            }

            // --- Perform Assignment Based on Type ---

            if (is_simple_var_assign) {
                // --- Case 1: Simple Variable Assignment (e.g., p1 := nil, word := 'abc') ---
                lhs_name = lhsNode->token->value;
                #ifdef DEBUG
                fprintf(stderr, "[DEBUG ASSIGN] Calling updateSymbol for simple LHS '%s'\n", lhs_name);
                #endif
                updateSymbol(lhs_name, rhs_val);
                freeValue(&rhs_val);
                // updateSymbol handles freeing old value, type checking, deep copies, and freeing rhs_val if consumed

            } else if (is_string_index_assign) {
                // --- Case 2: String Index Assignment (e.g., s[i] := 'c') ---
                #ifdef DEBUG
                fprintf(stderr, "[DEBUG ASSIGN STR_IDX] Handling String Index Assignment (LHS Base Type: %s).\n",
                        varTypeToString(target_ptr->type));
                #endif
                // target_ptr points to the base string's Value struct (from resolveLValueToPtr)
                AST* index_node = lhsNode->children[0];
                Value index_val = eval(index_node);
                if (index_val.type != TYPE_INTEGER) { /* Error */ freeValue(&rhs_val); freeValue(&index_val); EXIT_FAILURE_HANDLER(); }
                long long idx_ll = index_val.i_val;
                freeValue(&index_val);

                if (rhs_val.type != TYPE_CHAR && !(rhs_val.type == TYPE_STRING && rhs_val.s_val && strlen(rhs_val.s_val) == 1)) { /* Error */ freeValue(&rhs_val); EXIT_FAILURE_HANDLER(); }
                char char_to_assign = (rhs_val.type == TYPE_CHAR) ? rhs_val.c_val : rhs_val.s_val[0];

                size_t len = target_ptr->s_val ? strlen(target_ptr->s_val) : 0;
                if (idx_ll < 1 || (size_t)idx_ll > len) { /* Bounds Error */ freeValue(&rhs_val); EXIT_FAILURE_HANDLER(); }
                size_t idx_0based = (size_t)idx_ll - 1;

                if (target_ptr->s_val) {
                    target_ptr->s_val[idx_0based] = char_to_assign;
                    // Ensure type remains string (should already be, but belt-and-suspenders)
                    target_ptr->type = TYPE_STRING;
                } else { /* Error: trying to index NULL string */ }

                // Free RHS value only (target string buffer was modified in place)
                freeValue(&rhs_val);

            } else if (is_complex_lvalue_assign) {
                // --- Case 3: Other Complex LValue Assignments (Array[i], Record.F, Ptr^) ---
                #ifdef DEBUG
                fprintf(stderr, "[DEBUG ASSIGN COMPLEX] LHS AST Node Type: %s, Resolved Target Value Type: %s, RHS Value Type: %s\n",
                         astTypeToString(lhsNode->type), varTypeToString(target_ptr->type), varTypeToString(rhs_val.type));
                #endif

                VarType lhs_effective_type = target_ptr->type;
                VarType rhs_effective_type = rhs_val.type;

                if (lhs_effective_type == TYPE_BYTE && rhs_effective_type == TYPE_INTEGER) {
                    // Assigning INTEGER to BYTE target
                    long long val_to_assign = rhs_val.i_val;
                    if (val_to_assign < 0 || val_to_assign > 255) {
                        // This warning is good for catching Pscal logic errors if any.
                        // The Pscal Mandelbrot code with ((X MOD 256 + 256) MOD 256) should prevent this.
                        fprintf(stderr, "Warning: Overflow assigning INTEGER %lld to BYTE target. Value will be truncated/wrapped.\n", val_to_assign);
                    }
                    // For simple types like BYTE, we are not freeing sub-components of target_ptr
                    // before overwriting i_val. We just directly assign.
                    target_ptr->i_val = (val_to_assign & 0xFF); // Store the lower 8 bits
                    target_ptr->type = TYPE_BYTE; // Ensure the target's type field remains BYTE
                }
                else if (lhs_effective_type == TYPE_WORD && rhs_effective_type == TYPE_INTEGER) {
                    // Assigning INTEGER to WORD target
                    long long val_to_assign = rhs_val.i_val;
                    if (val_to_assign < 0 || val_to_assign > 65535) {
                         fprintf(stderr, "Warning: Overflow assigning INTEGER %lld to WORD target. Value will be truncated/wrapped.\n", val_to_assign);
                    }
                    target_ptr->i_val = (val_to_assign & 0xFFFF);
                    target_ptr->type = TYPE_WORD;
                }
                else if (lhs_effective_type == TYPE_REAL && rhs_effective_type == TYPE_INTEGER) {
                    // Assigning INTEGER to REAL target: Promote
                    target_ptr->r_val = (double)rhs_val.i_val;
                    target_ptr->type = TYPE_REAL;
                }
                else if (lhs_effective_type == TYPE_POINTER && rhs_effective_type == TYPE_POINTER) {
                    // Pointer assignment: shallow copy of address and base type node if compatible
                    // The 'type_def' of the LHS pointer variable determines its fundamental base type.
                    // If assigning nil, rhs_val.base_type_node might be NULL.
                    // If assigning another pointer, their base_type_nodes should ideally be compatible
                    // (a check for this is more advanced type system work).
                    // For now, copy the pointer value and retain the LHS's original base_type_node
                    // unless the RHS is a non-nil pointer with a more specific (or compatible) base type.
                    // This area can get complex with strict type checking for pointers.
                    // A simple address copy is often what's done, assuming Pscal type checker handled compatibility.
                    AST* original_lhs_base_type_node = target_ptr->base_type_node; // Preserve original type info
                    
                    target_ptr->ptr_val = rhs_val.ptr_val; // Copy the address
                    // If RHS is nil, its base_type_node is likely NULL.
                    // If RHS is not nil, it carries its own base_type_node.
                    // We should generally keep the LHS's declared base_type_node.
                    target_ptr->base_type_node = original_lhs_base_type_node;
                    target_ptr->type = TYPE_POINTER;
                }
                else if (lhs_effective_type == rhs_effective_type) {
                    // Types are the same: perform a deep copy.
                    // This handles Array := Array, Record := Record, String := String.
                    freeValue(target_ptr); // Free existing contents of the target Value struct
                    *target_ptr = makeCopyOfValue(&rhs_val); // Assign a deep copy of the new value
                    // makeCopyOfValue should preserve the type, but ensure it:
                    target_ptr->type = lhs_effective_type;
                }
                else {
                    // Incompatible types for complex assignment
                    fprintf(stderr, "Runtime error: Type mismatch for complex assignment (LHS target effective type: %s, RHS actual type: %s) for LValue AST node type %s.\n",
                             varTypeToString(lhs_effective_type), varTypeToString(rhs_effective_type), astTypeToString(lhsNode->type));
                    freeValue(&rhs_val); // Free RHS as it won't be used
                    EXIT_FAILURE_HANDLER();
                }

                // Free the temporary RHS value that was evaluated, as its data
                // has either been directly used (for simple types) or deep-copied.
                freeValue(&rhs_val);

            } else { // Should not happen if logic above is correct (i.e. not simple, not string_index, not complex_lvalue)
                 fprintf(stderr, "Internal Error: Unhandled assignment case in executeWithScope.\n");
                 freeValue(&rhs_val); // Cleanup RHS
                 EXIT_FAILURE_HANDLER();
            }

            break; // End case AST_ASSIGN
        }

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
            
                   freeValue(&caseValue);
            
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
            
            freeValue(&cond);
            
            if (is_true)
                executeWithScope(node->right, false);
            else if (node->extra)
                executeWithScope(node->extra, false);
            break;
        }
        case AST_WHILE: {
             while (1) {
                 Value cond = eval(node->left); // Evaluate condition (x < 3)

                 #ifdef DEBUG
                 fprintf(stderr, "[DEBUG WHILE] Condition eval result: Type=%s", varTypeToString(cond.type));
                 if (cond.type == TYPE_BOOLEAN || cond.type == TYPE_INTEGER) {
                     fprintf(stderr, ", i_val=%lld\n", cond.i_val);
                 } else if (cond.type == TYPE_REAL) {
                     fprintf(stderr, ", r_val=%f\n", cond.r_val);
                 } else {
                     fprintf(stderr, "\n");
                 }
                 #endif

                 int is_true;
                 if (cond.type == TYPE_REAL) {
                     is_true = (cond.r_val != 0.0);
                      #ifdef DEBUG
                      fprintf(stderr, "[DEBUG WHILE] Condition type is REAL. is_true set to %d (based on r_val!=0.0)\n", is_true);
                      #endif
                 } else {
                     // Assume INTEGER or BOOLEAN (or other compatible ordinal)
                     is_true = (cond.i_val != 0);
                      #ifdef DEBUG
                      fprintf(stderr, "[DEBUG WHILE] Condition type is %s. is_true set to %d (based on i_val!=0)\n", varTypeToString(cond.type), is_true);
                      #endif
                 }
                 
                 freeValue(&cond);

                 if (!is_true) {
                      #ifdef DEBUG
                      fprintf(stderr, "[DEBUG WHILE] Condition resulted in FALSE. Breaking loop.\n");
                      #endif
                      break; // Exit WHILE loop normally if condition is false
                 }

                 // Body execution should happen here...
                  #ifdef DEBUG
                  fprintf(stderr, "[DEBUG WHILE] Condition TRUE. Executing body...\n");
                  #endif
                 break_requested = 0; // Reset break flag *before* executing body
                 executeWithScope(node->right, false); // Execute body
                 if (break_requested) {
                      #ifdef DEBUG
                      fprintf(stderr, "[DEBUG WHILE] Break requested inside loop body. Exiting loop.\n");
                      #endif
                      break; // Exit the C 'while' loop due to break statement
                 }
             } // End C while(1) loop
             break_requested = 0; // Ensure flag is reset after loop exits
             break; // Break from the switch case for AST_WHILE
         } // End case AST_WHILE
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
                
                freeValue(&cond);
                
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
            
            // Make sure to free things that need to be free
            freeValue(&start_val);
            freeValue(&end_val);
            
            break; // Break from the switch case for AST_FOR_TO/DOWNTO
        }

        case AST_WRITE:
        case AST_WRITELN: {
            FILE *output = stdout; // Default to console
            int startIndex = 0;     // Start processing args from index 0 by default
            Value fileVal;          // To hold evaluated file variable if present
            bool isFileOp = false;  // Flag to track if writing to file
            bool colorWasSet = false; // <<< ADD Flag

            // --- START: Original File Variable Check Logic ---
            // Check if the first argument *might* be a file variable
            if (node->child_count > 0 && node->children[0] != NULL) { // Added NULL check for child node
                 // Check if it *looks* like a variable that could hold a file
                 if (node->children[0]->type == AST_VARIABLE) {
                     // Evaluate the first argument ONLY to check its type and get f_val
                     fileVal = eval(node->children[0]); // Evaluate children[0]
                     if (fileVal.type == TYPE_FILE) {
                         if (fileVal.f_val != NULL) { // Check if file is open
                            output = fileVal.f_val; // <<< Use f_val from the evaluated Value
                            startIndex = 1;         // <<< Skip file var when printing args
                            isFileOp = true;
#ifdef DEBUG
                            fprintf(stderr, "[DEBUG WRITE] Detected File Operation. Target FILE*: %p\n", (void*)output);
#endif
                         } else {
                            // File variable provided but not open - Error or Warning?
                            // Standard Pascal might raise an IO error here.
                            fprintf(stderr, "Runtime Warning: File variable passed to write(ln) is not open.\n");
                            // Keep output = stdout, startIndex = 0 - try to print all args to console?
                            // Or maybe skip output entirely? Let's print all to console for now.
                            isFileOp = false; // Treat as console op despite file var
                            freeValue(&fileVal); // Free the temporary fileVal (since we're not using its f_val)
                         }
                     } else {
                         // First arg wasn't TYPE_FILE, free the temp value from eval
                         freeValue(&fileVal);
                     }
                 }
                 // If first arg is not AST_VARIABLE, it cannot be a file variable, proceed with console output
            }

            // --- Apply ANSI color codes ONLY if writing to stdout ---
            if (!isFileOp) { // <<< Only apply colors if output is stdout
#ifdef DEBUG
                fprintf(stderr, "<< Write Handler Start (stdout): Reading FG=%d, Ext=%d, BG=%d, BGExt=%d, Bold=%d\n",
                        gCurrentTextColor, gCurrentColorIsExt,
                        gCurrentTextBackground, gCurrentBgIsExt,
                        gCurrentTextBold);
                fflush(stderr);
#endif
                char escape_sequence[64] = "\x1B[";
                char code_str[10];
                bool first_attr = true;

                // 1. Handle Bold/Intensity
                if (!gCurrentColorIsExt && gCurrentTextBold) { strcat(escape_sequence, "1"); first_attr = false; }
                // 2. Handle Foreground Color
                if (!first_attr) strcat(escape_sequence, ";");
                if (gCurrentColorIsExt) { snprintf(code_str, sizeof(code_str), "38;5;%d", gCurrentTextColor); }
                else { snprintf(code_str, sizeof(code_str), "%d", map16FgColorToAnsi(gCurrentTextColor, gCurrentTextBold)); }
                strcat(escape_sequence, code_str);
                first_attr = false;
                // 3. Handle Background Color
                strcat(escape_sequence, ";");
                if (gCurrentBgIsExt) { snprintf(code_str, sizeof(code_str), "48;5;%d", gCurrentTextBackground); }
                else { snprintf(code_str, sizeof(code_str), "%d", map16BgColorToAnsi(gCurrentTextBackground)); }
                strcat(escape_sequence, code_str);
                // 4. Terminate sequence
                strcat(escape_sequence, "m");
                // 5. Print sequence to stdout
                printf("%s", escape_sequence);
                fflush(stdout); // Flush color code immediately might help
                colorWasSet = true; // <<< SET Flag: Colors were applied
            } // End if (!isFileOp) for colors

            // --- Loop through arguments to print (start index adjusted for file ops) ---
            for (int i = startIndex; i < node->child_count; i++) {
                AST *argNode = node->children[i];
                if (!argNode) continue;
                Value val = eval(argNode); // Evaluate data argument

                // Use 'output' (either stdout or the file pointer)
                // (Keep the existing printing logic based on val.type using fprintf/fputc)
                 if (argNode->type == AST_FORMATTED_EXPR) {
                     // Print formatted string contained in 'val' (eval handles formatting)
                     if (val.type == TYPE_STRING) fprintf(output, "%s", val.s_val ? val.s_val : "");
                     else fprintf(output, "[formatted_eval_error]");
                 } else { // Standard printing
                     if (val.type == TYPE_INTEGER) fprintf(output, "%lld", val.i_val);
                     else if (val.type == TYPE_REAL) fprintf(output, "%f", val.r_val);
                     else if (val.type == TYPE_BOOLEAN) fprintf(output, "%s", (val.i_val != 0) ? "true" : "false");
                     else if (val.type == TYPE_STRING) fprintf(output, "%s", val.s_val ? val.s_val : "");
                     else if (val.type == TYPE_CHAR) fputc(val.c_val, output);
                     else if (val.type == TYPE_ENUM) fprintf(output, "%s", val.enum_val.enum_name ? val.enum_val.enum_name : "?");
                     // Do not print file vars or other unprintables when writing data
                     else if (val.type != TYPE_FILE) fprintf(output, "[unprintable_type_%d]", val.type);
                 }
                 freeValue(&val); // Free the evaluated data argument
            } // End loop through data arguments

            // Handle WriteLn vs Write
            if (node->type == AST_WRITELN) {
                fprintf(output, "\n"); // Add newline to the correct output stream
            }
            
            if (colorWasSet) { // <<< Check Flag
                printf("\x1B[0m"); // ANSI Reset Code
                // colorWasSet = false; // Optional: Reset flag
            }

            fflush(output); // Flush the output stream (stdout or file)

            // --- Crucial: Free the fileVal *if* it was evaluated and not used ---
            // (This was handled inside the file check logic now)

            break;
        } // End case AST_WRITE/AST_WRITELN
            
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
        case AST_PROCEDURE_CALL: {
            // Execute the procedure or function call
            Value call_result = executeProcedureCall(node); // Capture the returned Value

            // Free the Value returned by executeProcedureCall.
            // If it was a function, this frees the copied result (e.g., its s_val).
            // If it was a procedure, 'call_result' would be TYPE_VOID,
            // and freeValue on a TYPE_VOID Value is a safe no-op for its data.
            freeValue(&call_result);
            break;
        }
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
            // Perform deep copy for the enum name string
            v.enum_val.enum_name = src->enum_val.enum_name ? strdup(src->enum_val.enum_name) : NULL;
            if (src->enum_val.enum_name && !v.enum_val.enum_name) {
                 fprintf(stderr, "Memory allocation failed in makeCopyOfValue (enum name strdup)\n");
                 EXIT_FAILURE_HANDLER();
            }
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
            // <<< ADD/VERIFY THIS >>>
            v.dimensions = src->dimensions;
            // <<< END ADD/VERIFY >>>

            // Check if dimensions > 0 before mallocing bounds/data
            if (v.dimensions > 0 && src->lower_bounds && src->upper_bounds) {
                v.lower_bounds = malloc(sizeof(int) * src->dimensions);
                v.upper_bounds = malloc(sizeof(int) * src->dimensions);
                // Add null checks for malloc results
                if (!v.lower_bounds || !v.upper_bounds) { /* Handle error */ }

                for (int i = 0; i < src->dimensions; i++) {
                    v.lower_bounds[i] = src->lower_bounds[i];
                    v.upper_bounds[i] = src->upper_bounds[i];
                    // Check for valid bounds before calculating total_size
                    int size_i = (v.upper_bounds[i] - v.lower_bounds[i] + 1);
                    if (size_i <= 0) { total = 0; break; } // Invalid bounds -> empty array effectively
                    if (__builtin_mul_overflow(total, size_i, &total)) { total = -1; break; } // Check overflow
                }
            } else {
                // If dimensions is 0 or bounds are missing, ensure size is 0
                total = 0;
                v.lower_bounds = NULL;
                v.upper_bounds = NULL;
            }

            // Allocate and copy array data only if total size is valid
            v.array_val = NULL; // Initialize to NULL
            if (total > 0 && src->array_val) {
                 v.array_val = malloc(sizeof(Value) * total);
                 if (!v.array_val) { /* Handle error */ }
                 for (int i = 0; i < total; i++) {
                     v.array_val[i] = makeCopyOfValue(&src->array_val[i]);
                 }
            } else if (total < 0) {
                 fprintf(stderr, "Error: Array size overflow during copy.\n");
                 // Handle error - maybe set dimensions to 0?
                 v.dimensions = 0;
                 free(v.lower_bounds); v.lower_bounds = NULL;
                 free(v.upper_bounds); v.upper_bounds = NULL;
            } else {
                 // total is 0, ensure array_val is NULL (already done)
            }

            // Copy element type definition link
            v.element_type_def = src->element_type_def; // Assuming shallow copy is ok for AST node link
            v.element_type = src->element_type; // Copy element type enum

            break;
        } // End case TYPE_ARRAY
        case TYPE_CHAR:
            // Already handled by shallow copy
            break;
        case TYPE_SET:
            // Deep copy the set elements
            v.set_val.set_values = NULL; // Initialize destination pointer
            v.set_val.set_size = 0;      // Initialize destination size
            // v.max_length = 0; // Reset capacity tracking if you use it

            if (src->set_val.set_size > 0 && src->set_val.set_values != NULL) {
                size_t array_size_bytes = sizeof(long long) * src->set_val.set_size;
                v.set_val.set_values = malloc(array_size_bytes);
                if (!v.set_val.set_values) { 
                    freeValue(&v);
                    fprintf(stderr,
                            "Memory allocation failed in makeCopyOfValue (set)\n");
                    EXIT_FAILURE_HANDLER();
                }
                if (!v.set_val.set_values) {
                    fprintf(stderr, "Memory allocation failed in makeCopyOfValue (set values)\n");
                    // Potentially free other copied parts before exiting if necessary
                    EXIT_FAILURE_HANDLER();
                }
                memcpy(v.set_val.set_values, src->set_val.set_values, array_size_bytes);
                v.set_val.set_size = src->set_val.set_size;
                 // Optionally copy capacity if tracked: v.max_length = src->max_length;
            }
            // If source size is 0 or values pointer is NULL, destination remains empty (NULL/0)
            break;
        default:
            // ints, bools, reals etc. are already safely copied by value
            break;
    }

    return v;
}

Value* resolveLValueToPtr(AST* lvalueNode) {
    if (!lvalueNode) {
        fprintf(stderr, "Runtime error: Cannot resolve NULL lvalue node.\n");
        EXIT_FAILURE_HANDLER();
    }

    switch (lvalueNode->type) {
        case AST_VARIABLE: {
            Symbol* sym = lookupSymbol(lvalueNode->token->value); // Handles not found error
            if (sym->is_const) {
                 fprintf(stderr, "Runtime error: Cannot modify constant symbol '%s'.\n", sym->name);
                 EXIT_FAILURE_HANDLER();
            }
            if (!sym->value) {
                 fprintf(stderr, "Runtime error: Symbol '%s' has NULL value pointer.\n", sym->name);
                 EXIT_FAILURE_HANDLER();
            }
            return sym->value; // Return pointer to the symbol's value storage
        }

        case AST_ARRAY_ACCESS: {
            // Recursively resolve the base of the array access (e.g., 'myArray' or 'myRecord.myArray')
            Value* baseValuePtr = resolveLValueToPtr(lvalueNode->left);
            if (!baseValuePtr) { /* Error handled in recursive call */ EXIT_FAILURE_HANDLER(); }

            // Check if the resolved base is actually an array or string
            if (baseValuePtr->type == TYPE_ARRAY) {
                // --- Array Element Access ---
                if (!baseValuePtr->array_val) { /* Error: Array not initialized */ }
#ifdef DEBUG
fprintf(stderr, "[DEBUG RESOLVE_LVAL] Array Access Check:\n");
fprintf(stderr, "  lvalueNode->child_count = %d\n", lvalueNode->child_count);
fprintf(stderr, "  baseValuePtr->dimensions = %d\n", baseValuePtr->dimensions);
fflush(stderr); // Ensure output is seen before potential crash
#endif
                if (lvalueNode->child_count != baseValuePtr->dimensions) { /* Error: Index count mismatch */ }

                // Calculate indices
                int* indices = malloc(sizeof(int) * baseValuePtr->dimensions);
                if (!indices) { /* Mem Error */ }
                for (int i = 0; i < lvalueNode->child_count; i++) {
                    assert(i < baseValuePtr->dimensions);
                    Value idxVal = eval(lvalueNode->children[i]); // Eval index expressions
                    if (idxVal.type != TYPE_INTEGER) { /* Index Type Error */ free(indices); freeValue(&idxVal); EXIT_FAILURE_HANDLER(); }
                    indices[i] = (int)idxVal.i_val;
                    freeValue(&idxVal); // Free temp index value
                }

                // Calculate offset and check bounds
                int offset = computeFlatOffset(baseValuePtr, indices);
                int total_size = 1;
                for (int i = 0; i < baseValuePtr->dimensions; i++) { total_size *= (baseValuePtr->upper_bounds[i] - baseValuePtr->lower_bounds[i] + 1); }
                 if (offset < 0 || offset >= total_size) { /* Bounds Error */ free(indices); EXIT_FAILURE_HANDLER(); }

                free(indices); // Free indices array after calculating offset

                // Return pointer to the element within the array's value storage
                return &(baseValuePtr->array_val[offset]);

            } else if (baseValuePtr->type == TYPE_STRING) {
                // --- String Character Access ---
                // For L-Value resolution, we return a pointer to the *base string's Value* struct.
                // The caller (AST_ASSIGN) MUST detect this specific case and handle modification.
                #ifdef DEBUG
                const char* base_name = lvalueNode->left->token ? lvalueNode->left->token->value : "?"; // Get base var name for debug
                fprintf(stderr, "[DEBUG RESOLVE_LVAL] String index access LValue ('%s[?]'). Returning pointer to base string Value* %p\n",
                        base_name, (void*)baseValuePtr);
                #endif
                // NOTE: We don't evaluate or check the index here. The caller must do it.
                return baseValuePtr; // <<< RETURN POINTER TO BASE STRING VALUE >>> // Modified Line
            } else {
                fprintf(stderr, "Runtime error: Attempted array/string access on non-array/string type (%s).\n", varTypeToString(baseValuePtr->type));
                EXIT_FAILURE_HANDLER();
            }
            break; // Keep compiler happy
        }

        case AST_FIELD_ACCESS: {
            // Recursively resolve the base of the field access (e.g., 'myRecord' or 'myArray[i]')
            Value* baseValuePtr = resolveLValueToPtr(lvalueNode->left);
             if (!baseValuePtr) { /* Error handled in recursive call */ EXIT_FAILURE_HANDLER(); }

            // Check if the resolved base is actually a record
            if (baseValuePtr->type != TYPE_RECORD) {
                 fprintf(stderr, "Runtime error: Field access on non-record type (%s).\n", varTypeToString(baseValuePtr->type));
                 EXIT_FAILURE_HANDLER();
            }
             if (!baseValuePtr->record_val) {
                  fprintf(stderr, "Runtime error: Record accessed before initialization or after being freed.\n");
                  EXIT_FAILURE_HANDLER();
             }

            // Find the specific field by name
            const char* targetFieldName = lvalueNode->token ? lvalueNode->token->value : NULL;
            if (!targetFieldName) { /* Invalid AST node error */ }

            FieldValue* currentField = baseValuePtr->record_val;
            while (currentField) {
                if (currentField->name && strcmp(currentField->name, targetFieldName) == 0) {
                    // Found the field, return pointer to its Value struct
                    return &(currentField->value);
                }
                currentField = currentField->next;
            }

            // Field not found
            fprintf(stderr, "Runtime error: Field '%s' not found in record.\n", targetFieldName);
            EXIT_FAILURE_HANDLER();
            break; // Keep compiler happy
        }
        case AST_DEREFERENCE: {
            #ifdef DEBUG
            fprintf(stderr, "[DEBUG RESOLVE_LVAL] Resolving DEREFERENCE node...\n");
            #endif
            // 1. Evaluate the pointer expression itself (node->left)
            Value pointerValue = eval(lvalueNode->left);

            // 2. Check if it's actually a pointer
            if (pointerValue.type != TYPE_POINTER) {
                fprintf(stderr, "Runtime error: Attempting to dereference a non-pointer type (%s) in LValue.\n", varTypeToString(pointerValue.type));
                freeValue(&pointerValue); // Free temporary value
                EXIT_FAILURE_HANDLER();
            }

            // 3. Check if the pointer is nil
            if (pointerValue.ptr_val == NULL) {
                fprintf(stderr, "Runtime error: Attempting assignment via a nil pointer.\n");
                // Don't free pointerValue if it was nil (no heap data)
                EXIT_FAILURE_HANDLER();
            }

            // 4. The target for assignment is the Value struct pointed to by ptr_val
            Value* targetValuePtr = pointerValue.ptr_val;

            // 5. Free the temporary pointerValue struct returned by eval (it just held the address)
            //    Do NOT free the memory it points to (*targetValuePtr)
            //    Since pointerValue holds no heap data itself (just the ptr_val address),
            //    we don't need freeValue here.

            #ifdef DEBUG
            fprintf(stderr, "[DEBUG RESOLVE_LVAL] Dereference resolved to heap Value* address: %p\n", (void*)targetValuePtr);
            #endif

            return targetValuePtr; // Return the pointer TO THE HEAP-ALLOCATED VALUE
        }

        default:
            fprintf(stderr, "Runtime error: Invalid lvalue node type (%s) for assignment target resolution.\n", astTypeToString(lvalueNode->type));
            EXIT_FAILURE_HANDLER();
    }
    return NULL; // Should not be reached
}

Value setUnion(Value setA, Value setB) {
    // Basic check: Ensure both are sets
    if (setA.type != TYPE_SET || setB.type != TYPE_SET) {
        fprintf(stderr, "Internal Error: Non-set type passed to setUnion.\n");
        return makeVoid(); // Or handle error more gracefully
    }
    // TODO: Add check for base type compatibility if needed later

    Value result = makeValueForType(TYPE_SET, NULL); // Creates empty set structure {size=0, values=NULL, capacity=0}
    result.max_length = setA.set_val.set_size + setB.set_val.set_size; // Initial capacity guess

    if (result.max_length > 0) {
        result.set_val.set_values = malloc(sizeof(long long) * result.max_length);
        if (!result.set_val.set_values) {
            fprintf(stderr, "Malloc failed for set union result\n");
            result.max_length = 0; // Reset capacity on failure
            EXIT_FAILURE_HANDLER(); // Exit on critical memory failure
        }
    } else {
        result.set_val.set_values = NULL; // Handle case where both inputs are empty
    }
    result.set_val.set_size = 0; // Explicitly set initial size


    // Add elements from setA
    if (setA.set_val.set_values) {
        for (int i = 0; i < setA.set_val.set_size; i++) {
            addOrdinalToResultSet(&result, setA.set_val.set_values[i]); // Handles duplicates & realloc
        }
    }
    // Add elements from setB
     if (setB.set_val.set_values) {
        for (int i = 0; i < setB.set_val.set_size; i++) {
            addOrdinalToResultSet(&result, setB.set_val.set_values[i]); // Handles duplicates & realloc
        }
     }

    return result; // Return the new set Value
}

// Calculates the Difference (A - B)
Value setDifference(Value setA, Value setB) {
    if (setA.type != TYPE_SET || setB.type != TYPE_SET) { /* Error */ return makeVoid();}
    // TODO: Base type check?

    Value result = makeValueForType(TYPE_SET, NULL);
    result.max_length = setA.set_val.set_size; // Max possible size is size of A
    if (result.max_length > 0) {
         result.set_val.set_values = malloc(sizeof(long long) * result.max_length);
         if (!result.set_val.set_values) { /* Malloc error */ result.max_length=0; EXIT_FAILURE_HANDLER(); }
     } else {
         result.set_val.set_values = NULL;
     }
    result.set_val.set_size = 0;

    // Add elements from setA only if they are NOT in setB
    if (setA.set_val.set_values) {
        for (int i = 0; i < setA.set_val.set_size; i++) {
            if (!setContainsOrdinal(&setB, setA.set_val.set_values[i])) {
                // Assuming addOrdinalToResultSet handles capacity correctly
                addOrdinalToResultSet(&result, setA.set_val.set_values[i]);
            }
        }
    }
    return result;
}

// Calculates the Intersection (A * B)
Value setIntersection(Value setA, Value setB) {
    if (setA.type != TYPE_SET || setB.type != TYPE_SET) { /* Error */ return makeVoid();}
    // TODO: Base type check?

    Value result = makeValueForType(TYPE_SET, NULL);
    // Max possible size is the smaller of the two input sets
    result.max_length = (setA.set_val.set_size < setB.set_val.set_size) ? setA.set_val.set_size : setB.set_val.set_size;
     if (result.max_length > 0) {
         result.set_val.set_values = malloc(sizeof(long long) * result.max_length);
          if (!result.set_val.set_values) { /* Malloc error */ result.max_length=0; EXIT_FAILURE_HANDLER(); }
     } else {
         result.set_val.set_values = NULL;
     }
    result.set_val.set_size = 0;

    // Add elements from setA only if they ARE ALSO in setB
    if (setA.set_val.set_values) {
        for (int i = 0; i < setA.set_val.set_size; i++) {
            if (setContainsOrdinal(&setB, setA.set_val.set_values[i])) {
                // Assuming addOrdinalToResultSet handles capacity and duplicates within result
                 addOrdinalToResultSet(&result, setA.set_val.set_values[i]);
            }
        }
    }
    return result;
}

