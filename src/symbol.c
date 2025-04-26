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
    DEBUG_PRINT("[DEBUG] lookupGlobalSymbol: Searching for '%s'. Starting list walk (globalSymbols=%p).\n", name, (void*)current);
    
    while (current) {
        // --- Add Diagnostic Check ---
        if (current->name == NULL) {
             fprintf(stderr, "CRITICAL ERROR in lookupGlobalSymbol: Encountered symbol node with NULL name in global list!\n");
             // Optionally dump more info about 'current' or surrounding nodes if possible
             // Depending on severity, you might want to exit here.
             EXIT_FAILURE_HANDLER(); // Or handle more gracefully
        }
        // --- End Diagnostic Check ---
        DEBUG_PRINT("[DEBUG] lookupGlobalSymbol: Checking node %p, name '%s' against '%s'. next=%p\n",
                      (void*)current,
                      current->name ? current->name : "<NULL_NAME>", // Safety check
                      name,
                      (void*)current->next);

        if (strcmp(current->name, name) == 0) { // Original line 41
#ifdef DEBUG
            // Original line 43 (where crash likely occurs reading current->type for debug)
            DEBUG_PRINT("[DEBUG] lookupGlobalSymbol: found '%s', type=%s, value_ptr=%p\n",
                        name,
                        varTypeToString(current->type), // Accessing type here
                        (void*)current->value); // Print value pointer for debugging
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
            // Assign INTEGER or REAL (truncated) to INTEGER
            sym->value->i_val = (val.type == TYPE_INTEGER) ? val.i_val : (long long)val.r_val;
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
                // Assigning ENUM to ENUM (existing logic)
                if (!sym->value->enum_val.enum_name || sym->value->enum_val.enum_name[0] == '\0') {
                    if (sym->value->enum_val.enum_name) free(sym->value->enum_val.enum_name);
                    sym->value->enum_val.enum_name = val.enum_val.enum_name ? strdup(val.enum_val.enum_name) : NULL;
                } else {
                    if (!val.enum_val.enum_name || strcmp(sym->value->enum_val.enum_name, val.enum_val.enum_name) != 0) {
                        fprintf(stderr, "Runtime error: Enumerated type mismatch. Expected '%s', got '%s'.\n", sym->value->enum_val.enum_name, val.enum_val.enum_name ? val.enum_val.enum_name : "<NULL>");
                        EXIT_FAILURE_HANDLER();
                    }
                }
                // Assign ordinal value (safe even if names didn't match but we didn't exit)
                sym->value->enum_val.ordinal = val.enum_val.ordinal;
                sym->value->type = TYPE_ENUM; // Ensure type is set
                
            } else if (val.type == TYPE_INTEGER) {
                // <<< FIX: Assigning INTEGER to ENUM
                // Treat integer as the ordinal value.
                // Optional: Add bounds check if type definition info is available
                AST* typeDef = sym->type_def;
                if (typeDef && typeDef->type == AST_TYPE_REFERENCE) typeDef = typeDef->right;
                if (typeDef && typeDef->type == AST_ENUM_TYPE) {
                    int highOrdinal = typeDef->child_count - 1;
                    if (val.i_val < 0 || val.i_val > highOrdinal) {
                        fprintf(stderr, "Runtime error: Integer value %lld out of range for enum type '%s'.\n", val.i_val, sym->value->enum_val.enum_name ? sym->value->enum_val.enum_name : sym->name);
                        EXIT_FAILURE_HANDLER();
                    }
                } // Else: Cannot perform bounds check if typeDef isn't available/correct
                
                sym->value->enum_val.ordinal = (int)val.i_val; // Assign ordinal
                sym->value->type = TYPE_ENUM; // Ensure type is correct
                // Note: enum_name remains unchanged as we only got an integer
                
            } else {
                // Assigning other types to ENUM is an error
                fprintf(stderr, "Runtime error: type mismatch in enum assignment. Expected TYPE_ENUM or TYPE_INTEGER, got %s.\n", varTypeToString(val.type));
                EXIT_FAILURE_HANDLER();
            }
            break; // End case TYPE_ENUM
        }
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


