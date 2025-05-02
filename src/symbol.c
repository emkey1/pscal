//
//  symbol.c
//  pscal
//
//  Created by Michael Miller on 3/25/25.
//
#include "lexer.h"
#include "utils.h"
#include "globals.h"
#include "interpreter.h"
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include "symbol.h"
#include <assert.h>
#include "list.h"
#include "ast.h"

// Helper to search only in the global symbol table.
Symbol *lookupGlobalSymbol(const char *name) {
    Symbol *current = globalSymbols;
    //DEBUG_PRINT("[DEBUG] lookupGlobalSymbol: Searching for '%s'. Starting list walk (globalSymbols=%p).\n", name, (void*)current);
    
    while (current) {
        // --- Add Diagnostic Check ---
        if (current->name == NULL) {
             fprintf(stderr, "CRITICAL ERROR in lookupGlobalSymbol: Encountered symbol node with NULL name in global list!\n");
             // Optionally dump more info about 'current' or surrounding nodes if possible
             // Depending on severity, you might want to exit here.
             EXIT_FAILURE_HANDLER(); // Or handle more gracefully
        }
        // --- End Diagnostic Check ---
        //DEBUG_PRINT("[DEBUG] lookupGlobalSymbol: Checking node %p, name '%s' against '%s'. next=%p\n",
      //                (void*)current,
       //               current->name ? current->name : "<NULL_NAME>", // Safety check
        //              name,
         //             (void*)current->next);

        if (strcmp(current->name, name) == 0) { // Original line 41
#ifdef DEBUG
            // Original line 43 (where crash likely occurs reading current->type for debug)
     //       DEBUG_PRINT("[DEBUG] lookupGlobalSymbol: found '%s', type=%s, value_ptr=%p\n",
          //              name,
           //             varTypeToString(current->type), // Accessing type here
            //            (void*)current->value); // Print value pointer for debugging
            // Safely print value details only if current->value is not NULL
            if (current->value) {
                 // ... (keep existing switch statement for printing value details) ...
            } else {
                 DEBUG_PRINT(", Value: NULL\n");
            }
#endif
            return current;
        }
        current = current->next; // Original line 62
    }
#ifdef DEBUG
    DEBUG_PRINT("[DEBUG] lookupGlobalSymbol: symbol '%s' not found in global_env\n", name);
#endif
    return NULL;
}

Symbol *lookupLocalSymbol(const char *name) {
    Symbol *current = localSymbols;
    while (current) {
        if (strcmp(current->name, name) == 0) {
#ifdef DEBUG
            DEBUG_PRINT("[DEBUG] lookupLocalSymbol: found '%s' with value ", name);
            if (current->value) {
                switch(current->value->type) {
                    case TYPE_STRING:
                        DEBUG_PRINT("TYPE_STRING \"%s\"", current->value->s_val ? current->value->s_val : "null");
                        break;
                    case TYPE_INTEGER:
                        DEBUG_PRINT("TYPE_INTEGER %lld", current->value->i_val);
                        break;
                    default:
                        DEBUG_PRINT("Type %s", varTypeToString(current->value->type));
                        break;
                }
            } else {
                DEBUG_PRINT("NULL");
            }
            DEBUG_PRINT("\n");
#endif
            return current;
        }
        current = current->next;
    }
#ifdef DEBUG
    DEBUG_PRINT("[DEBUG] lookupLocalSymbol: symbol '%s' not found in local_env\n", name);
#endif
    return NULL;
}

