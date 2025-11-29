// src/symbol.h
#ifndef symbol_h
#define symbol_h

#include "core/types.h" // For VarType, Value (which includes FieldValue and TypeEntry)

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
    bool is_inline;
    bool closure_captures;
    bool closure_escapes;
    struct AST *type_def;      // Use forward-declared struct AST
    struct Symbol_s *next;     // Self-referential pointer using the tag
    // --- New fields for compiled procedures/functions ---
    bool is_defined;              // Flag to indicate if the body has been compiled (useful for forward declarations)
    int bytecode_address;         // Starting address (offset) in the bytecode chunk
    uint8_t arity;                // Number of parameters
    uint16_t locals_count;        // Number of local variables (excluding parameters)
    int slot_index;             // Index into the procedure's locals frame (-1 if not a local) // <--- THIS LINE IS UNCHANGED BUT CONFIRMED
    struct Symbol_s* real_symbol; // If this is an alias, this points to the real symbol
    struct Symbol_s* enclosing;   // Enclosing procedure/function, if any
    uint8_t upvalue_count;
    struct {
        uint8_t index;
        bool isLocal;
        bool is_ref;          // Indicates whether the captured variable is a reference (VAR param)
    } upvalues[256];
};

typedef struct Symbol_s Symbol;

#define HASHTABLE_SIZE 256
struct SymbolTable_s {
    Symbol *buckets[HASHTABLE_SIZE];
    struct SymbolTable_s *parent; /* For scoped procedure tables */
};
typedef struct SymbolTable_s HashTable;

// --- Include globals.h AFTER defining Symbol and HashTable ---
// This allows functions in symbol.c (prototyped below) to use macros
// or externs from globals.h if necessary.
#include "Pascal/globals.h" // For SymbolEnvSnapshot typedef, EXIT_FAILURE_HANDLER etc.

// --- Include ast.h if full AST definition is needed by prototypes below ---
// Or if symbol.c needs it extensively.
// Ensure ast.h forward-declares Symbol if it needs Symbol* and symbol.h isn't included first by ast.h.
#include "ast/ast.h"


// --- Public Symbol Table Interface Prototypes ---
Symbol *lookupSymbol(const char *name);
Symbol *lookupSymbolOptional(const char *name);
Symbol *lookupGlobalSymbol(const char *name);
Symbol *lookupLocalSymbol(const char *name);
void updateSymbol(const char *name, Value val);
void updateSymbolDirect(Symbol *sym, const char *name, Value val);
Symbol *lookupSymbolIn(HashTable *table, const char *name);
void insertGlobalSymbol(const char *name, VarType type, struct AST *type_def_ast); // Use struct AST
void insertGlobalAlias(const char *name, struct Symbol_s *target);
void insertConstGlobalSymbol(const char *name, Value val);
void insertConstSymbolIn(HashTable *table, const char *name, Value val);
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

// --- Scoped procedure table helpers ---
HashTable *pushProcedureTable(void);
void popProcedureTable(bool free_table);
Symbol *lookupProcedure(const char *name);
Symbol *resolveSymbolAlias(Symbol *sym);

#endif // symbol_h
