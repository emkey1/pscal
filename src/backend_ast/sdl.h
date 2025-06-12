// src/backend_ast/sdl.h

// Include guards are essential for header files
#ifndef PSCAL_SDL_H
#define PSCAL_SDL_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include "core/types.h"
#include <stdbool.h>

#define MAX_SDL_TEXTURES 32

#ifdef __cplusplus
extern "C" {
#endif

// Forward declare the VM struct
struct VM_s;

// SDL Globals
extern SDL_Window* gSdlWindow;
extern SDL_Renderer* gSdlRenderer;
extern SDL_Color gSdlCurrentColor;
extern bool gSdlInitialized;
extern int gSdlWidth;
extern int gSdlHeight;
extern TTF_Font* gSdlFont;
extern int gSdlFontSize;
extern SDL_Texture* gSdlTextures[MAX_SDL_TEXTURES];
extern int gSdlTextureWidths[MAX_SDL_TEXTURES];
extern int gSdlTextureHeights[MAX_SDL_TEXTURES];
extern bool gSdlTtfInitialized;
extern bool gSdlImageInitialized;


// System Functions
void InitializeSdlSystems(void);
void InitializeTextureSystem(void);
void SdlCleanupAtExit(void);

// AST-based built-in handlers
Value executeBuiltinInitGraph(AST *node);
Value executeBuiltinCloseGraph(AST *node);
Value executeBuiltinGraphLoop(AST *node);
Value executeBuiltinUpdateScreen(AST *node);
Value executeBuiltinClearDevice(AST *node);
Value executeBuiltinWaitKeyEvent(AST *node);
Value executeBuiltinGetMaxX(AST *node);
Value executeBuiltinGetMaxY(AST *node);
Value executeBuiltinGetTicks(AST *node);
Value executeBuiltinGetMouseState(AST *node);
Value executeBuiltinQuitRequested(AST *node);
Value executeBuiltinSetColor(AST *node);
Value executeBuiltinSetRGBColor(AST *node);
Value executeBuiltinPutPixel(AST *node);
Value executeBuiltinDrawLine(AST *node);
Value executeBuiltinDrawRect(AST *node);
Value executeBuiltinFillRect(AST *node);
Value executeBuiltinDrawCircle(AST *node);
Value executeBuiltinFillCircle(AST *node);
Value executeBuiltinDrawPolygon(AST *node);
Value executeBuiltinGetPixelColor(AST *node);
Value executeBuiltinInitTextSystem(AST *node);
Value executeBuiltinQuitTextSystem(AST *node);
Value executeBuiltinOutTextXY(AST *node);
Value executeBuiltinGetTextSize(AST *node);
Value executeBuiltinCreateTexture(AST *node);
Value executeBuiltinCreateTargetTexture(AST *node);
Value executeBuiltinDestroyTexture(AST *node);
Value executeBuiltinUpdateTexture(AST *node);
Value executeBuiltinSetRenderTarget(AST *node);
Value executeBuiltinRenderCopy(AST *node);
Value executeBuiltinRenderCopyRect(AST *node);
Value executeBuiltinRenderCopyEx(AST *node);
Value executeBuiltinLoadImageToTexture(AST *node);
Value executeBuiltinRenderTextToTexture(AST *node);
Value executeBuiltinSetAlphaBlend(AST *node);

// VM-native built-in handlers
Value vm_builtin_initgraph(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_closegraph(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_fillrect(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_updatescreen(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_cleardevice(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_getmaxx(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_getmaxy(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_getticks(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_setrgbcolor(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_inittextsystem(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_quittextsystem(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_gettextsize(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_getmousestate(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_destroytexture(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_rendercopyrect(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_setalphablend(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_rendertexttotexture(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_fillcircle(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_graphloop(struct VM_s* vm, int arg_count, Value* args);

#ifdef __cplusplus
}
#endif

#endif // PSCAL_SDL_H