void updateSymbol(const char *name, Value val) {
    Symbol *sym = lookupSymbol(name);
    
#ifdef DEBUG
    fprintf(stderr, "[DEBUG_UPDATE_CHECK] Called updateSymbol for: '%s'. Symbol found: %p. is_const: %d. Incoming value type: %s\n",
        name,
        (void*)sym,
        sym ? sym->is_const : -1, // Print is_const status if sym found
        varTypeToString(val.type));
 // Optionally add a way to print the call stack or caller function if possible
#endif
    
    if (sym->is_const) {
        fprintf(stderr, "Runtime error: Cannot assign to constant '%s'.\n", name);
        EXIT_FAILURE_HANDLER();
    }
    
#ifdef DEBUG
    // This first switch is only for DEBUG printing the incoming value 'val'
    // It should NOT contain assignment logic or use 'sym' as 'sym' is not declared yet.
    DEBUG_PRINT("[DEBUG] updateSymbol: updating symbol '%s' to ", name);
    if (!sym) {
#ifdef DEBUG
        fprintf(stderr, "[DEBUG_UPDATE] updateSymbol: FAILED to find symbol '%s'\n", name);
         // Handle error (though lookupSymbol usually exits on failure)
#endif
    } else {
#ifdef DEBUG
        fprintf(stderr, "[DEBUG_UPDATE] updateSymbol: Entry for Name='%s', FoundSymType=%s, IncomingValueType=%s\n",
                name,                           // e.g., "relendpos"
                varTypeToString(sym->type),     // What type did lookupSymbol return? Is it VOID here?
                varTypeToString(val.type));     // Type of the value being assigned (INTEGER from Min)
#endif
    }
    switch(val.type) {
        case TYPE_STRING:
            DEBUG_PRINT("TYPE_STRING \"%s\"", val.s_val ? val.s_val : "null");
            break;
        case TYPE_CHAR:
             // Ensure this only prints info about 'val', doesn't try to use 'sym'
            DEBUG_PRINT("TYPE_CHAR '%c' (ord %d)", val.c_val, (int)val.c_val);
            break;
        case TYPE_INTEGER:
            DEBUG_PRINT("TYPE_INTEGER %lld", val.i_val);
            break;
        case TYPE_BOOLEAN:
            DEBUG_PRINT("TYPE_BOOLEAN %s", val.i_val ? "true" : "false");
            break;
        // Add other cases for debug printing if needed
        default:
            DEBUG_PRINT("Type %s", varTypeToString(val.type));
            break;
    }
    DEBUG_PRINT("\n");
#endif

    // Look up the symbol in the symbol table
    //Symbol *sym = lookupSymbol(name);
    if (!sym) {
        fprintf(stderr, "Runtime error: variable '%s' not declared.\n", name);
        EXIT_FAILURE_HANDLER();
    }

    // Optional: Print debug message if types don't match but are compatible
    // (Keep the existing compatible type check logic)
    if (sym->type != val.type) {
        if (!((sym->type == TYPE_REAL && val.type == TYPE_INTEGER) ||
              (sym->type == TYPE_CHAR && val.type == TYPE_STRING && val.s_val && strlen(val.s_val) == 1) || // Added NULL check for val.s_val
              (sym->type == TYPE_WORD && val.type == TYPE_INTEGER) ||
              (sym->type == TYPE_BYTE && val.type == TYPE_INTEGER) ||
              (sym->type == TYPE_BOOLEAN && val.type == TYPE_INTEGER) || // Added Boolean/Int check
              (sym->type == TYPE_CHAR && val.type == TYPE_INTEGER))) { // Added Char/Int check
            // Print only if types are truly incompatible according to your rules
#ifdef DEBUG
            fprintf(stderr, "Debug: Type conversion warning for '%s': expected %s, got %s\n",
                    name, varTypeToString(sym->type), varTypeToString(val.type));
#endif
        }
    }

    // --- Main Assignment Logic ---
    // This switch operates on the TYPE OF THE SYMBOL being assigned TO (sym->type)
    switch (sym->type) {
        case TYPE_INTEGER:
            // Allow assignment from Integer, Real (truncated), Byte, Word, Boolean, Char (ordinal)
             if (val.type == TYPE_INTEGER || val.type == TYPE_BYTE || val.type == TYPE_WORD || val.type == TYPE_BOOLEAN) {
                 sym->value->i_val = val.i_val; // Copy integer value (0/1 for bool)
             } else if (val.type == TYPE_CHAR) {
                 sym->value->i_val = (long long)val.c_val; // Assign ordinal value of char
             } else if (val.type == TYPE_REAL) {
                 sym->value->i_val = (long long)val.r_val; // Truncate real
             } else {
                  // Type mismatch if none of the above
                  fprintf(stderr, "Runtime error: Type mismatch assigning to INTEGER. Cannot assign %s.\n", varTypeToString(val.type));
                  EXIT_FAILURE_HANDLER();
             }
            break;
        case TYPE_REAL:
            // Assign REAL or INTEGER (promoted) to REAL
            sym->value->r_val = (val.type == TYPE_REAL) ? val.r_val : (double)val.i_val;
            break;
        case TYPE_BYTE:
        case TYPE_WORD:
            // Assign INTEGER to BYTE/WORD (potential range issues ignored here)
            if (val.type == TYPE_INTEGER || val.type == sym->type) {
                sym->value->i_val = val.i_val;
             } else {
                fprintf(stderr, "Runtime error: type mismatch in %s assignment.\n",
                        (sym->type == TYPE_BYTE) ? "byte" : "word");
                EXIT_FAILURE_HANDLER();
            }
            break;
        case TYPE_STRING:
             // Free existing string value if any
            if (sym->value->s_val) {
                free(sym->value->s_val);
                sym->value->s_val = NULL; // Avoid double free if strdup fails
            }

            const char* source_str = NULL;
            char char_buf[2]; // Buffer for converting char to string

            if (val.type == TYPE_STRING) {
                source_str = val.s_val;
            } else if (val.type == TYPE_CHAR) {
                // Handle assigning a CHAR to a STRING
                char_buf[0] = val.c_val;
                char_buf[1] = '\0';
                source_str = char_buf;
            } else {
                 fprintf(stderr, "Runtime error: Type mismatch assigning to STRING. Cannot assign %s.\n", varTypeToString(val.type));
                 EXIT_FAILURE_HANDLER();
            }

            // Handle fixed-length strings
            if (sym->value->max_length > 0) {
                size_t source_len = source_str ? strlen(source_str) : 0;
                size_t copy_len = (source_len > (size_t)sym->value->max_length) ? (size_t)sym->value->max_length : source_len;

                // Allocate exactly max_length + 1
                sym->value->s_val = malloc((size_t)sym->value->max_length + 1);
                if (!sym->value->s_val) {
                    fprintf(stderr, "FATAL: Memory allocation failed in updateSymbol (fixed string).\n");
                    EXIT_FAILURE_HANDLER();
                }
                if (source_str) {
                    strncpy(sym->value->s_val, source_str, copy_len);
                }
                sym->value->s_val[copy_len] = '\0'; // Ensure null termination

            } else { // Handle dynamic strings
                sym->value->s_val = source_str ? strdup(source_str) : strdup("");
                if (!sym->value->s_val) {
                    fprintf(stderr, "FATAL: Memory allocation failed in updateSymbol (dynamic string strdup).\n");
                    EXIT_FAILURE_HANDLER();
                }
            }
            break;

        case TYPE_RECORD:
            if (val.type == TYPE_RECORD) {
                // Free existing record fields first if necessary
                if (sym->value->record_val) {
                    freeFieldValue(sym->value->record_val); // Assumes freeFieldValue handles deep free
                }
                sym->value->record_val = copyRecord(val.record_val); // Assumes copyRecord handles deep copy
                if (!sym->value->record_val && val.record_val) { // Check if copy failed potentially
                     fprintf(stderr, "FATAL: Failed to copy record value in updateSymbol.\n");
                     EXIT_FAILURE_HANDLER();
                }
            } else {
                fprintf(stderr, "Runtime error: type mismatch in record assignment.\n");
                EXIT_FAILURE_HANDLER();
            }
            break;
        case TYPE_BOOLEAN:
            // Assign BOOLEAN or INTEGER (0=false, non-zero=true) to BOOLEAN
            if (val.type == TYPE_BOOLEAN) {
                sym->value->i_val = val.i_val;
            } else if (val.type == TYPE_INTEGER) {
                sym->value->i_val = (val.i_val != 0) ? 1 : 0;
            } else {
                fprintf(stderr, "Runtime error: type mismatch in boolean assignment.\n");
                EXIT_FAILURE_HANDLER();
            }
            break;
        case TYPE_FILE:
            if (val.type == TYPE_FILE) {
                sym->value->f_val = val.f_val; // Note: Shallow copy of FILE pointer
                if (sym->value->filename) free(sym->value->filename);
                sym->value->filename = val.filename ? strdup(val.filename) : NULL;
            } else {
                fprintf(stderr, "Runtime error: type mismatch in file assignment.\n");
                EXIT_FAILURE_HANDLER();
            }
            break;
        case TYPE_ARRAY: {
                    // Ensure incoming value is also an array
                    if (val.type != TYPE_ARRAY) {
                        fprintf(stderr, "Runtime error: type mismatch in array assignment (expected ARRAY, got %s).\n", varTypeToString(val.type));
                        EXIT_FAILURE_HANDLER();
                    }
                    // Check if dimensions match (optional but recommended)
                    if (sym->value && sym->value->array_val && sym->value->dimensions != val.dimensions) {
                         fprintf(stderr, "Runtime error: Array dimension mismatch in assignment for '%s' (expected %d, got %d).\n", sym->name, sym->value->dimensions, val.dimensions);
                         // Consider freeing existing sym->value array data here if necessary before exiting
                         EXIT_FAILURE_HANDLER();
                    }

                    // --- Correctly Free Existing Array Data (if any) ---
                    if (sym->value && sym->value->array_val) {
        #ifdef DEBUG
                        fprintf(stderr,"[DEBUG_UPDATE] Freeing old array for '%s'\n", sym->name);
        #endif
                        int old_total_size = 1;
                        // Check if bounds arrays exist before accessing them
                        if (sym->value->dimensions > 0 && sym->value->lower_bounds && sym->value->upper_bounds) {
                             for (int i = 0; i < sym->value->dimensions; i++) {
                                 old_total_size *= (sym->value->upper_bounds[i] - sym->value->lower_bounds[i] + 1);
                             }
                        } else {
                            old_total_size = 0; // Cannot determine size if info missing
                        }

                        for (int i = 0; i < old_total_size; i++) {
                            freeValue(&sym->value->array_val[i]); // Deep free elements
                        }
                        free(sym->value->array_val);
                        free(sym->value->lower_bounds); // Free the bounds arrays
                        free(sym->value->upper_bounds);
                        sym->value->array_val = NULL;
                        sym->value->lower_bounds = NULL;
                        sym->value->upper_bounds = NULL;
                        sym->value->dimensions = 0;
                    }

                    // --- Allocate and Copy New Array Data ---
        #ifdef DEBUG
                     fprintf(stderr,"[DEBUG_UPDATE] Allocating and copying new array for '%s'\n", sym->name);
        #endif
                     // Calculate correct total size using incoming value's multi-dim info
                     int new_total_size = 1;
                     if (val.dimensions <= 0 || !val.lower_bounds || !val.upper_bounds) {
                         fprintf(stderr, "Runtime error: Invalid dimensions or bounds in source array for assignment to '%s'.\n", sym->name);
                         EXIT_FAILURE_HANDLER(); // Exit, cannot proceed
                     }
                     for (int i = 0; i < val.dimensions; i++) {
                         new_total_size *= (val.upper_bounds[i] - val.lower_bounds[i] + 1);
                     }

                     // Allocate space for the Value structs
                     sym->value->array_val = malloc(sizeof(Value) * new_total_size);
                     if (!sym->value->array_val) { fprintf(stderr, "Memory allocation error in array assignment (array_val).\n"); EXIT_FAILURE_HANDLER(); }

                     // Allocate and copy bounds arrays
                     sym->value->lower_bounds = malloc(sizeof(int) * val.dimensions);
                     sym->value->upper_bounds = malloc(sizeof(int) * val.dimensions);
                     if (!sym->value->lower_bounds || !sym->value->upper_bounds) { fprintf(stderr, "Memory allocation error in array assignment (bounds).\n"); EXIT_FAILURE_HANDLER(); }

                     memcpy(sym->value->lower_bounds, val.lower_bounds, sizeof(int) * val.dimensions);
                     memcpy(sym->value->upper_bounds, val.upper_bounds, sizeof(int) * val.dimensions);

                     // Copy metadata
                     sym->value->dimensions = val.dimensions;
                     sym->value->element_type = val.element_type;
                     sym->value->element_type_def = val.element_type_def; // Copy type def link

                     // Deep copy each element
                     for (int i = 0; i < new_total_size; i++) {
                         sym->value->array_val[i] = makeCopyOfValue(&val.array_val[i]);
                     }
                     sym->value->type = TYPE_ARRAY; // Ensure type is set correctly
                    break;
                } 
        case TYPE_CHAR:
            if (val.type == TYPE_CHAR) {
                // Assigning CHAR to CHAR
                sym->value->c_val = val.c_val;
                sym->value->type = TYPE_CHAR; // Explicitly set type
            }
            else if (val.type == TYPE_STRING) {
                // Assigning non-empty STRING to CHAR (take first char)
                if (val.s_val && val.s_val[0] != '\0') {
                    // Ensure string has exactly one char for strictness (optional)
                    // if (strlen(val.s_val) != 1) {
                    //    fprintf(stderr, "Runtime error: Assigning multi-char string to char.\n");
                    //    EXIT_FAILURE_HANDLER();
                    // }
                    sym->value->c_val = val.s_val[0]; // Copy character
                    sym->value->type = TYPE_CHAR;     // <<< FIX: Set type to CHAR
                } else {
                    fprintf(stderr, "Runtime error: Cannot assign empty string to char.\n");
                    EXIT_FAILURE_HANDLER();
                }
            }
            else if (val.type == TYPE_INTEGER) {
                // Assigning INTEGER to CHAR (use integer as ordinal value)
                sym->value->c_val = (char)val.i_val; // Cast integer ordinal to char
                sym->value->type = TYPE_CHAR;     // <<< FIX: Set type to CHAR
            }
            else {
                // Any other type is a mismatch
                fprintf(stderr, "Runtime error: Type mismatch assigning to CHAR. Cannot assign %s.\n", varTypeToString(val.type));
                EXIT_FAILURE_HANDLER();
            }
            // Free old string value IF the symbol *was* storing a string before
            // This is complex - safer to handle freeing during value destruction
            // or ensure makeValueForType initializes properly. We'll assume
            // previous value was handled or was not a string.
            break; // End case TYPE_CHAR

        case TYPE_MEMORYSTREAM:
            if (val.type == TYPE_MEMORYSTREAM) {
                 // Free existing stream if necessary? Depends on ownership semantics.
                 // if (sym->value->mstream) freeMemoryStream(sym->value->mstream);
                 sym->value->mstream = val.mstream; // Shallow copy of pointer
            } else {
                fprintf(stderr, "Runtime error: type mismatch in memory stream assignment.\n");
                EXIT_FAILURE_HANDLER();
            }
            break;
        case TYPE_ENUM:
        {
            // Target symbol is an ENUM
            if (val.type == TYPE_ENUM) {
                // Assigning ENUM to ENUM

                // --- MODIFICATION: Force strdup approach ---
                #ifdef DEBUG
                fprintf(stderr, "[DEBUG UPDATE ENUM] Forcing strdup for symbol '%s'. Incoming name: '%s'\n",
                        sym->name, val.enum_val.enum_name ? val.enum_val.enum_name : "<NULL>");
                #endif

                // 1. Free the existing name in the symbol's value struct, if any.
                //    Check pointer before freeing for safety.
                if (sym->value && sym->value->enum_val.enum_name) {
                     #ifdef DEBUG
                     fprintf(stderr, "[DEBUG UPDATE ENUM] Freeing old name '%s' at %p for symbol '%s'\n",
                             sym->value->enum_val.enum_name, (void*)sym->value->enum_val.enum_name, sym->name);
                     #endif
                    free(sym->value->enum_val.enum_name);
                    sym->value->enum_val.enum_name = NULL; // Avoid dangling pointer
                }

                // 2. Duplicate the name from the source value ('val').
                //    Check source pointer before strdup.
                sym->value->enum_val.enum_name = val.enum_val.enum_name ? strdup(val.enum_val.enum_name) : NULL;

                // 3. Check if strdup failed (if source was not NULL)
                if (val.enum_val.enum_name && !sym->value->enum_val.enum_name) {
                     fprintf(stderr, "FATAL: strdup failed for enum name in updateSymbol (forced copy)\n");
                     EXIT_FAILURE_HANDLER();
                 }
                 #ifdef DEBUG
                 fprintf(stderr, "[DEBUG UPDATE ENUM] Symbol '%s' new name pointer: %p ('%s')\n",
                         sym->name, (void*)sym->value->enum_val.enum_name,
                         sym->value->enum_val.enum_name ? sym->value->enum_val.enum_name : "<NULL>");
                 #endif
                 // --- End Force strdup approach ---


                // Assign ordinal value (always do this if types are compatible)
                sym->value->enum_val.ordinal = val.enum_val.ordinal;
                sym->value->type = TYPE_ENUM; // Ensure type is correct

            } else if (val.type == TYPE_INTEGER) {
                // Assigning INTEGER to ENUM (logic remains the same)
                #ifdef DEBUG
                fprintf(stderr, "[DEBUG UPDATE ENUM] Assigning Integer %lld as ordinal to Enum '%s'\n", val.i_val, sym->name);
                #endif
                // Add bounds check if possible
                AST* typeDef = sym->type_def;
                if (typeDef && typeDef->type == AST_TYPE_REFERENCE) typeDef = typeDef->right;
                if (typeDef && typeDef->type == AST_ENUM_TYPE) {
                    int highOrdinal = typeDef->child_count - 1;
                    if (val.i_val < 0 || val.i_val > highOrdinal) {
                        fprintf(stderr, "Runtime error: Integer value %lld out of range for enum type '%s'.\n", val.i_val, sym->value->enum_val.enum_name ? sym->value->enum_val.enum_name : sym->name);
                        EXIT_FAILURE_HANDLER();
                    }
                }
                sym->value->enum_val.ordinal = (int)val.i_val; // Assign ordinal
                sym->value->type = TYPE_ENUM; // Ensure type is correct
                // enum_name remains unchanged when assigning integer
            } else {
                // Assigning other types to ENUM is an error
                fprintf(stderr, "Runtime error: type mismatch in enum assignment for '%s'. Expected TYPE_ENUM or TYPE_INTEGER, got %s.\n", sym->name, varTypeToString(val.type));
                EXIT_FAILURE_HANDLER();
            }
            break; // End case TYPE_ENUM
        } // End TYPE_ENUM block

        default:
            fprintf(stderr, "Runtime error: unhandled type (%s) in updateSymbol assignment.\n", varTypeToString(sym->type));
            EXIT_FAILURE_HANDLER();
    } // End switch (sym->type)
}