void updateSymbolDirect(Symbol *sym, Value val) {
#ifdef DEBUG
    if (sym->value->type != val.type) {
        fprintf(stderr, "[DEBUG] Type mismatch on direct updateSymbol. Expected %s, got %s\n", varTypeToString(sym->value->type), varTypeToString(val.type));
    }
#endif
    *sym->value = val;
}

void assignToRecord(FieldValue *record, const char *fieldName, Value val);

void assignToContainer(Value *container, AST *node, Value val) {
    if (node->type == AST_ARRAY_ACCESS) {
        // ✅ Special case: string indexing
        if (container->type == TYPE_STRING) {
            if (node->child_count != 1) {
                fprintf(stderr, "String access must have exactly one index\n");
                EXIT_FAILURE_HANDLER();
            }

            int index = (int)eval(node->children[0]).i_val;
            if (index < 1 || index > (int)strlen(container->s_val)) {
                fprintf(stderr, "String index out of bounds: %d\n", index);
                EXIT_FAILURE_HANDLER();
            }

            if (val.type != TYPE_CHAR) {
                fprintf(stderr, "Cannot assign non-char to string[%d]\n", index);
                EXIT_FAILURE_HANDLER();
            }

            container->s_val[index - 1] = val.c_val; // 1-based Pascal indexing
            return;
        }

        // 🧠 Regular array case
        int indices[node->child_count];
        for (int i = 0; i < node->child_count; i++) {
            indices[i] = (int)eval(node->children[i]).i_val;
        }

        int flatIndex = computeFlatOffset(container, indices);

        // Calculate total size of array
        int totalSize = 1;
        for (int i = 0; i < container->dimensions; i++) {
            totalSize *= (container->upper_bounds[i] - container->lower_bounds[i] + 1);
        }

        if (flatIndex < 0 || flatIndex >= totalSize) {
            fprintf(stderr, "Array assignment index out of bounds\n");
            EXIT_FAILURE_HANDLER();
        }

        container->array_val[flatIndex] = val;
    }
    else if (node->type == AST_FIELD_ACCESS) {
        assignToRecord(container->record_val, node->token->value, val);
    }
    else {
        fprintf(stderr, "assignToContainer: unsupported container type %d\n", node->type);
        EXIT_FAILURE_HANDLER();
    }
}


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
        // Optionally dump call stack or more context if possible in debug mode
        return; // Or EXIT_FAILURE_HANDLER(); depending on desired strictness
    }
    
    if (lookupGlobalSymbol(name)) {
        fprintf(stderr, "[ERROR] Duplicate global symbol '%s'\n", name);
        return;
    }
    Symbol *new_symbol = malloc(sizeof(Symbol));
    if (!new_symbol) {
        fprintf(stderr, "Memory allocation error in insertGlobalSymbol\n");
        EXIT_FAILURE_HANDLER();
    }
    new_symbol->name = strdup(name);
    new_symbol->type = type;
    new_symbol->is_alias = false;
    new_symbol->is_const = false;
    new_symbol->is_local_var = false;
    new_symbol->next = NULL;
    new_symbol->value = calloc(1, sizeof(Value)); // calloc zero-initializes
    new_symbol->type_def = type_def; // Store type definition link

    if (!new_symbol->value) { // Check calloc result
        fprintf(stderr, "Memory allocation error (calloc) in insertGlobalSymbol\n");
        free(new_symbol->name);
        free(new_symbol);
        EXIT_FAILURE_HANDLER();
    }

    // Initialize value based on type
    new_symbol->value->type = type; // Set the type first

    if (type == TYPE_STRING) {
        *new_symbol->value = makeString(""); // Explicitly initialize string
    }
    // --- ADDED INITIALIZATION FOR ENUM ---
    else if (type == TYPE_ENUM) {
        // Initialize enum fields
        new_symbol->value->enum_val.ordinal = 0; // Default to the first ordinal value
        // Attempt to get the enum type name from the type definition AST
        if (type_def && type_def->token && type_def->token->value) {
             new_symbol->value->enum_val.enum_name = strdup(type_def->token->value);
             if (!new_symbol->value->enum_val.enum_name) {
                 fprintf(stderr, "Memory allocation error (strdup enum_name) in insertGlobalSymbol\n");
                 EXIT_FAILURE_HANDLER(); // Handle allocation failure
             }
        } else {
             // Fallback if type_def name isn't available (should ideally not happen for enums)
             new_symbol->value->enum_val.enum_name = strdup("<unknown_enum>");
             fprintf(stderr, "Warning: Could not determine enum type name for global symbol '%s'.\n", name);
        }
    }
    // --- END OF ADDED INITIALIZATION ---
    // Add initializations for other types like RECORD, ARRAY if needed here
    // based on how makeValueForType works for local symbols.

    // Link into symbol table
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
    fprintf(stderr, "[DEBUG] insertGlobalSymbol('%s', type=%s)\n", name, varTypeToString(type));
