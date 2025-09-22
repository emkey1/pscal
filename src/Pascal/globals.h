// src/globals.h
#ifndef GLOBALS_H
#define GLOBALS_H

#include <stdio.h>  // For fprintf, stderr
#include <stdlib.h> // For exit, EXIT_FAILURE
#include <pthread.h> // For mutex guarding of global tables
#include <stdatomic.h> // For atomic flags shared across threads

#include "types.h" // Provides TypeEntry, Value, List, AST forward decl etc.
#ifdef SDL
#include "backend_ast/sdl.h"   // For SDL related externs or types if any directly in globals.h
                   // (It's better if specific SDL globals are in sdl.h and sdl.c)
#endif

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
extern HashTable *constGlobalSymbols;
extern HashTable *localSymbols;
extern Symbol *current_function_symbol;

extern HashTable *procedure_table; // Procedure table is now a HashTable
extern HashTable *current_procedure_table; // Pointer to current procedure scope
extern TypeEntry *type_table;      // TypeEntry definition comes from types.h

// --- CRT State Variables ---
extern int gCurrentTextColor;
extern int gCurrentTextBackground;
extern bool gCurrentTextBold;
extern bool gCurrentColorIsExt;
extern bool gCurrentBgIsExt;
extern bool gCurrentTextUnderline;
extern bool gCurrentTextBlink;
extern bool gConsoleAttrDirty;
extern bool gConsoleAttrDirtyFromReset;
extern int gWindowLeft;
extern int gWindowTop;
extern int gWindowRight;
extern int gWindowBottom;

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

extern atomic_int break_requested;
extern int exit_requested; // Flag set by builtin 'exit' to unwind the current routine
extern int pascal_semantic_error_count; // Count of semantic/type errors during analysis
extern int pascal_parser_error_count;   // Count of parser (syntax) errors

// Mutex protecting shared global tables
extern pthread_mutex_t globals_mutex;

#define DEFAULT_STRING_CAPACITY 255

// Snapshot structure for saving/restoring local symbol environments
typedef struct SymbolEnvSnapshot {
    HashTable *head; // Uses HashTable typedef
} SymbolEnvSnapshot;

#ifdef __cplusplus
}
#endif

#endif /* GLOBALS_H */
