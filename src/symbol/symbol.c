//
//  symbol.c
//  pscal
//
//  Created by Michael Miller on 3/25/25.
//
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "frontend/lexer.h"
#include "core/utils.h"
#include "globals.h"
#include "backend_ast/interpreter.h"
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include "symbol/symbol.h"
#include <assert.h>
#include "core/list.h"
#include "frontend/ast.h"

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



// --- Hash Table Implementation ---

/**
 * Creates a new hash table.
 * Allocates memory for the HashTable structure and initializes buckets to NULL.
 * Exits the program with a failure status if memory allocation fails.
 *
 * @return A pointer to the newly created HashTable structure.
 */
HashTable *createHashTable(void) {
    HashTable *table = malloc(sizeof(HashTable));
    if (!table) {
        fprintf(stderr, "Memory allocation error in createHashTable\n");
        EXIT_FAILURE_HANDLER();
    }
    // Initialize all buckets to NULL
    for (int i = 0; i < HASHTABLE_SIZE; ++i) {
        table->buckets[i] = NULL;
    }
    DEBUG_PRINT("[DEBUG SYMBOL] Created HashTable at %p with %d buckets.\n", (void*)table, HASHTABLE_SIZE);
    return table;
}

/**
 * Frees all memory allocated for a hash table, including all Symbols within buckets.
 *
 * @param table A pointer to the HashTable structure to free.
 */
void freeHashTable(HashTable *table) {
    if (!table) {
        return;
    }
    DEBUG_PRINT("[DEBUG SYMBOL] Freeing HashTable at %p.\n", (void*)table);
    // Iterate through each bucket
    for (int i = 0; i < HASHTABLE_SIZE; ++i) {
        // Traverse and free the linked list in the current bucket
        Symbol *current = table->buckets[i];
        while (current) {
            Symbol *next = current->next;
            DEBUG_PRINT("[DEBUG SYMBOL] Freeing Symbol '%s' at %p in bucket %d.\n", current->name ? current->name : "?", (void*)current, i);
            
            if (current->name) {
                free(current->name);
                current->name = NULL; // Good practice
            }
            
            if (current->value) { // Check if there is a value pointer
                if (!current->is_alias) {
                    #ifdef DEBUG
                    fprintf(stderr, "[DEBUG SYMBOL] Freeing Value contents for NON-ALIASED Symbol '%s' at %p (Value* is %p).\n",
                            current->name ? current->name : "NULL", (void*)current, (void*)current->value);
                    #endif
                    freeValue(current->value); // Frees the *contents* of the Value struct

                    // --- CORRECTED: Only free the Value struct itself if it's not an alias ---
                    #ifdef DEBUG
                    fprintf(stderr, "[DEBUG SYMBOL] Freeing Value struct itself for NON-ALIASED Symbol '%s' at %p (Value* was %p).\n",
                            current->name ? current->name : "NULL", (void*)current, (void*)current->value);
                    #endif
                    free(current->value);      // Frees the Value struct itself
                    // --- END CORRECTION ---

                } else {
                    // If it's an alias, just nullify the pointer in the symbol struct
                    // to prevent double-free. The memory it points to is owned elsewhere.
                    #ifdef DEBUG
                    fprintf(stderr, "[DEBUG SYMBOL] Not freeing (neither contents nor struct) for ALIASED Value for Symbol '%s' at %p. Value owned by original symbol.\n",
                            current->name ? current->name : "NULL", (void*)current);
                    #endif
                }
                current->value = NULL; // Nullify the pointer in the Symbol struct in both cases (alias or freed)
            }
            free(current); // Free the Symbol struct
            current = next;
        }
        table->buckets[i] = NULL; // Ensure bucket pointer is NULL after freeing
    }
    // Free the HashTable structure itself
    free(table);
}
/**
 * Simple hash function for symbol names.
 * Sums ASCII values, multiplies by a prime, and takes modulo HASHTABLE_SIZE.
 * Uses strcasecmp for lookup, so hash can be based on original case.
 *
 * @param name The symbol name string.
 * @return The hash value (an index into the hash table buckets array).
 */
int hashFunctionName(const char *name) {
    if (!name) return 0; // Handle NULL name safely
    unsigned long hash = 0;
    int c;
    // Using a simple polynomial rolling hash approach
    // You can use more sophisticated hash functions if needed
    while ((c = *name++)) {
        hash = hash * 31 + tolower((unsigned char)c); // Use tolower for case-insensitivity in hashing too
    }
    return hash % HASHTABLE_SIZE; // Map hash value to an index within the table size
}


/**
 * Internal helper to look up a symbol in a specific hash table.
 *
 * @param table A pointer to the HashTable to search.
 * @param name  The symbol name string to look up.
 * @return A pointer to the found Symbol, or NULL if not found.
 */
Symbol* hashTableLookup(HashTable* table, const char* name) {
    if (!table || !name) return NULL;
    char lower_name[MAX_SYMBOL_LENGTH];
    strncpy(lower_name, name, MAX_SYMBOL_LENGTH -1);
    lower_name[MAX_SYMBOL_LENGTH-1] = '\0';
    for(int i = 0; lower_name[i]; i++) lower_name[i] = tolower(lower_name[i]);

    int index = hashFunctionName(lower_name);
#ifdef DEBUG
    fprintf(stderr, "[DEBUG hashTableLookup] Looking for '%s' (lc: '%s') in bucket %d\n", name, lower_name, index); // DIAGNOSTIC
#endif
    Symbol* current = table->buckets[index];
    while (current != NULL) {
#ifdef DEBUG
        fprintf(stderr, "[DEBUG hashTableLookup]   Checking against: '%s'\n", current->name); // DIAGNOSTIC
#endif
        if (current->name && strcmp(current->name, lower_name) == 0) {
#ifdef DEBUG
            fprintf(stderr, "[DEBUG hashTableLookup]   Found '%s'\n", name); // DIAGNOSTIC
#endif
            return current;
        }
        current = current->next;
    }
#ifdef DEBUG
    fprintf(stderr, "[DEBUG hashTableLookup]   '%s' NOT found in bucket %d\n", name, index); // DIAGNOSTIC
#endif
    return NULL;
}

