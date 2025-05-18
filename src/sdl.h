//
//  sdl.h
//  Pscal
//
//  Created by Michael Miller on 5/7/25.
//

// Include guards are essential for header files
#ifndef PSCAL_SDL_H
#define PSCAL_SDL_H

// --- INCLUDE SDL HEADERS HERE ---
// These headers define the types used in the global variable declarations below
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h> // For IMG_LoadTexture, needs to be included where SDL.h is
// --- END INCLUDE ---

// Include types.h which should NOT include SDL headers (confirmed by your content)
#include "types.h"

#define MAX_SDL_TEXTURES 32 // Define a maximum number of textures

// Wrap extern "C" for C++ compatibility (standard practice)
#ifdef __cplusplus
extern "C" {
#endif

// --- SDL Graphics Globals (Declarations) ---
// These global variables are defined in symbol.c
// Use the standard type names (SDL_Window*, SDL_Renderer*, etc.)
// since the full type definitions are available from the included headers.
extern SDL_Window* gSdlWindow;
extern SDL_Renderer* gSdlRenderer;
extern SDL_Color gSdlCurrentColor; // Use the standard type name
extern bool gSdlInitialized;       // Tracks if core SDL_Init(VIDEO) was called
extern bool gSdlImageInitialized; // To track IMG_Init
extern int gSdlWidth;
extern int gSdlHeight;
extern TTF_Font* gSdlFont; // Use TTF_Font* (standard type name)
extern int gSdlFontSize;
extern bool gSdlTtfInitialized;    // Tracks if TTF_Init() was called

extern SDL_Texture* gSdlTextures[MAX_SDL_TEXTURES]; // Use SDL_Texture* (standard type name)
extern int gSdlTextureWidths[MAX_SDL_TEXTURES];   // Store widths
extern int gSdlTextureHeights[MAX_SDL_TEXTURES];  // Store heights
Value executeBuiltinClearDevice(AST *node);
// --- END SDL ---

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
Value executeBuiltinGetPixelColor(AST *node);
Value executeBuiltinDrawLine(AST *node);
Value executeBuiltinDrawRect(AST *node); // Outline
Value executeBuiltinFillRect(AST *node); // Filled
Value executeBuiltinDrawCircle(AST *node); // Outline
Value executeBuiltinFillCircle(AST *node); // Filled
Value executeBuiltinRenderCopyEx(AST *node); 
Value executeBuiltinDrawPolygon(AST *node); // For outline
Value executeBuiltinSetAlphaBlend(AST *node); 

// SDL_ttf Text System
Value executeBuiltinInitTextSystem(AST *node);
Value executeBuiltinOutTextXY(AST *node);
Value executeBuiltinQuitTextSystem(AST *node);
Value executeBuiltinGetTextSize(AST *node);

// Mouse Input
Value executeBuiltinGetMouseState(AST *node);

// Texture Management
Value executeBuiltinCreateTexture(AST *node);
Value executeBuiltinDestroyTexture(AST *node);
Value executeBuiltinUpdateTexture(AST *node);
Value executeBuiltinRenderCopy(AST *node);
Value executeBuiltinRenderCopyRect(AST *node);
Value executeBuiltinSetRenderTarget(AST *node);
Value executeBuiltinCreateTargetTexture(AST *node);

// Misc Built-ins
Value executeBuiltinQuitRequested(AST *node);
void SdlCleanupAtExit(void);
Value executeBuiltinGetTicks(AST *node); 

// Image Built-ins
Value executeBuiltinLoadImageToTexture(AST *node);


#ifdef __cplusplus
}
#endif

#endif // PSCAL_SDL_H
