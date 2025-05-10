//
//  sdl.h
//  Pscal
//
//  Created by Michael Miller on 5/7/25. // Date seems to be in the future, just a note :)
//

// Include guards are essential for header files
#ifndef PSCAL_SDL_H  // <<<< ONLY the macro name here
#define PSCAL_SDL_H

// SDL stuff
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

// Need Value and AST types for function prototypes
#include "types.h" // Assuming Value and AST are defined here or in headers it includes

#define MAX_SDL_TEXTURES 32 // Define a maximum number of textures

// Wrap extern "C" for C++ compatibility, though likely not an issue if pure C
#ifdef __cplusplus
extern "C" {
#endif

// --- SDL Graphics Globals ---
extern SDL_Window* gSdlWindow;
extern SDL_Renderer* gSdlRenderer;
extern SDL_Color gSdlCurrentColor; // Store RGBA for current drawing color
extern bool gSdlInitialized;       // Tracks if core SDL_Init(VIDEO) was called
extern int gSdlWidth;
extern int gSdlHeight;
extern TTF_Font* gSdlFont;
extern int gSdlFontSize;
extern bool gSdlTtfInitialized;    // Tracks if TTF_Init() was called

extern SDL_Texture* gSdlTextures[MAX_SDL_TEXTURES]; // Array to hold texture pointers
extern int gSdlTextureWidths[MAX_SDL_TEXTURES];   // Store widths
extern int gSdlTextureHeights[MAX_SDL_TEXTURES];  // Store heights
// --- END SDL Globals ---

// --- SDL Misc ---
Value executeBuiltinQuitRequested(AST *node);

// --- C-level SDL System Functions ---
void InitializeSdlSystems(void); // Optional: if you want a C-level init for SDL/TTF called by main
void InitializeTextureSystem(void); // For zeroing out gSdlTextures
void SdlCleanupAtExit(void);        // For cleanup called from main

// --- Built-in Handler Function Prototypes ---
// These are the functions that will be pointed to by the dispatch table
// and are implemented in sdl.c

// Core SDL System
Value executeBuiltinInitGraph(AST *node);
Value executeBuiltinCloseGraph(AST *node);
Value executeBuiltinGraphLoop(AST *node);
Value executeBuiltinUpdateScreen(AST *node);
Value executeBuiltinWaitKeyEvent(AST *node); // Consider if this is purely SDL or also console
Value executeBuiltinClearDevice(AST *node);

// Drawing Primitives & State
Value executeBuiltinGetMaxX(AST *node);
Value executeBuiltinGetMaxY(AST *node);
Value executeBuiltinSetColor(AST *node); // For indexed color (if you keep it)
Value executeBuiltinSetRGBColor(AST *node);
Value executeBuiltinPutPixel(AST *node);
Value executeBuiltinDrawLine(AST *node);
Value executeBuiltinDrawRect(AST *node); // Outline
Value executeBuiltinFillRect(AST *node); // Filled
Value executeBuiltinDrawCircle(AST *node); // Outline
Value executeBuiltinFillCircle(AST *node); // Filled

// SDL_ttf Text System
Value executeBuiltinInitTextSystem(AST *node);
Value executeBuiltinOutTextXY(AST *node);
Value executeBuiltinQuitTextSystem(AST *node);

// Mouse Input
Value executeBuiltinGetMouseState(AST *node);

// Texture Management
Value executeBuiltinCreateTexture(AST *node);
Value executeBuiltinDestroyTexture(AST *node);
Value executeBuiltinUpdateTexture(AST *node);
Value executeBuiltinRenderCopy(AST *node);
Value executeBuiltinRenderCopyRect(AST *node);

#ifdef __cplusplus
}
#endif

#endif  // PSCAL_SDL_H