/**
 * Internal helper to insert a Symbol into a hash table.
 * Assumes the Symbol struct and its contents (name, value) are already allocated.
 * Handles collisions by adding to the head of the bucket's linked list.
 * Does NOT check for duplicates; callers should use hashTableLookup first if needed.
 *
 * @param table  A pointer to the HashTable to insert into.
 * @param symbol A pointer to the Symbol structure to insert.
 */
void hashTableInsert(HashTable *table, Symbol *symbol) {
    if (!table || !symbol || !symbol->name) {
        fprintf(stderr, "Internal error: Invalid parameters for hashTableInsert.\n");
        // Decide how to handle invalid input (exit or return)
        EXIT_FAILURE_HANDLER(); // Exiting on internal structure error
    }

    // Calculate the hash index for the symbol's name
    int index = hashFunctionName(symbol->name);

    // Insert the symbol at the head of the linked list in the bucket
    symbol->next = table->buckets[index]; // New symbol points to the current head
    table->buckets[index] = symbol;        // Bucket head now points to the new symbol

    DEBUG_PRINT("[DEBUG SYMBOL] Inserted Symbol '%s' at %p into bucket %d of HashTable %p.\n",
                symbol->name, (void*)symbol, index, (void*)table);
}


// --- Modified Public Symbol Table Functions ---

// Helper to search only in the global symbol table (now a HashTable).
Symbol *lookupGlobalSymbol(const char *name) {
    DEBUG_PRINT("[DEBUG SYMBOL] lookupGlobalSymbol: Searching for '%s' in globalSymbols %p.\n", name, (void*)globalSymbols);
    // Use the internal hash table lookup helper
    if (!globalSymbols) { // Defensive check if global table is not initialized
        fprintf(stderr, "Internal error: globalSymbols hash table is NULL.\n");
        return NULL;
    }
    return hashTableLookup(globalSymbols, name);
}

// Helper to search only in the local symbol table (now a HashTable).
Symbol *lookupLocalSymbol(const char *name) {
    DEBUG_PRINT("[DEBUG SYMBOL] lookupLocalSymbol: Searching for '%s' in localSymbols %p.\n", name, (void*)localSymbols);
    // Use the internal hash table lookup helper
    if (!localSymbols) { // Defensive check if local table is not initialized (valid state during global scope)
         DEBUG_PRINT("[DEBUG SYMBOL] lookupLocalSymbol: localSymbols is NULL, symbol '%s' not found locally.\n", name);
         return NULL;
    }
    return hashTableLookup(localSymbols, name);
}

/**
 * Looks up a symbol by name, checking the local scope first, then the global scope.
 * Exits the program if the symbol is not found in either scope.
 *
 * @param name The symbol name string to look up.
 * @return A pointer to the found Symbol structure.
 */
Symbol *lookupSymbol(const char *name) {
    // Check local scope first
    Symbol *sym = lookupLocalSymbol(name);

    // If not found in local scope, check global scope
    if (!sym) {
        sym = lookupGlobalSymbol(name);
    }

    // If the symbol is still not found in either scope, it's a runtime error.
    if (!sym) {
        fprintf(stderr, "Runtime error: Symbol '%s' not found.\n", name);
#ifdef DEBUG
        dumpSymbolTable(); // Dump the tables for debugging the error
#endif
        EXIT_FAILURE_HANDLER();
    }

    DEBUG_PRINT("[DEBUG SYMBOL] lookupSymbol: '%s' found, type=%s\n", name, varTypeToString(sym->type));

    return sym;
}


// --- Functions for inserting symbols (modified to use hashTableInsert) ---

/**
 * Inserts a new global symbol into the global symbol table (hash table).
 * Creates a new Symbol structure and initializes its Value.
 * Checks for duplicates before inserting (optional, current impl ignores duplicates).
 * Exits the program if memory allocation fails.
 *
 * @param name    The name of the symbol.
 * @param type    The VarType of the symbol.
 * @param type_def The AST node defining the type (used for complex types like records/arrays).
 */
