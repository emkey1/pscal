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
    fprintf(stderr, "[DEBUG_FREE] Popping local env (localSymbols=%p)\n", (void*)sym);
#endif
    while (sym) {
        Symbol *next = sym->next;
#ifdef DEBUG
        fprintf(stderr, "[DEBUG_FREE]   Processing local symbol '%s' at %p (is_alias=%d, is_local_var=%d)\n", // Added is_local_var
                sym->name ? sym->name : "NULL", (void*)sym, sym->is_alias, sym->is_local_var); // Added is_local_var
#endif

        // *** FIX: Only free Value if it's NOT an alias AND NOT a standard local variable ***
        if (sym->value && !sym->is_alias && !sym->is_local_var) {
#ifdef DEBUG
             fprintf(stderr, "[DEBUG_FREE]     Freeing Value* at %p for non-alias, non-local symbol '%s'\n", (void*)sym->value, sym->name ? sym->name : "NULL");
#endif
             freeValue(sym->value);
             free(sym->value); // Free the Value struct itself
             sym->value = NULL; // Prevent dangling pointer in Symbol struct
        } else if (sym->value && (sym->is_alias || sym->is_local_var)) { // Log why it's skipped
#ifdef DEBUG
             fprintf(stderr, "[DEBUG_FREE]     Skipping freeValue for symbol '%s' (value=%p, alias=%d, local=%d)\n",
                     sym->name ? sym->name : "NULL", (void*)sym->value, sym->is_alias, sym->is_local_var);
#endif
        }
        // ... (free name, free symbol struct) ...
         if (sym->name) {
#ifdef DEBUG
            fprintf(stderr, "[DEBUG_FREE]     Freeing name '%s' at %p\n", sym->name, (void*)sym->name);
#endif
            free(sym->name);
        }
#ifdef DEBUG
        fprintf(stderr, "[DEBUG_FREE]     Freeing Symbol* at %p\n", (void*)sym);
#endif
        free(sym); // Free the Symbol struct itself
        sym = next;
    }
    localSymbols = NULL;