Symbol *lookupSymbol(const char *name) {
    Symbol *sym = lookupLocalSymbol(name);
    if (!sym) sym = lookupGlobalSymbol(name);

    if (!sym) {
        fprintf(stderr, "Runtime error: Symbol '%s' not found.\n", name);
#ifdef DEBUG
        dumpSymbolTable();
#endif
        EXIT_FAILURE_HANDLER();
    }

#ifdef DEBUG
    fprintf(stderr, "[DEBUG] lookupSymbol: '%s' found, type=%s\n", name, varTypeToString(sym->type));
#endif

    return sym;
}

void assignToRecord(FieldValue *record, const char *fieldName, Value val);

void assignToRecord(FieldValue *record, const char *fieldName, Value val) {
    while (record) {
        if (strcmp(record->name, fieldName) == 0) {
            record->value = val;
            return;
        }
        record = record->next;
    }
    fprintf(stderr, "Runtime error: field '%s' not found in record.\n", fieldName);
    EXIT_FAILURE_HANDLER();
}

Symbol *lookupSymbolIn(Symbol *env, const char *name) {
    Symbol *sym = env;
    while (sym) {
        if (strcasecmp(sym->name, name) == 0)
            return sym;
        sym = sym->next;
    }
    // fallback to global scope
    return lookupGlobalSymbol(name);
}

