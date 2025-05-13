// globals.c
// This file defines and initializes the global variables used throughout the Pscal interpreter.

// Include the globals header file, which declares the global variables.
#include "globals.h" // This now includes symbol.h and defines HashTable


// --- Global Variable Definitions and Initialization ---
// These variables are declared as 'extern' in globals.h.

// I/O and type conversion globals
int last_io_error = 0; // Stores the error code of the last I/O operation.
int typeWarn = 1; // Flag to control type warning messages (e.g., 1 for enabled, 0 for disabled).

// Symbol table globals - NOW POINTERS TO HASHTABLES.
// These will be initialized by calling createHashTable() in initSymbolSystem().
HashTable *globalSymbols = NULL; // Global symbol table (initialized to NULL, will point to a HashTable).
HashTable *localSymbols = NULL;  // Current local symbol table (initialized to NULL, will point to a HashTable).

// Pointer to the Symbol representing the currently executing function (for 'result' variable).
// <<< REMOVE THE DUPLICATE DEFINITION OF current_function_symbol >>>
// Symbol *current_function_symbol = NULL; // This line should be removed if it's defined again below.

// Procedure table for storing information about declared procedures and functions.
// This remains a linked list of Procedure structs.
HashTable *procedure_table = NULL; // Initialized to NULL.

// User-defined type table for storing information about declared types (records, enums, etc.).
// This remains a linked list of TypeEntry structs.
TypeEntry *type_table = NULL; // Initialized to NULL.


// --- CRT State Variable Definitions & Defaults ---
// These variables hold the current state for console/text rendering (colors, bold).
int gCurrentTextColor       = 7;       // Default foreground color (LightGray in 16-color palette).
int gCurrentTextBackground = 0;       // Default background color (Black in 16-color palette).
bool gCurrentTextBold      = false;   // Default text boldness state.
bool gCurrentColorIsExt    = false;   // Flag for extended 256-color foreground mode.
bool gCurrentBgIsExt       = false;   // Flag for extended 256-color background mode.
// --- End CRT State Variables ---

// Flag used by builtins like GraphLoop to signal a quit request from the user.
int break_requested = 0;


#ifdef DEBUG
// In DEBUG mode, this flag controls whether execution debugging output is enabled.
int dumpExec = 1;  // Set to 1 by default in debug mode.
#endif

// --- ADDED: Place the single definition here if it wasn't at the top ---
// Based on the error message, the definition might be present here as well.
// Ensure this is the *only* definition in the file.
// <<< ENSURE ONLY ONE DEFINITION OF current_function_symbol EXISTS IN THIS FILE >>>
// If there was a definition at line 8 (as per your error message), remove that one
// and keep this one, or vice-versa. Make sure only one remains.
Symbol *current_function_symbol = NULL; // Define the global variable here.

// Note: Other global SDL/Audio variables declared in globals.h are typically
// defined and initialized in their respective .c files (sdl.c, audio.c).