#endif
}

// src/symbol.c (Complete and Corrected)

Symbol *insertLocalSymbol(const char *name, VarType type, AST* type_def, bool is_variable_declaration) {
    // First, check for an existing local symbol (case-insensitive check)
#ifdef DEBUG
    fprintf(stderr, "[DEBUG] insertLocalSymbol: Checking for existing local symbol named '%s'\n", name ? name : "<NULL_NAME>");
#endif
    Symbol *existing = localSymbols;
    while (existing) {
        if (existing->name == NULL) {
             fprintf(stderr, "CRITICAL ERROR in insertLocalSymbol: Encountered symbol node with NULL name in local list!\n");
             EXIT_FAILURE_HANDLER();
        }
        // Use strcasecmp for case-insensitive comparison if needed, or strcmp for case-sensitive
        if (strcasecmp(existing->name, name) == 0) {
#ifdef DEBUG
            fprintf(stderr, "[DEBUG] insertLocalSymbol: Symbol '%s' already exists in local scope, returning existing.\n", name);
#endif
            // If symbol exists, maybe update its type/value or return?
            // Current behavior: Return existing. Decide if re-declaration is an error.
            return existing;
        }
        existing = existing->next;
    }

    // Create a new symbol if it doesn't exist locally
    Symbol *sym = malloc(sizeof(Symbol));
    if (!sym) { // Check malloc for Symbol struct
        fprintf(stderr, "FATAL: malloc failed for Symbol struct in insertLocalSymbol for '%s'\n", name ? name : "<NULL_NAME>");
        EXIT_FAILURE_HANDLER();
    }

#ifdef DEBUG
    fprintf(stderr, "[DEBUG] insertLocalSymbol('%s', type=%s, is_var_decl=%d)\n", name ? name : "<NULL_NAME>", varTypeToString(type), is_variable_declaration);
#endif

    // Duplicate and lowercase the name for consistent storage/lookup
    char *lowerName = name ? strdup(name) : NULL;
    if (name && !lowerName) { // Check strdup for name
        fprintf(stderr, "FATAL: strdup failed for name in insertLocalSymbol for '%s'\n", name);
        free(sym); // Free partially allocated symbol
        EXIT_FAILURE_HANDLER();
    }
    if (lowerName) {
        for (int i = 0; lowerName[i]; i++) {
            lowerName[i] = (char)tolower((unsigned char)lowerName[i]);
        }
    }
    sym->name = lowerName; // Store the processed name

    // Assign type information
    sym->type = type;
    sym->type_def = type_def; // Store link to the AST type definition node

    // Allocate the Value struct
    sym->value = malloc(sizeof(Value));
    if (!sym->value) { // Check malloc for Value struct
        fprintf(stderr, "FATAL: malloc failed for Value struct in insertLocalSymbol for '%s'\n", sym->name ? sym->name : "<NULL_NAME>");
        if (sym->name) free(sym->name);
        free(sym);
        EXIT_FAILURE_HANDLER();
    }

    // Initialize the Value struct based on its type using makeValueForType
    // makeValueForType should handle its own internal allocations and checks
    *sym->value = makeValueForType(type, type_def);

    // Set flags for the symbol
    sym->is_alias = false; // Default, change later for VAR parameters etc.
    sym->is_local_var = is_variable_declaration; // Mark if it's a VAR decl or parameter
    sym->is_const = false; // Local symbols are not const by default

    // *** Handling for Fixed-Length Strings (Integrated from original file content) ***
    // Note: This logic assumes a specific AST structure for fixed strings.
    // It checks if the type is STRING and if a type_def AST node is provided
    // that allows extracting a length.
    if (type == TYPE_STRING && type_def != NULL) {
        AST* actualTypeDef = type_def;
        // Resolve reference if needed
        if (actualTypeDef->type == AST_TYPE_REFERENCE) {
            actualTypeDef = actualTypeDef->right; // Assuming right points to the actual definition
        }

        // Check if the actual type definition indicates a fixed length
        // This depends heavily on how typeSpecifier parses 'string[len]'
        // Assuming the AST_VARIABLE node for 'string' might have the length node in its 'right' child
        if (actualTypeDef && actualTypeDef->type == AST_VARIABLE && strcasecmp(actualTypeDef->token->value, "string") == 0 && actualTypeDef->right != NULL)
        {
            AST* lenNode = actualTypeDef->right; // The node representing the length
            Value lenVal; // Need a temporary Value struct to hold evaluation result

            // Evaluate the length expression - CAUTION: Calling eval during parsing/symbol insertion can be risky
            // if the expression isn't guaranteed to be a simple constant.
            // A safer approach might be to resolve this during a later semantic pass.
            // For now, assuming it works for constant integer lengths:
            // lenVal = eval(lenNode); // <<< Original call - commented out due to risk
            
            // SAFER ALTERNATIVE: Assume length node is AST_NUMBER with integer const
            if (lenNode->type == AST_NUMBER && lenNode->token && lenNode->token->type == TOKEN_INTEGER_CONST) {
                lenVal = makeInt(atoll(lenNode->token->value)); // Use atoll for long long
            } else {
                // If it's not a simple integer constant, treat length as invalid for now
                 fprintf(stderr, "Warning: Fixed string length for '%s' is not a simple integer constant. Treating as dynamic.\n", name);
                 lenVal = makeInt(0); // Indicate invalid length
            }


            if (lenVal.type == TYPE_INTEGER && lenVal.i_val > 0) {
                sym->value->max_length = (int)lenVal.i_val; // Store fixed length
#ifdef DEBUG
                fprintf(stderr, "[DEBUG] insertLocalSymbol: Set fixed string length for '%s' to %d\n", name, sym->value->max_length);
#endif
                // Re-allocate buffer for fixed size + null terminator
                if(sym->value->s_val) free(sym->value->s_val); // Free initial "" from makeValueForType
                sym->value->s_val = calloc(sym->value->max_length + 1, 1); // Use calloc for zero-init
                if (!sym->value->s_val) { // Check calloc
                    fprintf(stderr, "FATAL: calloc failed for fixed string buffer in insertLocalSymbol for '%s'\n", name);
                    if(sym->name) free(sym->name);
                    free(sym->value); // Free Value struct
                    free(sym); // Free Symbol struct
                    EXIT_FAILURE_HANDLER();
                }
            } else {
                 // Warning already printed if not a simple integer const
                 if (lenVal.type != TYPE_INTEGER || lenVal.i_val <= 0) {
                      fprintf(stderr, "Warning: Invalid fixed string length specification (value <= 0 or not integer) for '%s'. Treating as dynamic.\n", name);
                 }
                 sym->value->max_length = 0; // Treat as dynamic if length is invalid
                 // Keep the dynamically allocated "" from makeValueForType
            }
        } else {
            sym->value->max_length = 0; // Not a fixed-length string declaration
        }
    } else {
         sym->value->max_length = 0; // Default for non-string types
    }
    // *** End Fixed-Length String Handling ***

    // Link the new symbol into the head of the local symbol list
    sym->next = localSymbols;
    localSymbols = sym;

    return sym; // Return the newly created symbol
}

Procedure *getProcedureTable(void) {
    return procedure_table;
}
