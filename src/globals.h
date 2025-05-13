#ifndef GLOBALS_H
#define GLOBALS_H

#include "types.h" // Includes symbol.h for HashTable definition
#include "sdl.h"   // Includes SDL headers
#include "symbol.h"   // Includes SDL headers
#include <stdio.h>  // For fprintf, stderr (used in EXIT_FAILURE_HANDLER)
#include <stdlib.h> // For exit, EXIT_FAILURE (used in EXIT_FAILURE_HANDLER)

// Forward declare Procedure struct if its definition is in parser.h and parser.h is not included here
// However, based on your file structure, types.h -> symbol.h defines HashTable.
// And parser.h (which defines Procedure) includes types.h.
// So, if globals.h includes parser.h (or types.h which includes symbol.h), HashTable should be known.

// Let's assume parser.h is included before this line, or HashTable is known via symbol.h
// typedef struct SymbolTable HashTable; // This is likely in symbol.h

#ifdef __cplusplus
extern "C" {
#endif

// --- Symbol Table Globals ---
// Forward declare the struct tags explicitly, then typedef.
struct SymbolTable_s; // <--- Use the named tag from symbol.h
typedef struct SymbolTable_s HashTable;

struct Symbol_s;      // <--- Use the named tag from symbol.h
typedef struct Symbol_s Symbol;

// --- Procedure Table (now a Hash Table) ---
extern HashTable *procedure_table; 

extern TypeEntry *type_table;

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
// extern AST *globalRoot; // Usually defined in main.c or parser.c if truly global AST root

extern int last_io_error;
extern int typeWarn;
#ifdef DEBUG
extern int dumpExec;
extern List *inserted_global_names; // If used for debug symbol tracking
#endif

extern int break_requested;

#define DEFAULT_STRING_CAPACITY 255

// Snapshot structure for saving/restoring local symbol environments
typedef struct SymbolEnvSnapshot {
    HashTable *head;
} SymbolEnvSnapshot;

#ifdef __cplusplus
}
#endif
#endif /* GLOBALS_H */
