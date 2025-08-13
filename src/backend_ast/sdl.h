#ifndef PSCAL_SDL_H
#define PSCAL_SDL_H

#ifdef SDL
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
Value executeBuiltinPollKey(AST *node);
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
Value vmBuiltinInitgraph(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinClosegraph(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinFillrect(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinUpdatescreen(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinCleardevice(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGetmaxx(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGetmaxy(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGetticks(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSetrgbcolor(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinInittextsystem(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinQuittextsystem(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGettextsize(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGetmousestate(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinDestroytexture(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinRendercopyrect(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSetalphablend(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinRendertexttotexture(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinFillcircle(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGraphloop(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinPutpixel(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinCreatetargettexture(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinCreatetexture(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinDrawcircle(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinDrawline(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinDrawpolygon(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinDrawrect(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGetpixelcolor(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinOuttextxy(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinRendercopy(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinRendercopyex(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSetcolor(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSetrendertarget(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinUpdatetexture(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinPollkey(struct VM_s* vm, int arg_count, Value* args);

#ifdef __cplusplus
}
#endif

#endif
#endif // PSCAL_SDL_H