void insertGlobalSymbol(const char *name, VarType type, AST *type_def) {
    if (!name || name[0] == '\0') {
        fprintf(stderr, "[ERROR] Attempted to insert global symbol with invalid name.\n");
        return;
    }

    // Check for duplicates before inserting (optional - currently just warns/returns)
    // If strict "duplicate identifier" errors are needed, this check should be here
    // before allocation. For now, let's match the previous behavior and allow
    // the parser to potentially warn, or just silently ignore duplicates during insertion.
     if (lookupGlobalSymbol(name)) {
         // DEBUG_PRINT("[Warning] Duplicate global symbol '%s' ignored during insertion.\n", name);
         return; // Do not insert if it already exists
     }


    // Allocate Symbol struct
    Symbol *new_symbol = malloc(sizeof(Symbol));
    if (!new_symbol) { fprintf(stderr, "Memory allocation error in insertGlobalSymbol (Symbol struct)\n"); EXIT_FAILURE_HANDLER(); }

    // Duplicate name
    new_symbol->name = strdup(name);
    if (!new_symbol->name) { fprintf(stderr, "Memory allocation error (strdup name) in insertGlobalSymbol\n"); free(new_symbol); EXIT_FAILURE_HANDLER(); }
    toLowerString(new_symbol->name);

    // Set basic fields
    new_symbol->type = type;
    new_symbol->is_alias = false;
    new_symbol->is_const = false;
    new_symbol->is_local_var = false; // Globals aren't local vars
    new_symbol->next = NULL; // Will be linked by hashTableInsert
    new_symbol->type_def = type_def; // Store type definition link
    new_symbol->is_defined = false; // Flag to indicate if the body has been compiled (useful for forward declarations)
    new_symbol->bytecode_address = 0; // Starting address (offset) in the bytecode chunk
    new_symbol->arity = 0; // Number of parameters
    new_symbol->locals_count = 0; // Number of local variables (excluding parameters)

    // Allocate the Value struct itself
    new_symbol->value = malloc(sizeof(Value));
    if (!new_symbol->value) { fprintf(stderr, "Memory allocation error (malloc Value) in insertGlobalSymbol\n"); free(new_symbol->name); free(new_symbol); EXIT_FAILURE_HANDLER(); }

    // Initialize the contents of the Value struct using the helper function
    *(new_symbol->value) = makeValueForType(type, type_def); // Use assignment to copy the returned Value

    DEBUG_PRINT("[DEBUG SYMBOL] Created Symbol '%s' at %p (Value @ %p, base_type_node @ %p).\n",
                new_symbol->name, (void*)new_symbol, (void*)new_symbol->value, (void*)(new_symbol->value ? new_symbol->value->base_type_node : NULL));

    // The globalSymbols hash table must be created before calling this function.
    if (!globalSymbols) {
         fprintf(stderr, "Internal error: globalSymbols hash table is NULL during insertGlobalSymbol.\n");
         // Clean up allocated symbol and value before exiting
         if (new_symbol->value) {
             freeValue(new_symbol->value); // Free the contents of the Value struct
             free(new_symbol->value);      // Then free the Value struct itself
         }
         if (new_symbol->name) {
             free(new_symbol->name);
         }
         free(new_symbol); // Free the Symbol struct
         EXIT_FAILURE_HANDLER();
    }
    hashTableInsert(globalSymbols, new_symbol);

    // The symbol is now owned by the hash table structure.
}


/**
 * Inserts a new local symbol into the local symbol table (hash table).
 * Creates a new Symbol structure and initializes its Value.
 * Checks if a symbol with the same name already exists in the local scope (case-insensitive).
 * Exits the program if memory allocation fails.
 *
 * @param name                 The name of the symbol.
 * @param type                 The VarType of the symbol.
 * @param type_def             The AST node defining the type.
 * @param is_variable_declaration Flag indicating if this is a variable declaration (affects popLocalEnv).
 * @return A pointer to the newly created Symbol, or the existing one if a duplicate was found.
 */
Symbol *insertLocalSymbol(const char *name, VarType type, AST* type_def, bool is_variable_declaration) {
    if (!name || name[0] == '\0') {
         fprintf(stderr, "[ERROR] Attempted to insert local symbol with invalid name.\n");
         return NULL; // Return NULL on error
    }

    // <<< MODIFIED: Check for existing local symbol using hash table lookup >>>
    // Check for existing local symbol (case-insensitive) in the current local scope
    if (!localSymbols) { // localSymbols must be created before calling this function
         fprintf(stderr, "Internal error: localSymbols hash table is NULL during insertLocalSymbol.\n");
         EXIT_FAILURE_HANDLER();
    }
    Symbol *existing = hashTableLookup(localSymbols, name);
    if (existing) {
        DEBUG_PRINT("[DEBUG SYMBOL] insertLocalSymbol: Symbol '%s' already exists in local scope, returning existing.\n", name);
        return existing; // Symbol already exists in this scope
    }
    // <<< END MODIFIED >>>


    // Create a new symbol if it doesn't exist locally
    Symbol *sym = malloc(sizeof(Symbol));
    if (!sym) {
        fprintf(stderr, "FATAL: malloc failed for Symbol struct in insertLocalSymbol for '%s'\n", name);
        EXIT_FAILURE_HANDLER();
    }

    DEBUG_PRINT("[DEBUG SYMBOL] insertLocalSymbol('%s', type=%s, is_var_decl=%d)\n", name, varTypeToString(type), is_variable_declaration);

    // Duplicate the name (lowercase not strictly needed anymore with strcasecmp in lookup/hash)
    sym->name = strdup(name);
    if (!sym->name) {
        fprintf(stderr, "FATAL: strdup failed for name in insertLocalSymbol for '%s'\n", name);
        free(sym);
        EXIT_FAILURE_HANDLER();
    }
    toLowerString(sym->name);

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

    DEBUG_PRINT("[DEBUG SYMBOL] Created Symbol '%s' at %p (Value @ %p, base_type_node @ %p).\n",
                sym->name, (void*)sym, (void*)sym->value, (void*)(sym->value ? sym->value->base_type_node : NULL));


    // Set flags
    sym->is_alias = false;
    sym->is_local_var = is_variable_declaration; // Mark as local variable for correct cleanup
    sym->is_const = false; // Local variables are not constants initially
    sym->next = NULL; // Will be linked by hashTableInsert


    // <<< MODIFIED: Insert into the local hash table >>>
    hashTableInsert(localSymbols, sym);
    // <<< END MODIFIED >>>

    // The symbol is now owned by the local hash table structure.
    return sym; // Return the newly created symbol
}

// --- Modified Environment Management Functions ---

/**
 * Saves the current local symbol environment (hash table) state.
 * Replaces the current localSymbols hash table with a new empty one
 * for the next scope, and stores the old one in the snapshot.
 *
 * @param snap Pointer to a SymbolEnvSnapshot structure to store the current environment.
 */
