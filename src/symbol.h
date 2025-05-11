//
//  symbol.h
//  pscal
//
//  Created by Michael Miller on 3/25/25.
//

#ifndef symbol_h
#define symbol_h

// Include necessary headers that define types used in Symbol and HashTable structs.
// Need full definitions for VarType, Value, and AST.
#include "types.h" // For VarType, Value (which includes FieldValue)
#include "ast.h"   // For AST
#include "globals.h"   // For AST

// Keep standard library includes if their contents are used directly in this header's definitions or prototypes.
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h> // For uintptr_t


// Define the structure for a node in the symbol table.
// This struct represents a symbol entry (variable, constant, procedure, function, type)
// and acts as a node in the linked lists used within hash table buckets (separate chaining).
typedef struct Symbol {
    char *name;         // The name of the symbol (e.g., variable name, procedure name)
    VarType type;       // The type of the symbol
    Value *value;       // Pointer to the Value structure holding the symbol's data.
    bool is_alias;      // True if 'value' pointer points to another symbol's value.
    bool is_local_var;  // True if this symbol is a variable declared in a local scope.
    bool is_const;      // True if this symbol is a constant.
    AST *type_def;      // Pointer to the AST node that defines the symbol's type.
    struct Symbol *next; // Pointer to the next Symbol in the linked list within a hash table bucket.
} Symbol;


// Define the hash table structure for symbol tables (global and local scopes).
#define HASHTABLE_SIZE 256 // The fixed size of the hash table array (number of buckets).

// Use the struct tag in the typedef to define the structure type and its alias.
typedef struct SymbolTable {
    Symbol *buckets[HASHTABLE_SIZE]; // Array of pointers to Symbol (head of linked list for each bucket)
    // Optional: Add fields here for tracking number of symbols, number of occupied buckets, etc.
} HashTable;


// --- Public Symbol Table Interface Prototypes ---
// These functions provide the interface for looking up and inserting symbols.
// Their implementations in symbol.c will now internally use the hash table operations.
Symbol *lookupSymbol(const char *name);
Symbol *lookupGlobalSymbol(const char *name);
Symbol *lookupLocalSymbol(const char *name);
void updateSymbol(const char *name, Value val);
Symbol *lookupSymbolIn(HashTable *table, const char *name); // Helper takes HashTable*
void insertGlobalSymbol(const char *name, VarType type, AST *type_def);
Symbol *insertLocalSymbol(const char *name, VarType type, AST* type_def, bool is_variable_declaration);

// --- Local Environment Management Function Prototypes --- // <<< ADD THIS SECTION
// These functions are responsible for saving and restoring the local symbol table.
// They are defined in symbol.c and called from interpreter.c.
// Assumes SymbolEnvSnapshot is defined in globals.h and contains HashTable* head.
void saveLocalEnv(SymbolEnvSnapshot *snap); // <<< ADD PROTOTYPE
void restoreLocalEnv(SymbolEnvSnapshot *snap); // <<< ADD PROTOTYPE
void popLocalEnv(void); // <<< ADD PROTOTYPE


// --- Hash Table Internal Helper Prototypes ---
// These functions are the core hash table operations used internally by the symbol table interface.
HashTable *createHashTable(void);
void freeHashTable(HashTable *table);
int hashFunctionName(const char *name);
Symbol *hashTableLookup(HashTable *table, const char *name);
void hashTableInsert(HashTable *table, Symbol *symbol);


// --- Other related prototypes ---
// Keep prototypes that were in your original symbol.h and are still relevant.

// Function used by dispose to nullify pointer aliases in a symbol table.
// Modified to take HashTable* instead of Symbol* (head of a list).
void nullifyPointerAliasesByAddrValue(HashTable* table, uintptr_t disposedAddrValue);


// Keep other prototypes that were in your original symbol.h if they are still used elsewhere.
// For example, if other files need to know about these:
// bool hasSeenGlobal(const char *name); // Related to unit parsing?
// void markGlobalAsSeen(const char *name); // Related to unit parsing?
// Procedure *getProcedureTable(void); // Getting procedure table (remains a linked list of Procedure structs, defined in parser.h)
// void assignToRecord(FieldValue *record, const char *fieldName, Value val); // Record field assignment helper


#endif // symbol_h
