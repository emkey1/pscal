//
//  symbol.c
//  pscal
//
//  Created by Michael Miller on 3/25/25.
//
#include "Pascal/lexer.h"
#include "core/utils.h"
#include "Pascal/globals.h"
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include "symbol/symbol.h"
#include <assert.h>
#include "core/list.h"
#include "ast/ast.h"

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
    table->parent = NULL; // No parent by default
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
        Symbol *current = table->buckets[i];
        while (current) {
            Symbol *next = current->next;
            
            if (current->name) {
                free(current->name);
                current->name = NULL;
            }
            
            // If it's an alias, it doesn't own the other resources.
            if (!current->is_alias) {
                if (current->value) {
                    freeValue(current->value);
                    free(current->value);
                }
                if (current->type_def) {
                    freeAST(current->type_def);
                }
            }
            
            free(current);
            current = next;
        }
        table->buckets[i] = NULL;
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

static bool ensureGlobalSymbolsTable(void) {
    if (globalSymbols) {
        return true;
    }
    globalSymbols = createHashTable();
    if (!globalSymbols) {
        EXIT_FAILURE_HANDLER();
        return false;
    }
    return true;
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
        globalSymbols = createHashTable();
    }
    return hashTableLookup(globalSymbols, name);
}

// Helper to search only in the local symbol table (now a HashTable).
Symbol *lookupLocalSymbol(const char *name) {
    DEBUG_PRINT("[DEBUG SYMBOL] lookupLocalSymbol: Searching for '%s' starting at localSymbols %p.\n", name, (void*)localSymbols);
    HashTable *tbl = localSymbols;
    while (tbl) {
        Symbol *sym = hashTableLookup(tbl, name);
        if (sym) return sym;
        tbl = tbl->parent;
    }
    return NULL;
}

/**
 * Looks up a symbol by name, checking the local scope first, then the global scope.
 * Exits the program if the symbol is not found in either scope.
 *
 * @param name The symbol name string to look up.
 * @return A pointer to the found Symbol structure.
 */
Symbol *lookupSymbolOptional(const char *name) {
    Symbol *sym = lookupLocalSymbol(name);
    if (!sym) {
        sym = lookupGlobalSymbol(name);
    }
    return sym;
}

