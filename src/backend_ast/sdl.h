//
//  sdl.h
//  Pscal
//
//  Created by Michael Miller on 5/7/25.
//

// Include guards are essential for header files
#ifndef PSCAL_SDL_H
#define PSCAL_SDL_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declare the VM struct to break the circular dependency.
struct VM_s;

// SDL Globals
extern SDL_Window* gSdlWindow;
extern SDL_Renderer* gSdlRenderer;
extern SDL_Color gSdlCurrentColor;
extern bool gSdlInitialized;
extern bool gSdlImageInitialized;
extern int gSdlWidth;
extern int gSdlHeight;
extern TTF_Font* gSdlFont;
extern int gSdlFontSize;
extern bool gSdlTtfInitialized;
extern SDL_Texture* gSdlTextures[MAX_SDL_TEXTURES];
extern int gSdlTextureWidths[MAX_SDL_TEXTURES];
extern int gSdlTextureHeights[MAX_SDL_TEXTURES];

// System Functions
void InitializeSdlSystems(void);
void InitializeTextureSystem(void);
void SdlCleanupAtExit(void);

// AST-based built-in handlers
Value executeBuiltinInitGraph(AST *node);
// ... etc ... keep all other AST-based prototypes

// VM-native built-in handlers - using 'struct VM_s*'
Value vm_builtin_initgraph(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_closegraph(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_graphloop(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_updatescreen(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_cleardevice(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_getmaxx(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_setrgbcolor(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_fillrect(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_inittextsystem(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_quittextsystem(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_gettextsize(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_getmousestate(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_destroytexture(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_rendercopyrect(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_setalphablend(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_rendertexttotexture(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_getticks(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_drawcircle(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_fillcircle(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_drawline(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_loadimagetotexture(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_createtargettexture(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_setrendertarget(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_drawpolygon(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_getpixelcolor(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_createtexture(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_rendercopy(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_rendercopyex(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_updatetexture(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_waitkeyevent(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_setcolor(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_putpixel(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_outtextxy(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_quitrequested(struct VM_s* vm, int arg_count, Value* args);

#ifdef __cplusplus
}
#endif

#endif // PSCAL_SDL_H