void insertGlobalSymbol(const char *name, VarType type, AST *type_def) {
    if (!name || name[0] == '\0') {
        fprintf(stderr, "[ERROR] Attempted to insert global symbol with invalid name.\n");
        return; // Or EXIT_FAILURE_HANDLER();
    }

    // Check for duplicates (optional: add warning/error if desired)
    if (lookupGlobalSymbol(name)) {
        // fprintf(stderr, "[Warning] Duplicate global symbol '%s'\n", name);
        return; // Silently ignore duplicate for now
    }

    // Allocate Symbol struct
    Symbol *new_symbol = malloc(sizeof(Symbol));
    if (!new_symbol) {
        fprintf(stderr, "Memory allocation error in insertGlobalSymbol (Symbol struct)\n");
        EXIT_FAILURE_HANDLER();
    }

    // Duplicate name
    new_symbol->name = strdup(name);
    if (!new_symbol->name) { // Check strdup result
        fprintf(stderr, "Memory allocation error (strdup name) in insertGlobalSymbol\n");
        free(new_symbol);
        EXIT_FAILURE_HANDLER();
    }

    // Set basic fields
    new_symbol->type = type;
    new_symbol->is_alias = false;
    new_symbol->is_const = false;
    new_symbol->is_local_var = false; // Globals aren't local vars
    new_symbol->next = NULL;
    new_symbol->type_def = type_def; // Store type definition link

    // --- CORRECTED Initialization ---
    // Allocate the Value struct itself
    new_symbol->value = malloc(sizeof(Value));
    if (!new_symbol->value) {
        fprintf(stderr, "Memory allocation error (malloc Value) in insertGlobalSymbol\n");
        free(new_symbol->name);
        free(new_symbol);
        EXIT_FAILURE_HANDLER();
    }
    // Initialize the contents of the Value struct using the helper function
    // This function handles default values and fixed-string allocation correctly.
    *(new_symbol->value) = makeValueForType(type, type_def);
    // --- END CORRECTED Initialization ---

    // Link into global symbol table
    if (!globalSymbols) {
        globalSymbols = new_symbol;
    } else {
        Symbol *current = globalSymbols;
        while (current->next) {
            current = current->next;
        }
        current->next = new_symbol;
    }

#ifdef DEBUG
    fprintf(stderr, "[DEBUG] insertGlobalSymbol('%s', type=%s, max_len=%d)\n",
            name, varTypeToString(type), new_symbol->value ? new_symbol->value->max_length : -2);
#endif
}