#ifdef DEBUG
    fprintf(stderr, "[DEBUG_FREE] Finished popping local env\n");
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
    if (isBuiltin(node->token->value)) {
        Value retVal = executeBuiltinProcedure(node);
#ifdef DEBUG
        fprintf(stderr, "DEBUG: Builtin procedure '%s' returned type %s\n",
                node->token->value, varTypeToString(retVal.type));
#endif
        return retVal;
    }

    Procedure *proc = procedure_table;
    while (proc) {
        if (strcmp(proc->name, node->token->value) == 0)
            break;
        proc = proc->next;
    }
    if (!proc) {
        fprintf(stderr, "Runtime error: routine '%s' not found.\n", node->token->value);
        EXIT_FAILURE_HANDLER();
    }

    int num_params = proc->proc_decl->child_count;
    Value *arg_values = malloc(sizeof(Value) * num_params);
    if (!arg_values) { fprintf(stderr, "Memory allocation error\n"); EXIT_FAILURE_HANDLER(); }

    for (int i = 0; i < num_params; i++) {
        AST *paramNode = proc->proc_decl->children[i];

        if (paramNode->by_ref) {
            // Var parameter: defer evaluation, mark for aliasing later
            arg_values[i].type = TYPE_VOID;
        } else {
            Value actualVal = eval(node->children[i]);
                // Always use makeCopyOfValue for value parameters ***
                // Ensures deep copy for ALL types (records, arrays, strings, etc.)
            arg_values[i] = makeCopyOfValue(&actualVal);
        }
    }

    SymbolEnvSnapshot snapshot;
    saveLocalEnv(&snapshot);

    for (int i = num_params - 1; i >= 0; i--) {
        AST *paramNode = proc->proc_decl->children[i];
        char *paramName = (paramNode->type == AST_VAR_DECL)
                            ? paramNode->children[0]->token->value
                            : paramNode->token->value;
        VarType ptype = paramNode->var_type;
        AST *type_def = paramNode->right;

        if (paramNode->by_ref) {
            if (node->children[i]->type != AST_VARIABLE) {
                fprintf(stderr, "Runtime error: var parameter must be a variable reference.\n");
                EXIT_FAILURE_HANDLER();
            }
            char *argVarName = node->children[i]->token->value;
            Symbol *callerSym = lookupSymbolIn(snapshot.head, argVarName);
            if (!callerSym) {
                fprintf(stderr, "Runtime error: variable '%s' not declared (for var parameter).\n", argVarName);
                EXIT_FAILURE_HANDLER();
            }

            insertLocalSymbol(paramName, ptype, type_def, false);
            Symbol *sym = lookupLocalSymbol(paramName);
            sym->value = callerSym->value;     // 🔧 Shared value pointer
            sym->is_alias = true;
        } else {
            insertLocalSymbol(paramName, ptype, type_def, false);
            Symbol *sym = lookupLocalSymbol(paramName);
            sym->is_alias = false;
            updateSymbol(paramName, arg_values[i]);
        }
    }

    free(arg_values);

    Value retVal;
    if (proc->proc_decl->type == AST_FUNCTION_DECL) {
        AST *type_def = proc->proc_decl->right;
        VarType retType = type_def->var_type;

        insertLocalSymbol("result", retType, type_def, false);
        Symbol *resSym = lookupLocalSymbol("result");
        resSym->is_alias = false;

        insertLocalSymbol(proc->name, retType, type_def, false);
        Symbol *funSym = lookupLocalSymbol(proc->name);
        funSym->value = resSym->value;
        funSym->is_alias = true;

        current_function_symbol = funSym;
        executeWithScope(proc->proc_decl->extra, false);

        Value *functionResult = funSym->value;
        retVal = functionResult ? makeCopyOfValue(functionResult) : makeInt(0);

        restoreLocalEnv(&snapshot);

        current_function_symbol = NULL;
        return retVal;
    } else {
        executeWithScope(proc->proc_decl->right, false);

        restoreLocalEnv(&snapshot);
        return makeInt(0);
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

            Value val = *(sym->value);  // Copy for safety
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
                             char *result = malloc(len_left + len_right + 1);
                             if (!result) { /* Mem alloc error */ EXIT_FAILURE_HANDLER(); }
                             strcpy(result, left_s);
                             strcat(result, right_s);
                             Value out = makeString(result); // makeString handles copying
                             free(result);
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
            Value val = eval(node->right);

            if (node->left->type == AST_ARRAY_ACCESS || node->left->type == AST_FIELD_ACCESS) {
                Value container = eval(node->left->left);

                if (container.type == TYPE_ARRAY && node->left->type == AST_ARRAY_ACCESS) {
                    Symbol *sym = lookupSymbol(node->left->left->token->value);
                    if (!sym || (sym->value->type != TYPE_ARRAY && sym->value->type != TYPE_STRING)) {
                        fprintf(stderr, "Runtime error: array access on non-array variable '%s'.\n", node->left->left->token->value);
                        EXIT_FAILURE_HANDLER();
                    }
                    if (sym->value->type == TYPE_STRING) {
                        long long idx = eval(node->left->children[0]).i_val;
                        int len = (int)strlen(sym->value->s_val);

                        if (idx < 1 || idx > len) {
                            dumpASTFromRoot(node);
                            dumpSymbolTable();
                            fprintf(stderr, "Runtime error: string index %lld out of bounds [1..%d].\n", idx, len);
                            EXIT_FAILURE_HANDLER();
                        }

                        char char_to_assign = '\0';
                        if (val.type == TYPE_CHAR) {
                            char_to_assign = val.c_val;
                        } else if (val.type == TYPE_STRING && strlen(val.s_val) == 1) {
                            char_to_assign = val.s_val[0];
                        } else {
                            fprintf(stderr, "Runtime error: assignment to string index requires a char or single-character string.\n");
                            EXIT_FAILURE_HANDLER();
                        }

                        sym->value->s_val[idx - 1] = char_to_assign;
                        break;
                    }
                }

                assignToContainer(&container, node->left, val);
            } else {
                Symbol *sym = lookupSymbol(node->left->token->value);
                if (!sym) {
                    fprintf(stderr, "Runtime error: variable '%s' not declared.\n", node->left->token->value);
#ifdef DEBUG
                    fprintf(stderr, "[DEBUG_ASSIGN] Target symbol '%s' NOT FOUND before updateSymbol!\n", node->left->token->value);
#endif
                    EXIT_FAILURE_HANDLER();
                }

                if (sym->type == TYPE_ENUM) {
                    if (val.type != TYPE_ENUM) {
                        fprintf(stderr, "Runtime error: Type mismatch in assignment. Expected TYPE_ENUM, got %s.\n", varTypeToString(val.type));
                        EXIT_FAILURE_HANDLER();
                    }  else {
                        // <<< ADD THIS DEBUG PRINT >>>
#ifdef DEBUG
                        fprintf(stderr, "[DEBUG_ASSIGN] Before updateSymbol: Target='%s', TargetType=%s, ValueType=%s\n",
                                node->left->token->value,                  // e.g., "relendpos"
                                varTypeToString(sym->type),          // Should be INTEGER for relendpos
                                varTypeToString(val.type));                // Should be INTEGER (result of Min)
#endif
                   }
                    
                    /*
                    if (strcmp(sym->value->enum_val.enum_name, val.enum_val.enum_name) != 0) {
                        fprintf(stderr, "Runtime error: Enumerated type mismatch in assignment. Expected '%s', got '%s'.\n",
                                sym->value->enum_val.enum_name, val.enum_val.enum_name);
#ifdef DEBUG
                        printf(" ====== AST DUMP START =====\n");
                        dumpASTFromRoot(node);
                        printf(" ====== AST DUMP END =====\n");
#endif
                        EXIT_FAILURE_HANDLER();
                    }
                     */
                    sym->value->enum_val.ordinal = val.enum_val.ordinal;

                } else if (sym->type == TYPE_CHAR && val.type == TYPE_STRING && strlen(val.s_val) == 1) {
                    Value ch = makeChar(val.s_val[0]);
                    updateSymbol(sym->name, ch);

                } else {
                    if (sym->type == TYPE_STRING) {
                        if (val.type == TYPE_STRING) {
                            // use as-is
                        } else if (val.type == TYPE_CHAR) {
                            char *temp = malloc(2);
                            if (!temp) {
                                fprintf(stderr, "Runtime error: Memory allocation failed during char-to-string coercion.\n");
                                EXIT_FAILURE_HANDLER();
                            }
                            temp[0] = val.c_val;
                            temp[1] = '\0';
                            val = makeString(temp);
                            free(temp);
                        } else {
                            fprintf(stderr, "Runtime error: Cannot assign non-string/char to string variable '%s'.\n", sym->name);
                            EXIT_FAILURE_HANDLER();
                        }
                        if (sym->value->s_val) {
                            free(sym->value->s_val);
                        }
                        sym->value->s_val = strdup(val.s_val);
                        if (!sym->value->s_val) {
                            fprintf(stderr, "Runtime error: Memory allocation failed during string assignment.\n");
                            EXIT_FAILURE_HANDLER();
                        }
                    } else {
                        updateSymbol(node->left->token->value, val);
                    }
                }
            }
            break;
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
            const char *var_name = node->token->value;
            Value start_val = eval(node->left);
            Value end_val = eval(node->right);
            int step = (node->type == AST_FOR_TO) ? 1 : -1;

            Symbol *sym = lookupSymbol(var_name);
            if (!sym) {
                fprintf(stderr,"Runtime error: Loop variable %s not found\n", var_name);
                EXIT_FAILURE_HANDLER();
            }

            // Initial Assignment (using updateSymbol for type coercion)
            updateSymbol(var_name, start_val);

            // Determine Loop End Condition Value (compatible with loop var type)
            long long end_condition_val;
            VarType loop_var_type = sym->type;

            // --- Determine end condition value based on loop variable type ---
            // (This part remains the same as your original code)
            if (loop_var_type == TYPE_CHAR) {
                 if(end_val.type == TYPE_CHAR) end_condition_val = end_val.c_val;
                 else if(end_val.type == TYPE_STRING && end_val.s_val && strlen(end_val.s_val)==1) end_condition_val = end_val.s_val[0];
                 else { /* Error: Incompatible end value */ fprintf(stderr, "Incompatible end value type %s for CHAR loop\n", varTypeToString(end_val.type)); EXIT_FAILURE_HANDLER(); }
            } else if (loop_var_type == TYPE_INTEGER || loop_var_type == TYPE_BYTE || loop_var_type == TYPE_WORD || loop_var_type == TYPE_ENUM || loop_var_type == TYPE_BOOLEAN) {
                 if(end_val.type == loop_var_type || end_val.type == TYPE_INTEGER) end_condition_val = end_val.i_val;
                 else { /* Error: Incompatible end value */ fprintf(stderr, "Incompatible end value type %s for %s loop\n", varTypeToString(end_val.type), varTypeToString(loop_var_type)); EXIT_FAILURE_HANDLER(); }
             } else { /* Error: Invalid loop variable type */ fprintf(stderr, "Invalid loop variable type: %s\n", varTypeToString(loop_var_type)); EXIT_FAILURE_HANDLER(); }


            // --- Main loop execution using C while(1) ---
            while (1) {
                // Read current value of loop variable
                Value current = *sym->value;

                // --- Loop Condition Check (using loop_var_type) ---
                long long current_condition_val;
                 if (loop_var_type == TYPE_CHAR) current_condition_val = current.c_val;
                 else current_condition_val = current.i_val; // Assume other ordinals use i_val

                bool loop_finished = (node->type == AST_FOR_TO) ? (current_condition_val > end_condition_val)
                                                                : (current_condition_val < end_condition_val);
                if (loop_finished) break; // Exit FOR loop normally
                // --- End Loop Condition Check ---

                // <<< ADDED: Reset break flag before executing body >>>
                break_requested = 0;

                executeWithScope(node->extra, false); // Execute loop body

                // <<< ADDED: Check break flag after executing body >>>
                if (break_requested) {
                     DEBUG_PRINT("[DEBUG] FOR loop exiting due to break.\n");
                     // No need for 'for_broken' flag if using global flag directly
                     break; // Exit the C 'while(1)' loop
                }

                // --- Calculate and Assign Next Value ---
                // Read value again, as it might have been changed in the loop body
                current = *sym->value; // Re-read current value
                long long next_ordinal;
                Value next_val = makeInt(0); // Initialize temporary value holder

                // (Logic for calculating next_ordinal and creating next_val remains the same)
                if (loop_var_type == TYPE_CHAR) {
                     if (current.type != TYPE_CHAR) { /* Error */ fprintf(stderr, "Loop variable %s changed type mid-loop\n", var_name); EXIT_FAILURE_HANDLER(); }
                     next_ordinal = current.c_val + step;
                     next_val = makeChar((char)next_ordinal);
                } else if (loop_var_type == TYPE_INTEGER || loop_var_type == TYPE_BYTE || loop_var_type == TYPE_WORD || loop_var_type == TYPE_ENUM || loop_var_type == TYPE_BOOLEAN) {
                     if (current.type != loop_var_type) { /* Error */ fprintf(stderr, "Loop variable %s changed type mid-loop\n", var_name); EXIT_FAILURE_HANDLER(); }
                     next_ordinal = current.i_val + step;
                     next_val = makeInt(next_ordinal);
                     next_val.type = loop_var_type;
                     if(loop_var_type == TYPE_ENUM && current.enum_val.enum_name) {
                         next_val.enum_val.enum_name = strdup(current.enum_val.enum_name); // Maintain enum type name
                     }
                } else {
                     fprintf(stderr, "Runtime error: Invalid FOR loop variable type '%s' during update.\n", varTypeToString(loop_var_type));
                     EXIT_FAILURE_HANDLER();
                }

                // Use updateSymbol for the assignment back to the loop variable
                updateSymbol(var_name, next_val);

                // Free temporary strdup if needed (for enum name)
                if (next_val.type == TYPE_ENUM && next_val.enum_val.enum_name) {
                     free(next_val.enum_val.enum_name);
                     next_val.enum_val.enum_name = NULL; // Avoid double free issues
                }
                 // --- End Calculate and Assign ---

            } // end C while(1)

            // <<< ADDED: Ensure flag is reset after the loop finishes >>>
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
                    FILE *input = stdin; // Default to console input
                    int startIndex = 0;  // Default to processing args from index 0

                    // --- CORRECTED FILE ARGUMENT HANDLING ---
                    if (node->child_count > 0) {
                        // Evaluate the first argument regardless of its AST type initially
                        Value firstArg = eval(node->children[0]);

                        // Check if the *evaluated value* is a valid FILE type
                        if (firstArg.type == TYPE_FILE && firstArg.f_val != NULL) {
                            // It is a valid file, use it as the input source
                            input = firstArg.f_val;
                            startIndex = 1; // Start processing actual variables from the *next* argument (index 1)
        #ifdef DEBUG
                            fprintf(stderr, "[DEBUG] READLN: Using file '%s' (handle:%p) as input.\n",
                                    firstArg.filename ? firstArg.filename : "?", (void*)input);
        #endif
                        } else {
                            // The first argument exists but is not a valid file type.
                            // Assume it's a regular variable to read into.
                            // Keep input = stdin and startIndex = 0.
        #ifdef DEBUG
                            fprintf(stderr, "[DEBUG] READLN: First argument is not a file. Using stdin.\n");
        #endif
                        }
                    } else {
                         // No arguments provided at all.
                         // Keep input = stdin and startIndex = 0.
        #ifdef DEBUG
                         fprintf(stderr, "[DEBUG] READLN: No arguments. Using stdin.\n");
        #endif
                    }
                    // --- END CORRECTED HANDLING ---

                    // Loop to read into variable arguments (correctly uses 'input' and 'startIndex')
                    for (int i = startIndex; i < node->child_count; i++) {
                        AST *target = node->children[i];
                        char buffer[DEFAULT_STRING_CAPACITY];

        #ifdef DEBUG
                        fprintf(stderr, "[DEBUG] READLN: Loop %d: Reading from %s into target '%s'...\n",
                                i, (input == stdin) ? "stdin" : "file",
                                target->token ? target->token->value : "complex lvalue");
        #endif

                        if (fgets(buffer, sizeof(buffer), input) == NULL) {
                            // Handle read error or EOF
                            if (feof(input)) {
                                 buffer[0] = '\0';
                            } else {
                                 fprintf(stderr, "Runtime error: Failed to read from input for READLN.\n");
                                 buffer[0] = '\0';
                            }
                        }
                        buffer[strcspn(buffer, "\n")] = 0; // Remove trailing newline

                        // ... (Your existing assignment logic for variable/field/etc.) ...
                        if (target->type == AST_FIELD_ACCESS) {
                            // ... (field access assignment logic) ...
                             Value recVal = eval(target->left);
                             if (recVal.type != TYPE_RECORD) { /* error */ EXIT_FAILURE_HANDLER(); }
                             FieldValue *fv = recVal.record_val;
                             int found = 0;
                             while (fv) {
                                 if (strcmp(fv->name, target->token->value) == 0) {
                                     found = 1;
                                     if (fv->value.type == TYPE_INTEGER) fv->value = makeInt(atoi(buffer));
                                     else if (fv->value.type == TYPE_REAL) fv->value = makeReal(atof(buffer));
                                     else if (fv->value.type == TYPE_BOOLEAN) fv->value = makeBoolean(atoi(buffer));
                                     else if (fv->value.type == TYPE_STRING) { if(fv->value.s_val) free(fv->value.s_val); fv->value = makeString(buffer); }
                                     else if (fv->value.type == TYPE_CHAR) { fv->value = makeChar((buffer[0] != '\0') ? buffer[0] : ' ');}
                                     break;
                                 }
                                 fv = fv->next;
                             }
                             if (!found) { /* error */ fprintf(stderr, "Field %s not found\n", target->token->value); EXIT_FAILURE_HANDLER();}
                        } else if (target->type == AST_VARIABLE) {
                            Symbol *sym = lookupSymbol(target->token->value);
                            if (!sym) { /* error */ fprintf(stderr, "Var %s not found\n", target->token->value); EXIT_FAILURE_HANDLER(); }
                            if (sym->type == TYPE_INTEGER) updateSymbol(target->token->value, makeInt(atoi(buffer)));
                            else if (sym->type == TYPE_REAL) updateSymbol(target->token->value, makeReal(atof(buffer)));
                            else if (sym->type == TYPE_STRING) updateSymbol(target->token->value, makeString(buffer));
                            else if (sym->type == TYPE_CHAR) { updateSymbol(target->token->value, makeChar((buffer[0] != '\0') ? buffer[0] : ' ')); }
                            // Add other types
                        } else {
                             fprintf(stderr, "Runtime error: Cannot readln into target of type %s.\n", astTypeToString(target->type));
                             EXIT_FAILURE_HANDLER();
                        }
                    }

                    // Fix for readln without arguments (should now work correctly with file input too if needed)
                    // This condition is now only true if no arguments *at all* were given,
                    // OR if only a file argument was given (e.g., readln(f);)
                    if (node->child_count == startIndex) {
        #ifdef DEBUG
                        fprintf(stderr, "[DEBUG] READLN: Consuming rest of line from %s (no variable args).\n", (input == stdin) ? "stdin" : "file");
        #endif
                        char dummy_buffer[2];
                        while (fgets(dummy_buffer, sizeof(dummy_buffer), input) != NULL) {
                             if (strchr(dummy_buffer, '\n') != NULL) {
                                  break;
                             }
                        }
                    }
                    break; // Break from switch case AST_READLN
                }

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
