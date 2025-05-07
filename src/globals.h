/* globals.h */
#ifndef GLOBALS_H
#define GLOBALS_H

// The place where stuff that needs to be shared between files is placed
#include "types.h"
// SDL stuff
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#define MAX_SDL_TEXTURES 32 // Define a maximum number of textures

#ifdef __cplusplus
extern "C" {
#endif

// ... (other extern variables like gCurrentTextColor etc.) ...

// --- SDL Graphics Globals ---
extern SDL_Window* gSdlWindow;
extern SDL_Renderer* gSdlRenderer;
extern SDL_Color gSdlCurrentColor; // Store RGBA for current drawing color
extern bool gSdlInitialized;
extern int gSdlWidth;
extern int gSdlHeight;
extern TTF_Font* gSdlFont;
extern int gSdlFontSize;
extern SDL_Texture* gSdlTextures[MAX_SDL_TEXTURES]; // Array to hold texture pointers
extern int gSdlTextureWidths[MAX_SDL_TEXTURES];   // Store widths
extern int gSdlTextureHeights[MAX_SDL_TEXTURES];
extern bool gSdlTtfInitialized;
// --- END SDL ---

#ifdef __cplusplus
}
#endif

#ifdef SUPPRESS_EXIT
    #define EXIT_FAILURE_HANDLER() fprintf(stderr, "Suppressed\n")
#else
    #define EXIT_FAILURE_HANDLER() exit(EXIT_FAILURE)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// --- CRT State Variables (Declarations) ---
extern int gCurrentTextColor;
extern int gCurrentTextBackground;
extern bool gCurrentTextBold;
extern bool gCurrentColorIsExt;
extern bool gCurrentBgIsExt;
// --- End CRT State Variables ---

// For unit parsing...
#define MAX_RECURSION_DEPTH 10

// Lets implement command line parsing...
extern int gParamCount; // Number of options passed - 1
extern char **gParamValues;
extern AST *globalRoot;

/* I/O and type conversion globals */
extern int last_io_error;
extern int typeWarn;
#ifdef DEBUG
extern int dumpExec;
#endif

/* Symbol table globals */
typedef struct Symbol Symbol;
extern Symbol *globalSymbols;
extern Symbol *localSymbols;
extern Symbol *current_function_symbol;

/* User-defined type table */
typedef struct TypeEntry TypeEntry;
extern TypeEntry *type_table;

/* Procedure table for procedures and functions */
typedef struct Procedure Procedure;
extern Procedure *procedure_table;

#ifdef __cplusplus
}
#endif

// Define a new structure to hold user-defined type mappings.
typedef struct TypeEntry {
    char *name;    // The type name, already lowercased by the lexer.
    AST *typeAST;  // The AST node that defines the type (e.g. a record type).
    struct TypeEntry *next;
} TypeEntry;

extern int break_requested;

#define DEFAULT_STRING_CAPACITY 255


#endif /* GLOBALS_H */

