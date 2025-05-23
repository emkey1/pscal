// src/globals.h
#ifndef GLOBALS_H
#define GLOBALS_H

#include <stdio.h>  // For fprintf, stderr
#include <stdlib.h> // For exit, EXIT_FAILURE

#include "types.h" // Provides TypeEntry, Value, List, AST forward decl etc.
#include "sdl.h"   // For SDL related externs or types if any directly in globals.h
                   // (It's better if specific SDL globals are in sdl.h and sdl.c)

// --- EXIT_FAILURE_HANDLER Macro ---
#ifdef SUPPRESS_EXIT
    #define EXIT_FAILURE_HANDLER() fprintf(stderr, "Suppressed exit call from %s:%d\n", __FILE__, __LINE__)
#else
    #define EXIT_FAILURE_HANDLER() exit(EXIT_FAILURE)
#endif

#define MAX_SYMBOL_LENGTH 255
#define MAX_ID_LENGTH 256

// --- Forward Declarations and Typedefs needed by this file ---
// These types are defined in symbol.h
struct Symbol_s;
typedef struct Symbol_s Symbol;

struct SymbolTable_s;
typedef struct SymbolTable_s HashTable;

// TypeEntry is now fully defined in "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Global Variable EXTERN Declarations ---
extern HashTable *globalSymbols;
extern HashTable *localSymbols;
extern Symbol *current_function_symbol;

extern HashTable *procedure_table; // Procedure table is now a HashTable
extern TypeEntry *type_table;      // TypeEntry definition comes from types.h

// --- CRT State Variables ---
extern int gCurrentTextColor;
extern int gCurrentTextBackground;
extern bool gCurrentTextBold;
extern bool gCurrentColorIsExt;
extern bool gCurrentBgIsExt;

// --- Other Globals ---
#define MAX_RECURSION_DEPTH 10
extern int gParamCount;
extern char **gParamValues;
// extern AST *globalRoot; // Defined in main.c typically

extern int last_io_error;
extern int typeWarn;

#ifdef DEBUG
extern int dumpExec;
// Assuming List is defined in types.h or list.h (which types.h might include)
extern List *inserted_global_names;
#endif

extern int break_requested;

#define DEFAULT_STRING_CAPACITY 255

// Snapshot structure for saving/restoring local symbol environments
typedef struct SymbolEnvSnapshot {
    HashTable *head; // Uses HashTable typedef
} SymbolEnvSnapshot;

#ifdef __cplusplus
}
#endif

#endif /* GLOBALS_H */
