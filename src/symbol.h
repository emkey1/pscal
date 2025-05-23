// src/symbol.h
#ifndef symbol_h
#define symbol_h

#include "types.h" // For VarType, Value (which includes FieldValue and TypeEntry)

// Forward declare AST as its full definition might not be needed here,
// and ast.h includes symbol.h creating a potential for cycles if not careful.
struct AST;

// Standard library includes used by this header or often by symbol.c
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h> // For uintptr_t

// --- Struct Definitions and Typedefs for Symbol and HashTable ---
// These are the primary definitions.
struct Symbol_s {
    char *name;
    VarType type;
    Value *value;
    bool is_alias;
    bool is_local_var;
    bool is_const;
    struct AST *type_def;      // Use forward-declared struct AST
    struct Symbol_s *next;     // Self-referential pointer using the tag
};
typedef struct Symbol_s Symbol;

#define HASHTABLE_SIZE 256
struct SymbolTable_s {
    Symbol *buckets[HASHTABLE_SIZE];
};
typedef struct SymbolTable_s HashTable;

// --- Include globals.h AFTER defining Symbol and HashTable ---
// This allows functions in symbol.c (prototyped below) to use macros
// or externs from globals.h if necessary.
#include "globals.h" // For SymbolEnvSnapshot typedef, EXIT_FAILURE_HANDLER etc.

// --- Include ast.h if full AST definition is needed by prototypes below ---
// Or if symbol.c needs it extensively.
// Ensure ast.h forward-declares Symbol if it needs Symbol* and symbol.h isn't included first by ast.h.
#include "ast.h"


// --- Public Symbol Table Interface Prototypes ---
Symbol *lookupSymbol(const char *name);
Symbol *lookupGlobalSymbol(const char *name);
Symbol *lookupLocalSymbol(const char *name);
void updateSymbol(const char *name, Value val);
Symbol *lookupSymbolIn(HashTable *table, const char *name);
void insertGlobalSymbol(const char *name, VarType type, struct AST *type_def_ast); // Use struct AST
Symbol *insertLocalSymbol(const char *name, VarType type, struct AST *type_def_ast, bool is_variable_declaration); // Use struct AST

// --- Local Environment Management Function Prototypes ---
void saveLocalEnv(SymbolEnvSnapshot *snap);
void restoreLocalEnv(SymbolEnvSnapshot *snap);
void popLocalEnv(void);

// --- Hash Table Internal Helper Prototypes ---
HashTable *createHashTable(void);
void freeHashTable(HashTable *table);
int hashFunctionName(const char *name);
Symbol *hashTableLookup(HashTable *table, const char *name);
void hashTableInsert(HashTable *table, Symbol *symbol);

// --- Other related prototypes ---
void nullifyPointerAliasesByAddrValue(HashTable* table, uintptr_t disposedAddrValue);

#endif // symbol_h
