//
//  symbol.c
//  pscal
//
//  Created by Michael Miller on 3/25/25.
//
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
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

// SDL Globals
SDL_Window* gSdlWindow = NULL;
SDL_Renderer* gSdlRenderer = NULL;
SDL_Color gSdlCurrentColor = { 255, 255, 255, 255 }; // Default white
bool gSdlInitialized = false;
int gSdlWidth = 0;
int gSdlHeight = 0;
TTF_Font* gSdlFont = NULL;
int gSdlFontSize   = 16;
SDL_Texture* gSdlTextures[MAX_SDL_TEXTURES];
int gSdlTextureWidths[MAX_SDL_TEXTURES];
int gSdlTextureHeights[MAX_SDL_TEXTURES];
bool gSdlTtfInitialized = false;
// SDL Stuff End



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
    fprintf(stderr, "[DEBUG updateSymbol] Entry: Symbol '%s' located. sym=%p, sym->value=%p\n",
            name, (void*)sym, (void*)(sym ? sym->value : NULL));
    fflush(stderr);
    #endif

    #ifdef DEBUG
    fprintf(stderr, "[DEBUG_UPDATE_CHECK] Called updateSymbol for: '%s'. Symbol found: %p. is_const: %d. Incoming value type: %s\n",
            name, (void*)sym, sym ? sym->is_const : -1, varTypeToString(val.type));
    fflush(stderr);
    #endif

    if (!sym) {
        fprintf(stderr,"Internal Error: updateSymbol called for non-existent symbol '%s'.\n", name);
        EXIT_FAILURE_HANDLER();
    }
    if (sym->is_const) {
        fprintf(stderr, "Runtime error: Cannot assign to constant '%s'.\n", name);
        EXIT_FAILURE_HANDLER();
    }
    if (!sym->value) {
        fprintf(stderr, "Runtime error: Symbol '%s' has NULL value pointer during update. Cannot assign.\n", name);
        EXIT_FAILURE_HANDLER();
    }


    // --- Type Compatibility Check ---
    bool types_compatible = false;
    if (sym->type == val.type) { types_compatible = true; }
    else if (sym->type == TYPE_POINTER) { if (val.type == TYPE_POINTER) { types_compatible = true; } }
    else if (val.type == TYPE_POINTER) { types_compatible = false; } // Assigning PTR/NIL to non-PTR
    else { /* Check other compatible types */
        if (sym->type == TYPE_REAL && val.type == TYPE_INTEGER) types_compatible = true;
        else if (sym->type == TYPE_CHAR && val.type == TYPE_STRING && val.s_val && strlen(val.s_val) == 1) types_compatible = true;
        else if (sym->type == TYPE_STRING && val.type == TYPE_CHAR) types_compatible = true;
        else if (sym->type == TYPE_WORD && val.type == TYPE_INTEGER) types_compatible = true;
        else if (sym->type == TYPE_BYTE && val.type == TYPE_INTEGER) types_compatible = true;
        else if (sym->type == TYPE_BOOLEAN && val.type == TYPE_INTEGER) types_compatible = true;
        else if (sym->type == TYPE_INTEGER && val.type == TYPE_BOOLEAN) types_compatible = true;
        else if (sym->type == TYPE_CHAR && val.type == TYPE_INTEGER) types_compatible = true;
        else if (sym->type == TYPE_INTEGER && val.type == TYPE_CHAR) types_compatible = true;
        else if (sym->type == TYPE_INTEGER && (val.type == TYPE_BYTE || val.type == TYPE_WORD)) types_compatible = true;
        else if (sym->type == TYPE_ARRAY && val.type == TYPE_ARRAY) types_compatible = true;
        else if (sym->type == TYPE_RECORD && val.type == TYPE_RECORD) types_compatible = true;
        else if (sym->type == TYPE_SET && val.type == TYPE_SET) types_compatible = true;
        else if (sym->type == TYPE_ENUM && val.type == TYPE_ENUM) {
             if (sym->value && val.enum_val.enum_name && sym->value->enum_val.enum_name && strcmp(sym->value->enum_val.enum_name, val.enum_val.enum_name) == 0) { types_compatible = true; }
             else if (!sym->value->enum_val.enum_name || !val.enum_val.enum_name) { types_compatible = true; } else { types_compatible = false; }
        }
        else if (sym->type == TYPE_ENUM && val.type == TYPE_INTEGER) types_compatible = true;
    }
    if (!types_compatible) {
        fprintf(stderr, "Runtime error: Type mismatch. Cannot assign %s to %s for LHS '%s'.\n", varTypeToString(val.type), varTypeToString(sym->type), name);
        EXIT_FAILURE_HANDLER();
    }
    // --- End Type Compatibility Check ---


    // --- Free old heap data if necessary BEFORE assignment ---
    if (sym->type != TYPE_POINTER) {
         if (sym->type == TYPE_RECORD || sym->type == TYPE_ARRAY || sym->type == TYPE_SET || sym->type == TYPE_ENUM || sym->type == TYPE_MEMORYSTREAM || sym->type == TYPE_FILE) {
              freeValue(sym->value);
         } else if (sym->type == TYPE_STRING) {
             // String freeing is handled manually below before assignment
         }
    } // If target is POINTER, freeValue is NOT called on sym->value

    // --- Main Assignment Logic ---
    AST* preserved_base_type_node = NULL; // Temp storage for base node

    // <<< MODIFICATION: Store base node *before* the switch if target is pointer >>>
    if(sym->type == TYPE_POINTER) {
       preserved_base_type_node = sym->value->base_type_node;
       #ifdef DEBUG
       fprintf(stderr, "[DEBUG updateSymbol] POINTER case PRE-SAVE: Preserving base_type_node %p for '%s'.\n",
               (void*)preserved_base_type_node, name);
       fflush(stderr);
       #endif
    }


    switch (sym->type) {
        // --- Cases for non-pointer types ---
        case TYPE_INTEGER: if (val.type == TYPE_INTEGER || val.type == TYPE_BYTE || val.type == TYPE_WORD || val.type == TYPE_BOOLEAN) { sym->value->i_val = val.i_val; } else if (val.type == TYPE_CHAR) { sym->value->i_val = (long long)val.c_val; } else if (val.type == TYPE_REAL) { sym->value->i_val = (long long)val.r_val; } break;
        case TYPE_REAL: sym->value->r_val = (val.type == TYPE_REAL) ? val.r_val : (double)val.i_val; break;
        case TYPE_BYTE: if (val.type == TYPE_INTEGER || val.type == TYPE_BYTE || val.type == TYPE_WORD) { if (val.i_val < 0 || val.i_val > 255) fprintf(stderr, "Warning: Overflow assigning %lld to BYTE variable '%s'.\n", val.i_val, name); sym->value->i_val = (val.i_val & 0xFF); } break;
        case TYPE_WORD: if (val.type == TYPE_INTEGER || val.type == TYPE_BYTE || val.type == TYPE_WORD) { if (val.i_val < 0 || val.i_val > 65535) fprintf(stderr, "Warning: Overflow assigning %lld to WORD variable '%s'.\n", val.i_val, name); sym->value->i_val = (val.i_val & 0xFFFF); } break;
        case TYPE_STRING:
            // Free existing string data if present
            if (sym->value->s_val) {
                free(sym->value->s_val);
                sym->value->s_val = NULL;
            }
            // Prepare source string (handle incoming CHAR or STRING)
            const char* source_str = NULL;
            char char_buf[2];
            if (val.type == TYPE_STRING) {
                source_str = val.s_val;
            } else if (val.type == TYPE_CHAR) { // Allow assigning CHAR to STRING
                char_buf[0] = val.c_val;
                char_buf[1] = '\0';
                source_str = char_buf;
            }
            if (!source_str) source_str = ""; // Handle NULL source

            // Handle fixed vs dynamic length copy
            if (sym->value->max_length > 0) { // Target is fixed-length string
                size_t source_len = strlen(source_str);
                size_t copy_len = (source_len > (size_t)sym->value->max_length) ? (size_t)sym->value->max_length : source_len;
                sym->value->s_val = malloc((size_t)sym->value->max_length + 1);
                if (!sym->value->s_val) { /* Malloc Error */ EXIT_FAILURE_HANDLER(); }
                strncpy(sym->value->s_val, source_str, copy_len);
                sym->value->s_val[copy_len] = '\0';
            } else { // Target is dynamic string
                sym->value->s_val = strdup(source_str);
                if (!sym->value->s_val) { /* Malloc Error */ EXIT_FAILURE_HANDLER(); }
            }

            // <<< FIX: Add this line >>>
            sym->value->type = TYPE_STRING; // Ensure the type field is correctly set

            break; // End of TYPE_STRING case
        case TYPE_RECORD: freeValue(sym->value); *sym->value = makeCopyOfValue(&val); sym->value->type = TYPE_RECORD; break;
        case TYPE_BOOLEAN: if (val.type == TYPE_BOOLEAN) { sym->value->i_val = val.i_val; } else if (val.type == TYPE_INTEGER) { sym->value->i_val = (val.i_val != 0) ? 1 : 0; } break;
        case TYPE_FILE: fprintf(stderr, "Runtime error: Direct assignment of FILE variables is not supported.\n"); EXIT_FAILURE_HANDLER(); break;
        case TYPE_ARRAY: freeValue(sym->value); *sym->value = makeCopyOfValue(&val); sym->value->type = TYPE_ARRAY; break;
        case TYPE_CHAR: if (val.type == TYPE_CHAR) { sym->value->c_val = val.c_val; } else if (val.type == TYPE_STRING && val.s_val && strlen(val.s_val) == 1) { sym->value->c_val = val.s_val[0]; } else if (val.type == TYPE_INTEGER) { sym->value->c_val = (char)val.i_val; } sym->value->type = TYPE_CHAR; break;
        case TYPE_MEMORYSTREAM: /* Shallow copy might be okay if streams are reference counted or ownership is clear */ if (val.type == TYPE_MEMORYSTREAM) { freeValue(sym->value); sym->value->mstream = val.mstream; } break; // TODO: Review ownership
        case TYPE_ENUM: if (val.type == TYPE_ENUM) { freeValue(sym->value); sym->value->enum_val.enum_name = val.enum_val.enum_name ? strdup(val.enum_val.enum_name) : NULL; /*...*/ sym->value->enum_val.ordinal = val.enum_val.ordinal; } else if (val.type == TYPE_INTEGER) { /* Ordinal check needed */ sym->value->enum_val.ordinal = (int)val.i_val; } sym->value->type = TYPE_ENUM; break;
        case TYPE_SET: freeValue(sym->value); *sym->value = makeCopyOfValue(&val); sym->value->type = TYPE_SET; break;

        case TYPE_POINTER:
            // Assign the incoming pointer address (which might be NULL)
            sym->value->ptr_val = val.ptr_val;
            // Restore the original base_type_node as a safeguard against unexpected modification
            sym->value->base_type_node = preserved_base_type_node;
            #ifdef DEBUG
            fprintf(stderr, "[DEBUG UPDATE] POINTER case for '%s': Assigned ptr_val=%p. Base node RESTORED to %p.\n",
                 name, (void*)sym->value->ptr_val, (void*)sym->value->base_type_node);
            fflush(stderr);
            #endif
            break;

        default:
            fprintf(stderr, "Runtime error: unhandled target type (%s) in updateSymbol assignment logic.\n", varTypeToString(sym->type));
            EXIT_FAILURE_HANDLER();
    }
    #ifdef DEBUG
    // (Optional debug print after switch)
    #endif
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
        // DEBUG_PRINT("[Warning] Duplicate global symbol '%s'\n", name);
        return; // Silently ignore duplicate for now
    }

    // Allocate Symbol struct
    Symbol *new_symbol = malloc(sizeof(Symbol));
    if (!new_symbol) { fprintf(stderr, "Memory allocation error in insertGlobalSymbol (Symbol struct)\n"); EXIT_FAILURE_HANDLER(); }

    // Duplicate name
    new_symbol->name = strdup(name);
    if (!new_symbol->name) { fprintf(stderr, "Memory allocation error (strdup name) in insertGlobalSymbol\n"); free(new_symbol); EXIT_FAILURE_HANDLER(); }

    // Set basic fields
    new_symbol->type = type;
    new_symbol->is_alias = false;
    new_symbol->is_const = false;
    new_symbol->is_local_var = false; // Globals aren't local vars
    new_symbol->next = NULL;
    new_symbol->type_def = type_def; // Store type definition link (e.g., TYPE_REFERENCE)

    // Allocate the Value struct itself
    new_symbol->value = malloc(sizeof(Value));
    if (!new_symbol->value) { fprintf(stderr, "Memory allocation error (malloc Value) in insertGlobalSymbol\n"); free(new_symbol->name); free(new_symbol); EXIT_FAILURE_HANDLER(); }

    // Initialize the contents of the Value struct using the helper function
    Value temp_val = makeValueForType(type, type_def); // Call helper
    memcpy(new_symbol->value, &temp_val, sizeof(Value)); // Try memcpy instead                   // Copy result into symbol's value
#ifdef DEBUG
fprintf(stderr, "[DEBUG insertGlobalSymbol] '%s': Symbol Value Addr=%p, Copied Value base_type_node is now %p\n",
        name,
        (void*)new_symbol->value, // Print address of the allocated Value struct
        (void*)(new_symbol->value ? new_symbol->value->base_type_node : NULL));
fflush(stderr);
#endif

    // <<< Add Debug Print Here >>>
    #ifdef DEBUG
    fprintf(stderr, "[DEBUG insertGlobalSymbol] '%s': Copied Value from makeValueForType. new_symbol->value->base_type_node is now %p\n",
            name, (void*)(new_symbol->value ? new_symbol->value->base_type_node : NULL));
    fflush(stderr);
    #endif
    // <<< End Debug Print >>>

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

    // Optional: Keep original debug print if desired
    #ifdef DEBUG
    // fprintf(stderr, "[DEBUG] insertGlobalSymbol('%s', type=%s, max_len=%d)\n", name, varTypeToString(type), new_symbol->value ? new_symbol->value->max_length : -2);
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
