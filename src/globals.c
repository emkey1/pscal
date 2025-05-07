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

// SDL Stuff Start
SDL_Window* gSdlWindow = NULL;
SDL_Renderer* gSdlRenderer = NULL;
SDL_Color gSdlCurrentColor = { 255, 255, 255, 255 }; // Default white
bool gSdlInitialized = false;
int gSdlWidth = 0;
int gSdlHeight = 0;
TTF_Font* gSdlFont = NULL;
int gSdlFontSize   = 16;   
// SDL Stuff End

int break_requested = 0;


#ifdef DEBUG
int dumpExec = 1;  // Set to 1 by default in debug mode
#endif