Symbol *lookupSymbol(const char *name) {
    Symbol *sym = lookupSymbolOptional(name);
    if (!sym) {
        fprintf(stderr, "Runtime error: Symbol '%s' not found.\n", name);
#ifdef DEBUG
        dumpSymbolTable();
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

    // If the provided type definition is an enum, force the symbol type to TYPE_ENUM.
    if (type_def) {
        AST *def = type_def;
        if (def->type == AST_TYPE_REFERENCE && def->right) {
            def = def->right;
        }
        if (def->type == AST_ENUM_TYPE) {
            type = TYPE_ENUM;
        }
    }

    if (!globalSymbols) {
        globalSymbols = createHashTable();
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
    new_symbol->is_inline = false;
    new_symbol->closure_captures = false;
    new_symbol->closure_escapes = false;
    new_symbol->next = NULL; // Will be linked by hashTableInsert
    new_symbol->enclosing = NULL;
    new_symbol->type_def = type_def ? copyAST(type_def) : NULL; // Store a DEEP COPY of the type definition
    new_symbol->is_defined = false; // Flag to indicate if the body has been compiled (useful for forward declarations)
    new_symbol->bytecode_address = 0; // Starting address (offset) in the bytecode chunk
    new_symbol->arity = 0; // Number of parameters
    new_symbol->locals_count = 0; // Number of local variables (excluding parameters)
    new_symbol->upvalue_count = 0;

    // Allocate the Value struct itself
    new_symbol->value = malloc(sizeof(Value));
    if (!new_symbol->value) { fprintf(stderr, "Memory allocation error (malloc Value) in insertGlobalSymbol\n"); free(new_symbol->name); free(new_symbol); EXIT_FAILURE_HANDLER(); }

    // Initialize the contents of the Value struct using the helper function
    *(new_symbol->value) = makeValueForType(type, type_def, new_symbol); // Use assignment to copy the returned Value

    // If this symbol ultimately represents an enum, ensure the Value carries
    // the enum metadata (name/ordinal) so the VM can reason about it later.
    if (type == TYPE_ENUM && new_symbol->value) {
        AST *def = type_def;
        if (def && def->type == AST_TYPE_REFERENCE && def->right) {
            def = def->right;
        }
        if (def && def->type == AST_ENUM_TYPE && def->token && def->token->value) {
            if (new_symbol->value->enum_val.enum_name) {
                free(new_symbol->value->enum_val.enum_name);
            }
            new_symbol->value->enum_val.enum_name = strdup(def->token->value);
            new_symbol->value->enum_val.ordinal = 0;
            new_symbol->value->base_type_node = def;
        }
    }

    DEBUG_PRINT("[DEBUG SYMBOL] Created Symbol '%s' at %p (Value @ %p, base_type_node @ %p).\n",
                new_symbol->name, (void*)new_symbol, (void*)new_symbol->value, (void*)(new_symbol->value ? new_symbol->value->base_type_node : NULL));

    if (!ensureGlobalSymbolsTable()) {
        if (new_symbol->value) {
            freeValue(new_symbol->value);
            free(new_symbol->value);
        }
        free(new_symbol->name);
        free(new_symbol);
        return;
    }
    hashTableInsert(globalSymbols, new_symbol);

    // The symbol is now owned by the hash table structure.
}

void insertGlobalAlias(const char *name, Symbol *target) {
    if (!name || name[0] == '\0' || !target) {
        return;
    }

    if (!ensureGlobalSymbolsTable()) {
        return;
    }

    if (lookupGlobalSymbol(name)) {
        return;
    }

    Symbol *resolved = resolveSymbolAlias(target);
    if (!resolved) {
        return;
    }

    Symbol *alias = calloc(1, sizeof(Symbol));
    if (!alias) {
        fprintf(stderr, "Memory allocation error in insertGlobalAlias (Symbol struct)\n");
        EXIT_FAILURE_HANDLER();
    }

    alias->name = strdup(name);
    if (!alias->name) {
        free(alias);
        fprintf(stderr, "Memory allocation error (strdup name) in insertGlobalAlias\n");
        EXIT_FAILURE_HANDLER();
    }
    toLowerString(alias->name);

    alias->type = resolved->type;
    alias->value = resolved->value;
    alias->is_const = resolved->is_const;
    alias->is_local_var = false;
    alias->is_inline = resolved->is_inline;
    alias->closure_captures = resolved->closure_captures;
    alias->closure_escapes = resolved->closure_escapes;
    alias->type_def = resolved->type_def;
    alias->next = NULL;
    alias->enclosing = NULL;
    alias->is_alias = true;
    alias->real_symbol = resolved;
    alias->is_defined = resolved->is_defined;
    alias->bytecode_address = resolved->bytecode_address;
    alias->arity = resolved->arity;
    alias->locals_count = resolved->locals_count;
    alias->slot_index = resolved->slot_index;
    alias->upvalue_count = resolved->upvalue_count;
    if (alias->upvalue_count > 0) {
        memcpy(alias->upvalues, resolved->upvalues, sizeof(resolved->upvalues));
    }

    hashTableInsert(globalSymbols, alias);
}

// Insert a constant symbol into constGlobalSymbols.
// Stores a copy of the provided Value and marks the symbol as const.
void insertConstGlobalSymbol(const char *name, Value val) {
    if (!name || name[0] == '\0') {
        fprintf(stderr, "[ERROR] Attempted to insert const symbol with invalid name.\n");
        return;
    }
    if (!constGlobalSymbols) {
        fprintf(stderr, "Internal error: constGlobalSymbols hash table is NULL during insertConstGlobalSymbol.\n");
        constGlobalSymbols = createHashTable();
        if (!constGlobalSymbols) {
            EXIT_FAILURE_HANDLER();
            return;
        }
    }
    Symbol *existing = hashTableLookup(constGlobalSymbols, name);
    if (existing) {
        existing->type = val.type;
        existing->is_const = true;
        if (existing->value) {
            freeValue(existing->value);
        } else {
            existing->value = malloc(sizeof(Value));
            if (!existing->value) {
                fprintf(stderr, "Memory allocation error (malloc Value) in insertConstGlobalSymbol\n");
                EXIT_FAILURE_HANDLER();
            }
        }
        *(existing->value) = makeCopyOfValue(&val);
        return;
    }

    Symbol *new_symbol = malloc(sizeof(Symbol));
    if (!new_symbol) {
        fprintf(stderr, "Memory allocation error in insertConstGlobalSymbol (Symbol struct)\n");
        EXIT_FAILURE_HANDLER();
    }
    new_symbol->name = strdup(name);
    if (!new_symbol->name) {
        free(new_symbol);
        fprintf(stderr, "Memory allocation error (strdup name) in insertConstGlobalSymbol\n");
        EXIT_FAILURE_HANDLER();
    }
    toLowerString(new_symbol->name);

    new_symbol->type = val.type;
    new_symbol->is_alias = false;
    new_symbol->is_const = true;
    new_symbol->is_local_var = false;
    new_symbol->is_inline = false;
    new_symbol->closure_captures = false;
    new_symbol->closure_escapes = false;
    new_symbol->next = NULL;
    new_symbol->type_def = NULL;
    new_symbol->enclosing = NULL;
    new_symbol->real_symbol = NULL;
    new_symbol->is_defined = false;
    new_symbol->bytecode_address = 0;
    new_symbol->arity = 0;
    new_symbol->locals_count = 0;
    new_symbol->slot_index = 0;
    new_symbol->upvalue_count = 0;

    new_symbol->value = malloc(sizeof(Value));
    if (!new_symbol->value) {
        free(new_symbol->name);
        free(new_symbol);
        fprintf(stderr, "Memory allocation error (malloc Value) in insertConstGlobalSymbol\n");
        EXIT_FAILURE_HANDLER();
    }
    *(new_symbol->value) = makeCopyOfValue(&val);

    hashTableInsert(constGlobalSymbols, new_symbol);
}

// Insert a constant symbol into the specified hash table.
// Used for class-scoped constants so they do not pollute the global table.
void insertConstSymbolIn(HashTable *table, const char *name, Value val) {
    if (!table || !name || name[0] == '\0') {
        fprintf(stderr, "[ERROR] Attempted to insert const symbol with invalid name.\n");
        return;
    }
    if (hashTableLookup(table, name)) {
        return; // Already inserted
    }

    Symbol *new_symbol = malloc(sizeof(Symbol));
    if (!new_symbol) {
        fprintf(stderr, "Memory allocation error in insertConstSymbolIn (Symbol struct)\n");
        EXIT_FAILURE_HANDLER();
    }
    new_symbol->name = strdup(name);
    if (!new_symbol->name) {
        free(new_symbol);
        fprintf(stderr, "Memory allocation error (strdup name) in insertConstSymbolIn\n");
        EXIT_FAILURE_HANDLER();
    }
    toLowerString(new_symbol->name);

    new_symbol->type = val.type;
    new_symbol->is_alias = false;
    new_symbol->is_const = true;
    new_symbol->is_local_var = false;
    new_symbol->is_inline = false;
    new_symbol->closure_captures = false;
    new_symbol->closure_escapes = false;
    new_symbol->next = NULL;
    new_symbol->type_def = NULL;
    new_symbol->enclosing = NULL;
    new_symbol->real_symbol = NULL;
    new_symbol->is_defined = false;
    new_symbol->bytecode_address = 0;
    new_symbol->arity = 0;
    new_symbol->locals_count = 0;
    new_symbol->slot_index = 0;
    new_symbol->upvalue_count = 0;

    new_symbol->value = malloc(sizeof(Value));
    if (!new_symbol->value) {
        free(new_symbol->name);
        free(new_symbol);
        fprintf(stderr, "Memory allocation error (malloc Value) in insertConstSymbolIn\n");
        EXIT_FAILURE_HANDLER();
    }
    *(new_symbol->value) = makeCopyOfValue(&val);

    hashTableInsert(table, new_symbol);
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
    *(sym->value) = makeValueForType(type, type_def, sym); // Initialize using helper

    DEBUG_PRINT("[DEBUG SYMBOL] Created Symbol '%s' at %p (Value @ %p, base_type_node @ %p).\n",
                sym->name, (void*)sym, (void*)sym->value, (void*)(sym->value ? sym->value->base_type_node : NULL));


    // Set flags
    sym->is_alias = false;
    sym->is_local_var = is_variable_declaration; // Mark as local variable for correct cleanup
    sym->is_const = false; // Local variables are not constants initially
    sym->is_inline = false;
    sym->closure_captures = false;
    sym->closure_escapes = false;
    sym->next = NULL; // Will be linked by hashTableInsert
    sym->enclosing = NULL;
    sym->upvalue_count = 0;


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
    localSymbols->parent = snap->head;
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
    if (localSymbols) {
        for (int i = 0; i < HASHTABLE_SIZE; i++) {
            Symbol *sym = localSymbols->buckets[i];
            while (sym) {
                if (!sym->is_alias) sym->type_def = NULL;
                sym = sym->next;
            }
        }
    }
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

// --- Scoped Procedure Table Management ---

/** Pushes a new procedure table onto the scope stack and returns it. */
HashTable *pushProcedureTable(void) {
    HashTable *new_table = createHashTable();
    new_table->parent = current_procedure_table;
    current_procedure_table = new_table;
    return new_table;
}

/** Restores the previous procedure table. If free_table is true, frees the popped table. */
void popProcedureTable(bool free_table) {
    if (!current_procedure_table) return;
    HashTable *old = current_procedure_table;
    current_procedure_table = current_procedure_table->parent;
    if (free_table) {
        freeHashTable(old);
    }
}

/** Looks up a procedure by name in the current procedure table scope chain. */
Symbol *lookupProcedure(const char *name) {
    if (!current_procedure_table && procedure_table) {
        current_procedure_table = procedure_table;
    }
    for (HashTable *tbl = current_procedure_table; tbl; tbl = tbl->parent) {
        Symbol *sym = hashTableLookup(tbl, name);
        if (sym) {
            return sym->is_alias ? sym->real_symbol : sym;
        }
    }
    return NULL;
}

Symbol *resolveSymbolAlias(Symbol *sym) {
    if (!sym) return NULL;
    if (sym->is_alias && sym->real_symbol) {
        return sym->real_symbol;
    }
    return sym;
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
// in src/symbol/symbol.c

static void updateSymbolInternal(Symbol *sym, const char *name, Value val) {
#ifdef DEBUG
    fprintf(stderr,
            "[DEBUG updateSymbol] Attempting to update symbol '%s' (Type: %s, Value @ %p, is_const: %d, is_alias: %d, is_local_var: %d). Incoming value type: %s\n",
            name ? name : (sym && sym->name ? sym->name : "<unnamed>"),
            sym ? varTypeToString(sym->type) : "<?>",
            (void*)(sym ? sym->value : NULL),
            sym ? sym->is_const : -1,
            sym ? sym->is_alias : -1,
            sym ? sym->is_local_var : -1,
            varTypeToString(val.type));
    fflush(stderr);
#endif
    if (!sym) {
        fprintf(stderr, "Runtime error: Attempted to assign to NULL symbol reference.\n");
        freeValue(&val);
        EXIT_FAILURE_HANDLER();
    }
    if (sym->is_const) {
        fprintf(stderr, "Runtime error: Cannot assign to constant '%s'.\n",
                name ? name : (sym->name ? sym->name : "<unnamed>"));
        freeValue(&val);
        EXIT_FAILURE_HANDLER();
    }
    if (!sym->value) {
        fprintf(stderr, "Runtime error: Symbol '%s' has NULL value pointer during assignment.\n",
                name ? name : (sym->name ? sym->name : "<unnamed>"));
        freeValue(&val);
        EXIT_FAILURE_HANDLER();
    }

    // --- Type Compatibility Check ---
    bool types_compatible = false;
    if (sym->type == val.type) {
        types_compatible = true; // Exact type match
    } else {
        // Handle specific allowed coercions and promotions.
        if (isRealType(sym->type) && (isRealType(val.type) || isIntlikeType(val.type))) types_compatible = true;
        else if (sym->type == TYPE_INTEGER && isRealType(val.type)) { types_compatible = false; } // No implicit Real to Integer
        else if (isIntlikeType(sym->type) && isIntlikeType(val.type)) types_compatible = true;
        else if (sym->type == TYPE_STRING && val.type == TYPE_CHAR) types_compatible = true;
        else if (sym->type == TYPE_CHAR && val.type == TYPE_STRING && val.s_val && strlen(val.s_val) == 1) types_compatible = true;
        else if (sym->type == TYPE_ENUM && val.type == TYPE_ENUM) {
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
        else if (sym->type == TYPE_ENUM && isIntlikeType(val.type)) types_compatible = true;
        else if (sym->type == TYPE_POINTER && (val.type == TYPE_POINTER || val.type == TYPE_NIL)) types_compatible = true;
        else if (sym->type == TYPE_SET && val.type == TYPE_SET) types_compatible = true;
        else if (sym->type == TYPE_MEMORYSTREAM && val.type == TYPE_MEMORYSTREAM) types_compatible = true;
        else if (sym->type == TYPE_FILE && val.type == TYPE_FILE) types_compatible = true;
    }

    if (!types_compatible) {
                fprintf(stderr, "Runtime error: Type mismatch. Cannot assign %s to %s for symbol '%s'.\n",
                        varTypeToString(val.type), varTypeToString(sym->type),
                        name ? name : (sym->name ? sym->name : "<unnamed>"));
                freeValue(&val);
                EXIT_FAILURE_HANDLER();
    }
    // --- End Type Compatibility Check ---


    bool isTextAttrSymbol = false;
    if (sym->name) {
        if (strcasecmp(sym->name, "crt.textattr") == 0) {
            isTextAttrSymbol = true;
        }
    }
    if (!isTextAttrSymbol && name) {
        if (strcasecmp(name, "crt.textattr") == 0) {
            isTextAttrSymbol = true;
        }
    }
    if (isTextAttrSymbol && !gTextAttrInitialized) {
        gTextAttrInitialized = true;
        if (isIntlikeType(val.type) && asI64(val) == 0) {
            freeValue(sym->value);
            SET_INT_VALUE(sym->value, 7);
            setCurrentTextAttrFromByte(7);
            freeValue(&val);
            return;
        }
    }

    // --- Free Old Value Contents (if necessary) ---
    // **CRITICAL EXCEPTION**: Do NOT free the buffer for a fixed-length string,
    // as we are about to write into it. For all other types, freeing the
    // old contents before assigning new ones is correct.
    if (!(sym->type == TYPE_STRING && sym->value->max_length > 0)) {
        freeValue(sym->value);
    }


    // --- Perform Assignment ---
    // Use a switch on the TARGET symbol's type to handle assignments correctly.
    switch (sym->type) {
        case TYPE_INTEGER:
            if (isIntlikeType(val.type)) {
                SET_INT_VALUE(sym->value, asI64(val));
            } else if (isRealType(val.type)) {
                SET_INT_VALUE(sym->value, (long long)AS_REAL(val)); // Implicit Truncation
            }
            break;

        case TYPE_INT64:
            if (isIntlikeType(val.type)) {
                SET_INT_VALUE(sym->value, asI64(val));
            } else if (isRealType(val.type)) {
                SET_INT_VALUE(sym->value, (long long)AS_REAL(val));
            }
            break;

        case TYPE_REAL:
            if (isRealType(val.type) || isIntlikeType(val.type)) {
                SET_REAL_VALUE(sym->value, asLd(val));
            }
            break;

        case TYPE_FLOAT:
            if (isRealType(val.type) || isIntlikeType(val.type)) {
                SET_REAL_VALUE(sym->value, asLd(val));
            }
            break;

        case TYPE_LONG_DOUBLE:
            if (isRealType(val.type) || isIntlikeType(val.type)) {
                SET_REAL_VALUE(sym->value, asLd(val));
            }
            break;

        case TYPE_BYTE: {
            if (isIntlikeType(val.type)) {
                long long tmp = asI64(val);
                if (tmp < 0 || tmp > 255) {
                    fprintf(stderr, "Runtime warning: Assignment to BYTE variable '%s' out of range (0-255). Value %lld will be truncated.\n", name, tmp);
                }
                SET_INT_VALUE(sym->value, (tmp & 0xFF));
            }
            break;
        }

        case TYPE_WORD: {
            if (isIntlikeType(val.type)) {
                long long tmp = asI64(val);
                if (tmp < 0 || tmp > 65535) {
                    fprintf(stderr, "Runtime warning: Assignment to WORD variable '%s' out of range (0-65535). Value %lld will be truncated.\n", name, tmp);
                }
                SET_INT_VALUE(sym->value, (tmp & 0xFFFF));
            }
            break;
        }

        case TYPE_STRING: {
            const char* source_str = NULL;
            char char_buf[2];

            if (val.type == TYPE_STRING) {
                source_str = val.s_val;
            } else if (val.type == TYPE_CHAR) {
                char_buf[0] = val.c_val;
                char_buf[1] = '\0';
                source_str = char_buf;
            }
            if (!source_str) source_str = "";

            if (sym->value->max_length > 0) { // Target is a fixed-length string.
                strncpy(sym->value->s_val, source_str, sym->value->max_length);
                sym->value->s_val[sym->value->max_length] = '\0';
            } else { // Target is a dynamic string.
                sym->value->s_val = strdup(source_str);
                if (!sym->value->s_val) {
                    fprintf(stderr, "FATAL: Memory allocation failed for dynamic string assignment to '%s'.\n", name);
                    EXIT_FAILURE_HANDLER();
                }
            }
            break;
        }

        case TYPE_RECORD:
        case TYPE_ARRAY:
        case TYPE_SET:
            *(sym->value) = makeCopyOfValue(&val);
            break;

        case TYPE_FILE:
            if (val.type == TYPE_FILE) {
                if (sym->value->f_val) fclose(sym->value->f_val);
                sym->value->f_val = val.f_val;
                if (sym->value->filename) free(sym->value->filename);
                sym->value->filename = val.filename ? strdup(val.filename) : NULL;
                val.f_val = NULL;
                val.filename = NULL;
            }
            break;

        case TYPE_BOOLEAN:
            if (isIntlikeType(val.type)) SET_INT_VALUE(sym->value, asI64(val) != 0 ? 1 : 0);
            break;

        case TYPE_CHAR:
            if (isIntlikeType(val.type)) {
                sym->value->c_val = (int)asI64(val);
            } else if (val.type == TYPE_STRING && val.s_val && strlen(val.s_val) == 1) {
                sym->value->c_val = val.s_val[0];
            }
            break;

        case TYPE_MEMORYSTREAM:
            *(sym->value) = makeCopyOfValue(&val); // Assuming makeCopyOfValue handles MStream correctly (shallow copy of pointer)
            break;

        case TYPE_ENUM:
            if (val.type == TYPE_ENUM) {
                if(sym->value->enum_val.enum_name) free(sym->value->enum_val.enum_name);
                sym->value->enum_val.enum_name = val.enum_val.enum_name ? strdup(val.enum_val.enum_name) : NULL;
                sym->value->enum_val.ordinal = val.enum_val.ordinal;
            } else if (isIntlikeType(val.type)) {
                AST* typeDef = sym->type_def;
                if (typeDef && typeDef->type == AST_TYPE_REFERENCE) typeDef = typeDef->right;
                long long maxOrdinal = -1;
                if (typeDef && typeDef->type == AST_ENUM_TYPE) { maxOrdinal = typeDef->child_count - 1; }

                long long v = asI64(val);
                if (maxOrdinal != -1 && (v < 0 || v > maxOrdinal)) {
                    fprintf(stderr, "Runtime warning: Assignment to ENUM variable '%s' out of range (0..%lld). Value %lld is invalid.\n", name, maxOrdinal, v);
                }
                sym->value->enum_val.ordinal = (int)v;
            }
            break;

        case TYPE_POINTER:
            sym->value->ptr_val = val.ptr_val;
            // The `base_type_node` of the variable itself does not change on assignment.
            break;

        case TYPE_VOID:
            fprintf(stderr, "Runtime error: Attempted assignment to VOID type symbol '%s'.\n", name);
            EXIT_FAILURE_HANDLER();
            break;

        default:
            fprintf(stderr, "Runtime error: Unhandled target type (%s) in updateSymbol assignment logic for '%s'.\n",
                    varTypeToString(sym->type), name);
            EXIT_FAILURE_HANDLER();
            break;
    }

    if (isTextAttrSymbol) {
        uint8_t attr_byte = (uint8_t)(sym->value->i_val & 0xFF);
        setCurrentTextAttrFromByte(attr_byte);
    }

    // Free the incoming temporary Value's contents now that its data has been used.
    freeValue(&val);

    #ifdef DEBUG
    fprintf(stderr, "[DEBUG updateSymbol] Assignment to '%s' successful. Final value type: %s\n",
            name ? name : (sym->name ? sym->name : "<unnamed>"),
            varTypeToString(sym->value->type));
    #endif
}

void updateSymbol(const char *name, Value val) {
    Symbol *sym = lookupSymbol(name);
    updateSymbolInternal(sym, name, val);
}

void updateSymbolDirect(Symbol *sym, const char *name, Value val) {
    updateSymbolInternal(sym, name ? name : (sym ? sym->name : NULL), val);
}
