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
    Symbol *sym = lookupSymbol(name); // Handles not found error

#ifdef DEBUG
    fprintf(stderr, "[DEBUG_UPDATE_CHECK] Called updateSymbol for: '%s'. Symbol found: %p. is_const: %d. Incoming value type: %s\n",
            name, (void*)sym, sym ? sym->is_const : -1, varTypeToString(val.type));
#endif

    if (!sym) {
        // lookupSymbol already exited if not found, but defensive check
        fprintf(stderr,"Internal Error: updateSymbol called for non-existent symbol '%s'.\n", name);
        EXIT_FAILURE_HANDLER(); // Should not happen if lookupSymbol works
        return;
    }
    if (sym->is_const) {
        fprintf(stderr, "Runtime error: Cannot assign to constant '%s'.\n", name);
        EXIT_FAILURE_HANDLER();
    }
    // Ensure the symbol has a value structure to update
    if (!sym->value) {
        fprintf(stderr, "Runtime error: Symbol '%s' has NULL value pointer during update. Cannot assign.\n", name);
        // Attempt to allocate and initialize? Or is this an internal error?
        // Let's treat it as an error for now, as variables should be initialized.
         sym->value = malloc(sizeof(Value));
         if (!sym->value) {
             fprintf(stderr, "FATAL: Memory allocation failed trying to recover NULL value pointer for '%s'\n", name);
             EXIT_FAILURE_HANDLER();
         }
         // Initialize *minimally* before attempting assignment below
         memset(sym->value, 0, sizeof(Value));
         sym->value->type = sym->type; // Set the type based on the symbol table entry
         fprintf(stderr, "Warning: Initialized previously NULL value pointer for symbol '%s' during update.\n", name);
         // If initialized here, default values might be missing (e.g., for records/arrays)
         // Consider using makeValueForType if type_def is available:
         // *(sym->value) = makeValueForType(sym->type, sym->type_def);
    }


    // --- Type Compatibility Check ---

    bool types_compatible = false;
    if (sym->type == val.type) {
        types_compatible = true; // Same types are always compatible
    } else if (sym->type == TYPE_POINTER) {
        // Target is POINTER. Allow assigning POINTER or NIL.
        if (val.type == TYPE_POINTER) { // NIL is also TYPE_POINTER
            types_compatible = true;
            // Add stricter base type check here if desired for non-nil assignments:
            // if (val.ptr_val != NULL && sym->value->base_type_node != val.base_type_node) {
            //    fprintf(stderr, "Runtime error: Incompatible pointer types in assignment to '%s'.\n", name);
            //    EXIT_FAILURE_HANDLER();
            // }
        }
    } else if (val.type == TYPE_POINTER) {
        // Assigning POINTER/NIL to a non-pointer type is always an error.
        types_compatible = false;
    } else {
        // Check other compatible non-pointer assignments
        if (sym->type == TYPE_REAL && val.type == TYPE_INTEGER) types_compatible = true;
        else if (sym->type == TYPE_CHAR && val.type == TYPE_STRING && val.s_val && strlen(val.s_val) == 1) types_compatible = true;
        else if (sym->type == TYPE_STRING && val.type == TYPE_CHAR) types_compatible = true;
        else if (sym->type == TYPE_WORD && val.type == TYPE_INTEGER) types_compatible = true;
        else if (sym->type == TYPE_BYTE && val.type == TYPE_INTEGER) types_compatible = true;
        else if (sym->type == TYPE_BOOLEAN && val.type == TYPE_INTEGER) types_compatible = true; // Allow Int (0/1) to Bool
        else if (sym->type == TYPE_INTEGER && val.type == TYPE_BOOLEAN) types_compatible = true; // Allow Bool (0/1) to Int
        else if (sym->type == TYPE_CHAR && val.type == TYPE_INTEGER) types_compatible = true; // Allow Int Ordinal to Char
        else if (sym->type == TYPE_INTEGER && val.type == TYPE_CHAR) types_compatible = true; // Allow Char Ordinal to Int
        else if (sym->type == TYPE_INTEGER && (val.type == TYPE_BYTE || val.type == TYPE_WORD)) types_compatible = true; // Allow Byte/Word to Int
        // Add array/record compatibility checks if needed (usually types must match exactly)
        else if (sym->type == TYPE_ARRAY && val.type == TYPE_ARRAY) types_compatible = true; // Basic check, deeper check needed
        else if (sym->type == TYPE_RECORD && val.type == TYPE_RECORD) types_compatible = true; // Basic check, deeper check needed
        else if (sym->type == TYPE_SET && val.type == TYPE_SET) types_compatible = true; // Basic check
        else if (sym->type == TYPE_ENUM && val.type == TYPE_ENUM) {
             // Allow assigning enums if they are the same underlying type (check name)
             if (sym->value && val.enum_val.enum_name && sym->value->enum_val.enum_name &&
                 strcmp(sym->value->enum_val.enum_name, val.enum_val.enum_name) == 0) {
                 types_compatible = true;
             } else if (!sym->value->enum_val.enum_name || !val.enum_val.enum_name) {
                  #ifdef DEBUG
                  fprintf(stderr, "Warning: Assigning enums with missing type names for '%s'. Assuming compatible.\n", name);
                  #endif
                  types_compatible = true; // Allow if names missing, rely on ordinal
             } else {
                 // Mismatched enum types
                  types_compatible = false;
             }
        }
         else if (sym->type == TYPE_ENUM && val.type == TYPE_INTEGER) types_compatible = true; // Allow Int Ordinal to Enum

    }

    if (!types_compatible) {
        fprintf(stderr, "Runtime error: Type mismatch. Cannot assign %s to %s for LHS '%s'.\n",
                varTypeToString(val.type), varTypeToString(sym->type), name);
        EXIT_FAILURE_HANDLER();
    }
    // --- End Type Compatibility Check ---


    // --- Main Assignment Logic ---
    // This switch operates on the TYPE OF THE SYMBOL being assigned TO (sym->type)
    switch (sym->type) {
        case TYPE_INTEGER:
            if (val.type == TYPE_INTEGER || val.type == TYPE_BYTE || val.type == TYPE_WORD || val.type == TYPE_BOOLEAN) { sym->value->i_val = val.i_val; }
            else if (val.type == TYPE_CHAR) { sym->value->i_val = (long long)val.c_val; }
            else if (val.type == TYPE_REAL) { sym->value->i_val = (long long)val.r_val; }
            else { /* Should have been caught */ }
            break;
        case TYPE_REAL:
            sym->value->r_val = (val.type == TYPE_REAL) ? val.r_val : (double)val.i_val;
            break;
        case TYPE_BYTE:
            if (val.type == TYPE_INTEGER || val.type == TYPE_BYTE || val.type == TYPE_WORD) {
                // TODO: Add range check for byte (0-255)
                 if (val.i_val < 0 || val.i_val > 255) fprintf(stderr, "Warning: Overflow assigning %lld to BYTE variable '%s'.\n", val.i_val, name);
                 sym->value->i_val = (val.i_val & 0xFF); // Mask to byte range
            } else { /* Should have been caught */ }
            break;
        case TYPE_WORD:
            if (val.type == TYPE_INTEGER || val.type == TYPE_BYTE || val.type == TYPE_WORD) {
                // TODO: Add range check for word (0-65535)
                 if (val.i_val < 0 || val.i_val > 65535) fprintf(stderr, "Warning: Overflow assigning %lld to WORD variable '%s'.\n", val.i_val, name);
                 sym->value->i_val = (val.i_val & 0xFFFF); // Mask to word range
            } else { /* Should have been caught */ }
            break;
        case TYPE_STRING:
             // Free existing string value first
            if (sym->value->s_val) { free(sym->value->s_val); sym->value->s_val = NULL; }
            const char* source_str = NULL; char char_buf[2];
            if (val.type == TYPE_STRING) { source_str = val.s_val; }
            else if (val.type == TYPE_CHAR) { char_buf[0] = val.c_val; char_buf[1] = '\0'; source_str = char_buf; }
            else { /* Should have been caught */ }
            if (!source_str) source_str = ""; // Safety for NULL source

            if (sym->value->max_length > 0) { // Fixed-length string
                size_t source_len = strlen(source_str);
                size_t copy_len = (source_len > (size_t)sym->value->max_length) ? (size_t)sym->value->max_length : source_len;
                sym->value->s_val = malloc((size_t)sym->value->max_length + 1);
                if (!sym->value->s_val) { /* Malloc error */ EXIT_FAILURE_HANDLER(); }
                strncpy(sym->value->s_val, source_str, copy_len);
                sym->value->s_val[copy_len] = '\0';
            } else { // Dynamic string
                sym->value->s_val = strdup(source_str);
                if (!sym->value->s_val) { /* Malloc error */ EXIT_FAILURE_HANDLER(); }
            }
            break;
        case TYPE_RECORD:
             if (val.type == TYPE_RECORD) {
                 freeValue(sym->value); // Free old fields/data
                 *sym->value = makeCopyOfValue(&val); // Deep copy
                 sym->value->type = TYPE_RECORD;
             } else { /* Should have been caught */ }
             break;
        case TYPE_BOOLEAN:
            if (val.type == TYPE_BOOLEAN) { sym->value->i_val = val.i_val; }
            else if (val.type == TYPE_INTEGER) { sym->value->i_val = (val.i_val != 0) ? 1 : 0; }
            else { /* Should have been caught */ }
            break;
        case TYPE_FILE:
             // Error - should have been caught by type check unless adding specific FILE logic
             fprintf(stderr, "Runtime error: Direct assignment of FILE variables is not supported.\n");
             EXIT_FAILURE_HANDLER();
             break;
        case TYPE_ARRAY:
             if (val.type == TYPE_ARRAY) {
                 // TODO: Add dimension/bounds/type compatibility checks here if needed before copy
                 freeValue(sym->value); // Frees old array data/bounds
                 *sym->value = makeCopyOfValue(&val); // Deep copy
                 sym->value->type = TYPE_ARRAY;
             } else { /* Should have been caught */ }
             break;
        case TYPE_CHAR:
            if (val.type == TYPE_CHAR) { sym->value->c_val = val.c_val; }
            else if (val.type == TYPE_STRING && val.s_val && strlen(val.s_val) == 1) { sym->value->c_val = val.s_val[0]; }
            else if (val.type == TYPE_INTEGER) { sym->value->c_val = (char)val.i_val; } // Assign char by ordinal
            else { /* Should have been caught */ }
            sym->value->type = TYPE_CHAR; // Ensure type is CHAR
            break;
        case TYPE_MEMORYSTREAM:
             if (val.type == TYPE_MEMORYSTREAM) {
                  // Shallow copy pointer - Requires careful memory management by user
                  sym->value->mstream = val.mstream;
             } else { /* Should have been caught */ }
             break;
        case TYPE_ENUM:
             if (val.type == TYPE_ENUM) {
                  // Free old name if exists
                 if (sym->value->enum_val.enum_name) { free(sym->value->enum_val.enum_name); sym->value->enum_val.enum_name = NULL;}
                 // Copy name (strdup)
                 sym->value->enum_val.enum_name = val.enum_val.enum_name ? strdup(val.enum_val.enum_name) : NULL;
                 if (val.enum_val.enum_name && !sym->value->enum_val.enum_name) { /* Malloc error */ EXIT_FAILURE_HANDLER();}
                 // Copy ordinal
                 sym->value->enum_val.ordinal = val.enum_val.ordinal;
             } else if (val.type == TYPE_INTEGER) {
                 // Assign INTEGER to ENUM (assign ordinal, check bounds)
                  AST* typeDef = sym->type_def; // Use the target symbol's type definition
                  if (typeDef && typeDef->type == AST_TYPE_REFERENCE) typeDef = lookupType(typeDef->token->value); // Resolve reference
                  if (typeDef && typeDef->type == AST_ENUM_TYPE) {
                      int highOrdinal = typeDef->child_count - 1;
                      if (val.i_val < 0 || val.i_val > highOrdinal) {
                          fprintf(stderr, "Runtime error: Integer value %lld out of range for enum type '%s'.\n", val.i_val, sym->value->enum_val.enum_name ? sym->value->enum_val.enum_name : sym->name);
                          EXIT_FAILURE_HANDLER();
                      }
                  } else { /* Warning or error if cannot check bounds */ }
                  sym->value->enum_val.ordinal = (int)val.i_val;
                  // Keep existing enum_name
             } else { /* Should have been caught */ }
             sym->value->type = TYPE_ENUM; // Ensure type is ENUM
             break;
        case TYPE_SET:
             if (val.type == TYPE_SET) {
                 // Free existing set contents
                 freeValue(sym->value);
                 *sym->value = makeCopyOfValue(&val); // Deep copy
                 sym->value->type = TYPE_SET;
             } else { /* Should have been caught */ }
             break;
        case TYPE_POINTER:
            // Type check already ensured val.type is TYPE_POINTER (incl. nil)
            // Perform shallow copy of the pointer value (address or NULL)
            sym->value->ptr_val = val.ptr_val;

            // --- REMOVED THE INCORRECT COPY OF base_type_node ---
            // The base type is fixed by the variable's declaration and
            // should not change during assignment. Assigning nil should
            // definitely not clear the base type information.
            // sym->value->base_type_node = val.base_type_node; // <<< REMOVE THIS LINE >>>

            #ifdef DEBUG
            fprintf(stderr, "[DEBUG UPDATE] Assigning pointer value %p to symbol '%s'\n", (void*)val.ptr_val, name);
            #endif
            break;

        default:
            // Should not happen if type system is complete
            fprintf(stderr, "Runtime error: unhandled target type (%s) in updateSymbol assignment logic.\n", varTypeToString(sym->type));
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
