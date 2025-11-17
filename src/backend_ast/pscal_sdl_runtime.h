#ifndef PSCAL_SDL_RUNTIME_H
#define PSCAL_SDL_RUNTIME_H

#ifdef SDL
#include "core/sdl_headers.h"
#include PSCALI_SDL_HEADER
#include PSCALI_SDL_TTF_HEADER
#include PSCALI_SDL_IMAGE_HEADER
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
extern SDL_GLContext gSdlGLContext;
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

void sdlEnsureInputWatch(void);
bool sdlCachedKeyDown(SDL_Scancode sc);
bool sdlIsGraphicsActive(void);
bool sdlHasPendingKeycode(void);
SDL_Keycode sdlWaitNextKeycode(void);
bool sdlPollNextKey(SDL_Keycode* outCode);
bool sdlTextInputActive(void);
void sdlStartTextInput(void);
void sdlStopTextInput(void);
void sdlFlushSpuriousQuitEvents(void);


// System Functions
void initializeSdlSystems(void);
void initializeTextureSystem(void);
void cleanupSdlWindowResources(void);
void sdlCleanupAtExit(void);

// AST-based built-in handlers

// VM-native built-in handlers
Value vmBuiltinInitgraph(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinInitgraph3d(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinClosegraph(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinClosegraph3d(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinFillrect(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinUpdatescreen(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinCleardevice(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGetmaxx(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGetmaxy(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGetticks(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGetscreensize(struct VM_s* vm, int arg_count, Value* args);
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
Value vmBuiltinGlsetswapinterval(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlswapwindow(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinPutpixel(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinCreatetargettexture(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinCreatetexture(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinLoadimagetotexture(struct VM_s* vm, int arg_count, Value* args);
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
Value vmBuiltinPollkey(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinIskeydown(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinWaitkeyevent(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinUpdatetexture(struct VM_s* vm, int arg_count, Value* args);

#ifdef __cplusplus
}
#endif

#endif
#endif // PSCAL_SDL_RUNTIME_H
