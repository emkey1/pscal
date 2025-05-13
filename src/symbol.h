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
// Forward declare AST if ast.h includes symbol.h to break include cycle for full definitions
struct AST; // Forward declaration
#include "globals.h"   // For global variable declarations

// Keep standard library includes if their contents are used directly in this header's definitions or prototypes.
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h> // For uintptr_t

// Define the structure for a node in the symbol table with a named tag.
struct Symbol_s { // <--- NAMED TAG
    char *name;
    VarType type;
    Value *value;
    bool is_alias;
    bool is_local_var;
    bool is_const;
    struct AST *type_def;      // Use forward-declared struct AST
    struct Symbol_s *next; // Self-referential pointer using the tag
};
typedef struct Symbol_s Symbol; // Typedef for convenience

// Define the hash table structure with a named tag.
#define HASHTABLE_SIZE 256
struct SymbolTable_s { // <--- NAMED TAG
    Symbol *buckets[HASHTABLE_SIZE];
};
typedef struct SymbolTable_s HashTable; // Typedef for convenience

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