void saveLocalEnv(SymbolEnvSnapshot *snap) {
    if (!snap) {
        fprintf(stderr, "Internal error: saveLocalEnv called with NULL snapshot.\n");
        EXIT_FAILURE_HANDLER();
    }
    // Store the current local symbol table (hash table pointer) in the snapshot.
    snap->head = localSymbols;

    // Create a new empty hash table for the new local scope.
    localSymbols = createHashTable();
    DEBUG_PRINT("[DEBUG SYMBOL] Saved local env %p, created new empty local env %p.\n", (void*)snap->head, (void*)localSymbols);
}

/**
 * Restores the previous local symbol environment (hash table) from a snapshot.
 * Frees the current local symbol table and replaces it with the one from the snapshot.
 *
 * @param snap Pointer to a SymbolEnvSnapshot structure containing the previous environment.
 */
void restoreLocalEnv(SymbolEnvSnapshot *snap) {
    if (!snap) {
        fprintf(stderr, "Internal error: restoreLocalEnv called with NULL snapshot.\n");
        EXIT_FAILURE_HANDLER();
    }
    // Pop the current local symbol table (free its memory).
    // The current localSymbols table holds symbols inserted in the now-ending scope.
    DEBUG_PRINT("[DEBUG SYMBOL] Restoring local env. Freeing current local env %p.\n", (void*)localSymbols);
    freeHashTable(localSymbols);

    // Restore the previous local symbol table from the snapshot.
    localSymbols = snap->head;
    DEBUG_PRINT("[DEBUG SYMBOL] Restored local env to %p.\n", (void*)localSymbols);
}

/**
 * Pops the entire current local symbol environment (hash table).
 * Frees all memory associated with the local symbol table and sets localSymbols to NULL.
 * Note: This is similar to restoreLocalEnv but typically called at the end of the
 * main program block or when explicitly clearing all local scope.
 * In the current interpreter structure, restoreLocalEnv handles freeing when moving
 * between nested scopes. popLocalEnv might only be strictly needed for the
 * final cleanup of the outermost local scope (main block vars), or if restoreLocalEnv
 * isn't used for the outermost block. Let's adapt it to free the current local table.
 */
void popLocalEnv(void) {
    DEBUG_PRINT("[DEBUG SYMBOL] popLocalEnv: Freeing current local env %p.\n", (void*)localSymbols);
    // Free the current local symbol table.
    freeHashTable(localSymbols);

    // Set the local symbol table pointer to NULL.
    localSymbols = NULL;
    DEBUG_PRINT("[DEBUG SYMBOL] popLocalEnv: localSymbols set to NULL.\n");
}

// --- Other Symbol Table Functions (Implementation changes) ---

// updateSymbol implementation remains largely the same, but it uses lookupSymbol (which is modified).
// assignToRecord implementation remains the same.

/**
 * Looks up a symbol in a specific hash table environment.
 * This helper is used by save/restore and other internal functions.
 *
 * @param env A pointer to the HashTable environment to search.
 * @param name  The symbol name string to look up.
 * @return A pointer to the found Symbol, or NULL if not found.
 */
Symbol *lookupSymbolIn(HashTable *env, const char *name) {
     if (!env) return NULL; // Handle NULL environment safely
     return hashTableLookup(env, name); // Use the internal hash table lookup
}

/**
 * Dumps the global and local symbol tables (now hash tables).
 */
/**
 * Dumps the global and local symbol tables (now hash tables).
 * Iterates through each bucket in the hash table and the linked list of Symbols within it,
 * calling dumpSymbol for each Symbol.
 */
void dumpSymbolTable(void) {
    printf("--- Symbol Table Dump ---\n");

    // Dump Global Symbol Table
    printf("Global Symbols (%p):\n", (void*)globalSymbols);
    // Check if the global hash table itself is NULL (e.g., if initSymbolSystem failed or hasn't run).
    if (!globalSymbols) {
        printf("  (null)\n");
    } else {
        // Iterate through each bucket in the global hash table array.
        for (int i = 0; i < HASHTABLE_SIZE; ++i) {
            // Get the head of the linked list for the current bucket.
            Symbol *sym = globalSymbols->buckets[i]; // <<< Correctly starts traversal from the bucket
            // Only print if the bucket is not empty.
            if (sym) {
                printf("  Bucket %d:\n", i);
                // Traverse the linked list of Symbols within the current bucket.
                while (sym) {
                    printf("    "); // Indent symbols within a bucket for clarity.
                    // Call dumpSymbol to print the details of this individual Symbol.
                    // The prototype for dumpSymbol should be available via utils.h.
                    dumpSymbol(sym); // Assumes dumpSymbol is defined and prototyped.
                    sym = sym->next; // Move to the next Symbol in the linked list.
                }
            }
        }
    }

    // Dump Local Symbol Table
    printf("Local Symbols (%p):\n", (void*)localSymbols);
    // Check if the local hash table itself is NULL.
    // localSymbols will be NULL in the global scope before any function calls.
    if (!localSymbols) {
        printf("  (null)\n");
    } else {
         // Iterate through each bucket in the local hash table array.
        for (int i = 0; i < HASHTABLE_SIZE; ++i) {
             // Get the head of the linked list for the current bucket.
            Symbol *sym = localSymbols->buckets[i]; // <<< Correctly starts traversal from the bucket
             // Only print if the bucket is not empty.
            if (sym) {
                printf("  Bucket %d:\n", i);
                // Traverse the linked list of Symbols within the current bucket.
                while (sym) {
                    printf("    "); // Indent symbols within a bucket.
                    // Call dumpSymbol to print the details of this individual Symbol.
                    // The prototype for dumpSymbol should be available via utils.h.
                    dumpSymbol(sym); // Assumes dumpSymbol is defined and prototyped.
                    sym = sym->next; // Move to the next Symbol in the linked list.
                }
            }
        }
    }

    printf("--- End of Symbol Table Dump ---\n");
}
// --- Modified Pointer Alias Nullification ---
// Modify nullifyPointerAliasesByAddrValue to iterate over HashTable buckets
void nullifyPointerAliasesByAddrValue(HashTable* table, uintptr_t disposedAddrValue) {
    if (!table) return;

    // Iterate through each bucket
    for (int i = 0; i < HASHTABLE_SIZE; ++i) {
        // Traverse the linked list in the current bucket
        Symbol* current = table->buckets[i];
        while (current) {
            // Compare the stored pointer address (cast to integer) with the disposed address value
            if (current->value && current->type == TYPE_POINTER &&
                ((uintptr_t)current->value->ptr_val) == disposedAddrValue) {
                #ifdef DEBUG
                fprintf(stderr, "[DEBUG DISPOSE] Nullifying alias '%s' in bucket %d which pointed to disposed memory address 0x%lx.\n",
                        current->name ? current->name : "?", i, disposedAddrValue);
                #endif
                current->value->ptr_val = NULL; // Set the alias pointer to nil
            }
            current = current->next;
        }
    }
}

