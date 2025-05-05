// globals.c
#include "globals.h"

int last_io_error = 0;
int typeWarn = 1;

Symbol *globalSymbols = NULL;
Symbol *localSymbols = NULL;
Symbol *current_function_symbol = NULL;
Procedure *procedure_table = NULL;
TypeEntry *type_table = NULL;

// --- CRT State Variable Definitions & Defaults ---
int gCurrentTextColor       = 7;       // Default LightGray
int gCurrentTextBackground = 0;       // Default Black
bool gCurrentTextBold      = false;   // Default off
bool gCurrentColorIsExt    = false;   // Default standard 16-color mode
bool gCurrentBgIsExt       = false;   // Default standard 16-color mode (using 0-7 range)
// --- End CRT State Variables ---

// SDL Stuff
SDL_Window* gSdlWindow = NULL;
SDL_Renderer* gSdlRenderer = NULL;
// Initialize default drawing color (e.g., white)
SDL_Color gSdlCurrentColor = { 255, 255, 255, 255 }; // R, G, B, Alpha
bool gSdlInitialized = false; // Don't start graphics if not needed

int break_requested = 0;


#ifdef DEBUG
int dumpExec = 1;  // Set to 1 by default in debug mode
#endif