Symbol *insertLocalSymbol(const char *name, VarType type, AST* type_def, bool is_variable_declaration) {
    if (!name || name[0] == '\0') {
         fprintf(stderr, "[ERROR] Attempted to insert local symbol with invalid name.\n");
         return NULL; // Return NULL on error
    }

    // Check for existing local symbol (case-insensitive)
#ifdef DEBUG
    fprintf(stderr, "[DEBUG] insertLocalSymbol: Checking for existing local symbol named '%s'\n", name);
#endif
    Symbol *existing = localSymbols;
    while (existing) {
        if (existing->name && strcasecmp(existing->name, name) == 0) { // Added NULL check for existing->name
#ifdef DEBUG
            fprintf(stderr, "[DEBUG] insertLocalSymbol: Symbol '%s' already exists in local scope, returning existing.\n", name);
#endif
            return existing; // Symbol already exists in this scope
        }
        existing = existing->next;
    }

    // Create a new symbol if it doesn't exist locally
    Symbol *sym = malloc(sizeof(Symbol));
    if (!sym) {
        fprintf(stderr, "FATAL: malloc failed for Symbol struct in insertLocalSymbol for '%s'\n", name);
        EXIT_FAILURE_HANDLER();
    }

#ifdef DEBUG
    fprintf(stderr, "[DEBUG] insertLocalSymbol('%s', type=%s, is_var_decl=%d)\n", name, varTypeToString(type), is_variable_declaration);
#endif

    // Duplicate and lowercase the name for consistent storage/lookup
    char *lowerName = strdup(name);
    if (!lowerName) {
        fprintf(stderr, "FATAL: strdup failed for name in insertLocalSymbol for '%s'\n", name);
        free(sym);
        EXIT_FAILURE_HANDLER();
    }
    // Convert to lowercase (optional, depends on desired lookup behavior)
    // for (int i = 0; lowerName[i]; i++) {
    //     lowerName[i] = (char)tolower((unsigned char)lowerName[i]);
    // }
    sym->name = lowerName; // Store the processed name

    // Assign type information
    sym->type = type;
    sym->type_def = type_def; // Store link to the AST type definition node

    // Allocate and initialize the Value struct using the helper function
    sym->value = malloc(sizeof(Value));
    if (!sym->value) {
        fprintf(stderr, "FATAL: malloc failed for Value struct in insertLocalSymbol for '%s'\n", sym->name);
        free(sym->name);
        free(sym);
        EXIT_FAILURE_HANDLER();
    }
    *(sym->value) = makeValueForType(type, type_def); // Initialize using helper

    // Set flags
    sym->is_alias = false;
    sym->is_local_var = is_variable_declaration;
    sym->is_const = false;

    // Link the new symbol into the head of the local symbol list
    sym->next = localSymbols;
    localSymbols = sym;

    return sym; // Return the newly created symbol
}

Procedure *getProcedureTable(void) {
    return procedure_table;
}