/**
 * Update the value of an existing symbol in the symbol tables.
 * Looks up the symbol by name (first in local scope, then global).
 * Assigns the new value, handling type compatibility, memory management for complex types,
 * and preventing assignment to constants.
 * Exits the program with a runtime error if the symbol is not found, is a constant,
 * or if there's a type mismatch or memory allocation failure during assignment.
 *
 * @param name The name of the symbol to update.
 * @param val  The new Value to assign to the symbol. A copy is made if necessary.
 */
void updateSymbol(const char *name, Value val) {
    // Look up the symbol in the symbol tables (local then global).
    // lookupSymbol handles the "symbol not found" error and exits if necessary.
    Symbol *sym = lookupSymbol(name);

    #ifdef DEBUG // Debug print to show which symbol is being updated and its properties
    fprintf(stderr, "[DEBUG updateSymbol] Attempting to update symbol '%s' (Type: %s, Value @ %p, is_const: %d, is_alias: %d, is_local_var: %d). Incoming value type: %s\n",
            name, varTypeToString(sym->type), (void*)(sym ? sym->value : NULL),
            sym ? sym->is_const : -1, sym ? sym->is_alias : -1, sym ? sym->is_local_var : -1,
            varTypeToString(val.type));
    fflush(stderr); // Flush debug output immediately
    #endif

    // Check if the symbol is a constant. Constants cannot be reassigned.
    if (sym->is_const) {
        fprintf(stderr, "Runtime error: Cannot assign to constant '%s'.\n", name);
        // Free the incoming value 'val' as it won't be used for assignment.
        freeValue(&val); // Assumes freeValue is defined and works for all Value types.
        EXIT_FAILURE_HANDLER(); // Terminate the program.
    }

    // Defensive check: Ensure the Symbol has an allocated Value structure to update.
    // This should be true if insertGlobalSymbol/insertLocalSymbol worked correctly (they allocate sym->value).
    if (!sym->value) {
        fprintf(stderr, "Runtime error: Symbol '%s' has NULL value pointer during assignment.\n", name);
        // Free the incoming value 'val'.
        freeValue(&val);
        EXIT_FAILURE_HANDLER();
    }

    // --- Type Compatibility Check ---
    // Check if the type of the new value is compatible with the symbol's declared type.
    // Implement type coercion/promotion rules here based on Pascal semantics.
    bool types_compatible = false;
    if (sym->type == val.type) {
        types_compatible = true; // Exact type match
    } else {
        // Handle specific allowed coercions and promotions.
        // Example: Integer to Real is allowed. Single Char to String is allowed.
        // Integer/Byte/Word/Boolean/Char to Integer/Real might be allowed depending on operation/context.
        // Pointers can only be assigned to compatible pointers or nil.

        if (sym->type == TYPE_REAL && val.type == TYPE_INTEGER) types_compatible = true; // Integer to Real promotion
        else if (sym->type == TYPE_INTEGER && val.type == TYPE_REAL) { /* Real to Integer requires explicit Trunc/Round */ types_compatible = false; } // No implicit Real to Integer
        else if (sym->type == TYPE_STRING && val.type == TYPE_CHAR) types_compatible = true; // Char to String promotion (creates a 1-char string)
        else if (sym->type == TYPE_CHAR && val.type == TYPE_STRING && val.s_val && strlen(val.s_val) == 1) types_compatible = true; // Single-char String to Char
        else if (sym->type == TYPE_INTEGER && (val.type == TYPE_BYTE || val.type == TYPE_WORD || val.type == TYPE_BOOLEAN || val.type == TYPE_CHAR)) types_compatible = true; // Ordinal to Integer
        else if ((sym->type == TYPE_BYTE || sym->type == TYPE_WORD || sym->type == TYPE_BOOLEAN || sym->type == TYPE_CHAR) && val.type == TYPE_INTEGER) types_compatible = true; // Integer to smaller Ordinal (potential truncation/warning handled below)
        else if (sym->type == TYPE_BOOLEAN && val.type == TYPE_INTEGER) types_compatible = true; // Integer to Boolean (0=False, non-0=True)
        else if (sym->type == TYPE_ENUM && val.type == TYPE_ENUM) { // Enum to same Enum type
             // Strict check: require enum names to match (case-sensitive?)
             if ((sym->value->enum_val.enum_name == NULL && val.enum_val.enum_name == NULL) ||
                 (sym->value->enum_val.enum_name != NULL && val.enum_val.enum_name != NULL &&
                  strcmp(sym->value->enum_val.enum_name, val.enum_val.enum_name) == 0)) {
                 types_compatible = true;
             } else {
                 #ifdef DEBUG
                 fprintf(stderr, "[DEBUG updateSymbol] Enum type mismatch: Cannot assign enum '%s' to enum '%s'.\n",
                         val.enum_val.enum_name ? val.enum_val.enum_name : "?",
                         sym->value->enum_val.enum_name ? sym->value->enum_val.enum_name : "?");
                 #endif
                 types_compatible = false;
             }
        }
        else if (sym->type == TYPE_ENUM && val.type == TYPE_INTEGER) types_compatible = true; // Integer to Enum (assign by ordinal value)
        else if (sym->type == TYPE_POINTER && (val.type == TYPE_POINTER || val.type == TYPE_NIL)) types_compatible = true; // Pointer to Pointer or Nil

        // Add other compatible type checks as needed (e.g., Set to Set)
        else if (sym->type == TYPE_SET && val.type == TYPE_SET) types_compatible = true;
        else if (sym->type == TYPE_MEMORYSTREAM && val.type == TYPE_MEMORYSTREAM) types_compatible = true; // Shallow copy likely intended
        else if (sym->type == TYPE_FILE && val.type == TYPE_FILE) {
             fprintf(stderr, "Runtime error: Direct assignment of FILE variables is not supported.\n"); // Explicitly disallow File assignment
             freeValue(&val); // Free incoming value
             EXIT_FAILURE_HANDLER();
        }
    }


    if (!types_compatible) {
        fprintf(stderr, "Runtime error: Type mismatch. Cannot assign %s to %s for symbol '%s'.\n",
                varTypeToString(val.type), varTypeToString(sym->type), name);
        fprintf(stderr, "%d", val.type);
        freeValue(&val); // Free incoming value
        EXIT_FAILURE_HANDLER(); // Terminate the program on type mismatch.
    }
    // --- End Type Compatibility Check ---


    // --- Free Old Value Contents (if necessary) ---
    // For complex types and strings, free the dynamically allocated content
    // BEFORE overwriting the Value union or pointer.
    // For simple types, direct assignment of the union member is fine;
    // no dynamic memory is owned by the Value struct itself for them.
    // For pointers, freeValue only sets ptr_val to NULL, doesn't free pointed-to data.
    // We only need to free the *contents* if the target symbol's Value struct
    // currently holds dynamic memory for its type.
    // The freeValue function handles the logic for different types.
    // We free the OLD value's contents *before* the assignment.
    // We should NOT free the Value struct itself (sym->value), only its contents.
    freeValue(sym->value); // Free the contents of the target symbol's current value.


    // --- Perform Assignment ---
    // Copy the new value into the symbol's value storage.
    // Use makeCopyOfValue for complex types to ensure a deep copy.
    // For simple types or shallow-copied pointers/handles, direct struct copy or member assignment is sufficient.

    // Let's use a switch based on the *symbol's declared type* (sym->type)
    // to handle the assignment correctly, potentially promoting the incoming value (val) if needed.

    switch (sym->type) {
        case TYPE_INTEGER:
            // Coerce compatible types to Integer.
            if (val.type == TYPE_INTEGER || val.type == TYPE_BYTE || val.type == TYPE_WORD || val.type == TYPE_BOOLEAN) sym->value->i_val = val.i_val;
            else if (val.type == TYPE_CHAR) sym->value->i_val = (long long)val.c_val; // Char to Integer (ASCII)
            else if (val.type == TYPE_REAL) sym->value->i_val = (long long)val.r_val; // Real to Integer (truncation)
            break;
            
        case TYPE_REAL:
            // Coerce compatible types to Real.
            if (val.type == TYPE_REAL) sym->value->r_val = val.r_val;
            else if (val.type == TYPE_INTEGER) sym->value->r_val = (double)val.i_val; // Integer to Real promotion
            else if (val.type == TYPE_CHAR) sym->value->r_val = (double)val.c_val; // Char to Real promotion
            break;
            
        case TYPE_BYTE:
            // Coerce compatible types to Byte.
            if (val.type == TYPE_INTEGER || val.type == TYPE_BYTE || val.type == TYPE_WORD) {
                // Check for potential overflow/underflow when assigning Integer to Byte.
                if (val.i_val < 0 || val.i_val > 255) {
                    fprintf(stderr, "Runtime warning: Assignment to BYTE variable '%s' out of range (0-255). Value %lld will be truncated.\n", name, val.i_val);
                }
                sym->value->i_val = (val.i_val & 0xFF); // Store the lower 8 bits.
            } else if (val.type == TYPE_CHAR) {
                sym->value->i_val = (long long)val.c_val; // Char to Byte (ASCII)
            } else if (val.type == TYPE_BOOLEAN) {
                sym->value->i_val = val.i_val; // Boolean (0/1) to Byte
            }
            break;
            
        case TYPE_WORD:
            // Coerce compatible types to Word.
            if (val.type == TYPE_INTEGER || val.type == TYPE_BYTE || val.type == TYPE_WORD) {
                // Check for potential overflow/underflow when assigning Integer to Word.
                if (val.i_val < 0 || val.i_val > 65535) {
                    fprintf(stderr, "Runtime warning: Assignment to WORD variable '%s' out of range (0-65535). Value %lld will be truncated.\n", name, val.i_val);
                }
                sym->value->i_val = (val.i_val & 0xFFFF); // Store the lower 16 bits.
            } else if (val.type == TYPE_CHAR) {
                sym->value->i_val = (long long)val.c_val; // Char to Word (ASCII)
            } else if (val.type == TYPE_BOOLEAN) {
                sym->value->i_val = val.i_val; // Boolean (0/1) to Word
            }
            break;
            
            
        case TYPE_STRING: {
            // Free existing string data in the target symbol's value (already done by freeValue(sym->value)).
            // Create and assign a new string based on the incoming value.
            const char* source_str = NULL;
            char char_buf[2]; // Buffer for potential Char to String conversion
            
            // Get the source string pointer, handling incoming CHAR or STRING.
            if (val.type == TYPE_STRING) {
                source_str = val.s_val;
            } else if (val.type == TYPE_CHAR) { // Allow assigning CHAR to STRING (creates a 1-char string).
                char_buf[0] = val.c_val;
                char_buf[1] = '\0'; // Null-terminate the buffer.
                source_str = char_buf;
            }
            // Handle NULL source string defensively.
            if (!source_str) source_str = "";
            
            // Handle fixed vs dynamic length string assignment.
            if (sym->value->max_length > 0) { // Target is a fixed-length string.
                size_t source_len = strlen(source_str);
                // Copy up to the maximum length of the target string.
                size_t copy_len = (source_len > (size_t)sym->value->max_length) ? (size_t)sym->value->max_length : source_len;
                
                // Allocate memory for the fixed-length string (max_length + 1 for null terminator).
                sym->value->s_val = malloc((size_t)sym->value->max_length + 1);
                if (!sym->value->s_val) {
                    fprintf(stderr, "FATAL: Memory allocation failed for fixed string assignment to '%s'.\n", name);
                    // Free incoming value 'val' (especially if it's a String/Record/Array/Set/Enum etc.)
                    freeValue(&val);
                    EXIT_FAILURE_HANDLER();
                }
                // Copy the source string content into the allocated buffer.
                strncpy(sym->value->s_val, source_str, copy_len);
                // Ensure null termination at the end of the copied content.
                sym->value->s_val[copy_len] = '\0';
                
                // The buffer is already null-terminated at max_length + 1.
                
            } else { // Target is a dynamic string (max_length is -1).
                // Create a duplicate of the source string.
                sym->value->s_val = strdup(source_str);
                if (!sym->value->s_val) {
                    fprintf(stderr, "FATAL: Memory allocation failed for dynamic string assignment to '%s'.\n", name);
                    freeValue(&val);
                    EXIT_FAILURE_HANDLER();
                }
            }
            // The type of the symbol's value remains TYPE_STRING.
            break;
        }
        case TYPE_RECORD:
             // Free existing record content (already done by freeValue(sym->value)).
             // Make a deep copy of the incoming record value and assign it.
             *(sym->value) = makeCopyOfValue(&val); // makeCopyOfValue handles deep copy.
             // Ensure the type is correct (should be copied by makeCopyOfValue, but double check).
             sym->value->type = TYPE_RECORD;
             break;

        case TYPE_BOOLEAN:
             // Coerce compatible types to Boolean.
             if (val.type == TYPE_BOOLEAN) sym->value->i_val = val.i_val; // Boolean to Boolean (0/1)
             else if (val.type == TYPE_INTEGER) sym->value->i_val = (val.i_val != 0) ? 1 : 0; // Integer to Boolean (0=False, non-0=True)
             break;

        case TYPE_FILE:
             fprintf(stderr, "Runtime error: Direct assignment of FILE variables is not supported.\n");
             // Free incoming value 'val' (especially if it's a File Value).
             freeValue(&val);
             EXIT_FAILURE_HANDLER();
             break; // Should not be reached

        case TYPE_ARRAY:
             // Free existing array content (already done by freeValue(sym->value)).
             // Make a deep copy of the incoming array value and assign it.
             *(sym->value) = makeCopyOfValue(&val); // makeCopyOfValue handles deep copy.
             // Ensure the type is correct.
             sym->value->type = TYPE_ARRAY;
             break;

        case TYPE_CHAR:
             // Coerce compatible types to Char.
             if (val.type == TYPE_CHAR) sym->value->c_val = val.c_val; // Char to Char
             else if (val.type == TYPE_STRING && val.s_val && strlen(val.s_val) == 1) sym->value->c_val = val.s_val[0]; // Single-char String to Char
             else if (val.type == TYPE_INTEGER) { // Integer to Char (by ordinal value)
                  if (val.i_val < 0 || val.i_val > 255) { // Check ASCII range
                      fprintf(stderr, "Runtime warning: Assignment to CHAR variable '%s' out of ASCII range (0-255). Value %lld will be truncated.\n", name, val.i_val);
                  }
                  sym->value->c_val = (char)(val.i_val & 0xFF); // Take lower 8 bits as char.
             }
             sym->value->type = TYPE_CHAR; // Ensure the type is set.
             break;

        case TYPE_MEMORYSTREAM:
              freeValue(sym->value); // Frees the *old* MStream struct and its buffer pointed to by sym->value->mstream

              // Note: freeValue sets v->mstream to NULL on the Value it operates on, but sym->value is the container.
              // The sym->value struct itself is not changed by freeValue, only its members like sym->value->mstream.

              Value temp_copy = makeCopyOfValue(&val);
              
              *(sym->value) = temp_copy; // Assign the shallow copy to the symbol's value struct.
                                         // This copies all fields of Value, including the mstream pointer.
              
              // Ensure the symbol's type is correctly set to MEMORYSTREAM,
              // especially if makeCopyOfValue only did a shallow struct copy and val's type was different somehow.
              // However, makeCopyOfValue starts with v = *src, so type should be copied.
              sym->value->type = TYPE_MEMORYSTREAM;
              fflush(stderr);
              break;
        case TYPE_ENUM:
             // Free existing enum name string if present (freeValue(sym->value) handles this).
             // Assign the new enum value.
             if (val.type == TYPE_ENUM) { // Enum to same/compatible Enum type
                  // Free the old enum name string from the symbol's value.
                  if(sym->value->enum_val.enum_name) free(sym->value->enum_val.enum_name);

                  // Copy the new enum value's name and ordinal.
                  sym->value->enum_val.enum_name = val.enum_val.enum_name ? strdup(val.enum_val.enum_name) : NULL;
                  sym->value->enum_val.ordinal = val.enum_val.ordinal;
             } else if (val.type == TYPE_INTEGER) { // Integer to Enum (assign by ordinal)
                  // Check if the ordinal is within the valid range for the enum type.
                  // This requires looking up the enum type definition.
                  AST* typeDef = sym->type_def; // Get the AST node for the symbol's type definition.
                  if (typeDef && typeDef->type == AST_TYPE_REFERENCE) typeDef = typeDef->right; // Resolve reference.

                  long long maxOrdinal = -1;
                  if (typeDef && typeDef->type == AST_ENUM_TYPE) { maxOrdinal = typeDef->child_count - 1; }
                  else {
                      #ifdef DEBUG
                       fprintf(stderr, "[DEBUG updateSymbol] Cannot determine enum definition for range check on type '%s' assigned to '%s'.\n",
                               val.enum_val.enum_name ? val.enum_val.enum_name : "?",
                               name);
                       #endif
                       // Continue without range check if definition is not found.
                  }

                  if (maxOrdinal != -1 && (val.i_val < 0 || val.i_val > maxOrdinal)) {
                       fprintf(stderr, "Runtime warning: Assignment to ENUM variable '%s' out of range (0..%lld). Value %lld is invalid.\n", name, maxOrdinal, val.i_val);
                       // Decide how to handle out-of-range assignment (clamp, error, or assign anyway).
                       // Let's assign the ordinal value but warn.
                  }
                  // Free the old enum name string.
                  if(sym->value->enum_val.enum_name) free(sym->value->enum_val.enum_name);
                  sym->value->enum_val.enum_name = NULL; // No name associated with integer assignment.
                  sym->value->enum_val.ordinal = (int)val.i_val; // Assign the integer ordinal value.
             }
             sym->value->type = TYPE_ENUM; // Ensure type is correct.
             break;

        case TYPE_SET:
             // Free existing set content (freeValue(sym->value) handles this).
             // Make a deep copy of the incoming set value and assign it.
             *(sym->value) = makeCopyOfValue(&val); // makeCopyOfValue handles deep copy.
             // Ensure the type is correct.
             sym->value->type = TYPE_SET;
             break;

        case TYPE_POINTER:
            // Handling assignment to a POINTER symbol.
            // The incoming value's type is already checked to be TYPE_POINTER or TYPE_NIL.

            #ifdef DEBUG // ADDED DEBUG PRINTS IN POINTER CASE
            fprintf(stderr, "[DEBUG updateSymbol] Assigning to POINTER symbol '%s'. Incoming value type: %s\n",
                             name, varTypeToString(val.type));
            if (val.type == TYPE_POINTER) {
                fprintf(stderr, "[DEBUG updateSymbol]   Incoming pointer value: %p\n", (void*)val.ptr_val);
            } else if (val.type == TYPE_NIL) {
                fprintf(stderr, "[DEBUG updateSymbol]   Incoming value is NIL (assigning NULL to ptr_val).\n");
            }
            #endif // END ADDED DEBUG PRINTS

            // Assign the pointer value from the incoming Value.
            // If val.type is TYPE_NIL, val.ptr_val is expected to be NULL,
            // and assigning NULL to sym->value->ptr_val correctly sets the pointer to nil.
            sym->value->ptr_val = val.ptr_val;

            // The base_type_node represents the declared type of the pointer variable
            // and should NOT change during assignment. It was set during symbol insertion.
            // sym->value->base_type_node = ... // DO NOT CHANGE THIS HERE

            // Ensure the symbol's value type is correctly set to POINTER.
            // This is crucial because the Value struct is copied into sym->value below.
            // The incoming 'val' might be TYPE_NIL, but the symbol is TYPE_POINTER.
            sym->value->type = TYPE_POINTER;

            break;

        case TYPE_VOID:
             // Should not be able to assign to a VOID type symbol.
             fprintf(stderr, "Runtime error: Attempted assignment to VOID type symbol '%s'.\n", name);
             freeValue(&val);
             EXIT_FAILURE_HANDLER();
             break; // Should not be reached.

        default:
            // Handle other unhandled target types defensively.
            fprintf(stderr, "Runtime error: unhandled target type (%s) in updateSymbol assignment logic for '%s'.\n",
                    varTypeToString(sym->type), name);
            freeValue(&val);
            EXIT_FAILURE_HANDLER();
            break; // Should not be reached.
    }

    // Free the incoming Value struct's *contents* now that its data has been used
    // for assignment or copied into the symbol's value.
    // This is crucial for complex types where 'val' might hold dynamic memory.
    // For simple types, freeValue is a no-op for contents but safe to call.
    // Note: 'val' itself (the struct on the stack) is not freed here, only its contents.
    // freeValue(&val); // Frees contents like val.s_val, val.record_val, etc.

    #ifdef DEBUG // Optional debug print after assignment
    fprintf(stderr, "[DEBUG updateSymbol] Assignment to '%s' successful. Final value type: %s\n",
            name, varTypeToString(sym->value->type));
    #endif
}
