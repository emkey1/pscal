#ifdef SDL
#include "pscal_sdl_runtime.h"
#include "core/utils.h"
#include "vm/vm.h"
#include "sdl_ios_dispatch.h"
#if PSCALI_HAS_SYSWM
#include PSCALI_SDL_SYSWM_HEADER
#endif

#if defined(PSCAL_TARGET_IOS)
__attribute__((weak)) void pscalIOSPromoteSDLWindow(void);
__attribute__((weak)) void pscalRuntimeSdlDidOpen(void);
__attribute__((weak)) void pscalIOSPromoteSDLNativeWindow(void *nativeWindow);
static inline void sdlPromoteIosWindowIfAvailable(void) {
    if (pscalIOSPromoteSDLWindow) {
        pscalIOSPromoteSDLWindow();
    }
}
static inline void sdlNotifyUiSdlDidOpen(void) {
    if (pscalRuntimeSdlDidOpen) {
        pscalRuntimeSdlDidOpen();
    }
}
#endif

static bool sdlGlSetAttribute(SDL_GLattr attr, int value) {
#if defined(PSCALI_SDL3)
    return SDL_GL_SetAttribute(attr, value);
#else
    return SDL_GL_SetAttribute(attr, value) == 0;
#endif
}

static bool sdlGlMakeCurrent(SDL_Window* window, SDL_GLContext context) {
#if defined(PSCALI_SDL3)
    return SDL_GL_MakeCurrent(window, context);
#else
    return SDL_GL_MakeCurrent(window, context) == 0;
#endif
}

static bool sdlGlSetSwapIntervalValue(int interval) {
#if defined(PSCALI_SDL3)
    return SDL_GL_SetSwapInterval(interval);
#else
    return SDL_GL_SetSwapInterval(interval) == 0;
#endif
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinInitgraph3d) {
    if (arg_count != 5 || !IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1]) || args[2].type != TYPE_STRING
        || !IS_INTLIKE(args[3]) || !IS_INTLIKE(args[4])) {
        runtimeError(vm, "VM Error: InitGraph3D expects (Integer, Integer, String, Integer, Integer)");
        return makeVoid();
    }

    if (!gSdlInitialized) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
            runtimeError(vm, "Runtime error: SDL_Init failed in InitGraph3D: %s", SDL_GetError());
            return makeVoid();
        }
        gSdlInitialized = true;

#ifdef SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH
        SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
#endif
#if PSCALI_HAS_SYSWM
        SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
#endif
    }

    cleanupSdlWindowResources();

    int width = (int)AS_INTEGER(args[0]);
    int height = (int)AS_INTEGER(args[1]);
    const char* title = args[2].s_val ? args[2].s_val : "Pscal 3D Graphics";
    int depthBits = (int)AS_INTEGER(args[3]);
    int stencilBits = (int)AS_INTEGER(args[4]);

    if (width <= 0 || height <= 0) {
        runtimeError(vm, "Runtime error: InitGraph3D width and height must be positive.");
        return makeVoid();
    }
    if (depthBits < 0 || stencilBits < 0) {
        runtimeError(vm, "Runtime error: InitGraph3D depth and stencil sizes must be non-negative.");
        return makeVoid();
    }

    if (!sdlGlSetAttribute(SDL_GL_RED_SIZE, 8) ||
        !sdlGlSetAttribute(SDL_GL_GREEN_SIZE, 8) ||
        !sdlGlSetAttribute(SDL_GL_BLUE_SIZE, 8) ||
        !sdlGlSetAttribute(SDL_GL_ALPHA_SIZE, 8) ||
        !sdlGlSetAttribute(SDL_GL_DEPTH_SIZE, depthBits) ||
        !sdlGlSetAttribute(SDL_GL_STENCIL_SIZE, stencilBits) ||
        !sdlGlSetAttribute(SDL_GL_DOUBLEBUFFER, 1)) {
        runtimeError(vm, "Runtime error: SDL_GL_SetAttribute failed: %s", SDL_GetError());
        return makeVoid();
    }

#if defined(PSCALI_SDL3)
    gSdlWindow = SDL_CreateWindow(title, width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (gSdlWindow) {
        SDL_SetWindowPosition(gSdlWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
#else
    gSdlWindow = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
#endif
    if (!gSdlWindow) {
        runtimeError(vm, "Runtime error: SDL_CreateWindow failed: %s", SDL_GetError());
        return makeVoid();
    }

    gSdlWidth = width;
    gSdlHeight = height;

    gSdlGLContext = SDL_GL_CreateContext(gSdlWindow);
    if (!gSdlGLContext) {
        runtimeError(vm, "Runtime error: SDL_GL_CreateContext failed: %s", SDL_GetError());
        SDL_DestroyWindow(gSdlWindow); gSdlWindow = NULL;
        return makeVoid();
    }

    if (!sdlGlMakeCurrent(gSdlWindow, gSdlGLContext)) {
        runtimeError(vm, "Runtime error: SDL_GL_MakeCurrent failed: %s", SDL_GetError());
        SDL_GL_DeleteContext(gSdlGLContext); gSdlGLContext = NULL;
        SDL_DestroyWindow(gSdlWindow); gSdlWindow = NULL;
        return makeVoid();
    }

    sdlGlSetSwapIntervalValue(1);

    gSdlRenderer = NULL;
    initializeTextureSystem();

    SDL_PumpEvents();
    SDL_RaiseWindow(gSdlWindow);
#if SDL_VERSION_ATLEAST(2,0,5)
#ifndef PSCALI_SDL3
    SDL_SetWindowInputFocus(gSdlWindow);
#endif
#endif
#if defined(PSCAL_TARGET_IOS)
#if PSCALI_HAS_SYSWM
    if (pscalIOSPromoteSDLNativeWindow) {
        SDL_SysWMinfo wmInfo;
        SDL_VERSION(&wmInfo.version);
        if (SDL_GetWindowWMInfo(gSdlWindow, &wmInfo)) {
            void *nativeWindow = NULL;
            if (wmInfo.subsystem == SDL_SYSWM_UIKIT && wmInfo.info.uikit.window) {
                nativeWindow = (void *)wmInfo.info.uikit.window;
            } else if (wmInfo.info.uikit.window) {
                nativeWindow = (void *)wmInfo.info.uikit.window;
            }
            if (nativeWindow) {
                pscalIOSPromoteSDLNativeWindow(nativeWindow);
            }
        }
    }
#endif
    sdlPromoteIosWindowIfAvailable();
#endif
    sdlEnsureInputWatch();
    sdlFlushSpuriousQuitEvents();

    if (!sdlTextInputActive()) {
        sdlStartTextInput();
    }
#if defined(PSCAL_TARGET_IOS)
    sdlNotifyUiSdlDidOpen();
#endif

    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinClosegraph3d) {
    if (arg_count != 0) runtimeError(vm, "CloseGraph3D expects 0 arguments.");
    cleanupSdlWindowResources();
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinGlsetswapinterval) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "GLSetSwapInterval expects 1 integer argument.");
        return makeVoid();
    }
#if defined(PSCAL_TARGET_IOS)
    return makeVoid();
#else
    if (!gSdlInitialized || !gSdlWindow || !gSdlGLContext) {
        runtimeError(vm, "Runtime error: GLSetSwapInterval requires an active OpenGL window. Call InitGraph3D first.");
        return makeVoid();
    }

    int interval = (int)AS_INTEGER(args[0]);
    if (!sdlGlSetSwapIntervalValue(interval)) {
        runtimeError(vm, "Runtime error: SDL_GL_SetSwapInterval failed: %s", SDL_GetError());
    }

    return makeVoid();
#endif
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinGlswapwindow) {
    if (arg_count != 0) {
        runtimeError(vm, "GLSwapWindow expects 0 arguments.");
        return makeVoid();
    }
#if defined(PSCAL_TARGET_IOS)
    return makeVoid();
#else
    if (!gSdlInitialized || !gSdlWindow || !gSdlGLContext) {
        runtimeError(vm, "Runtime error: GLSwapWindow requires an active OpenGL window. Call InitGraph3D first.");
        return makeVoid();
    }

    SDL_GL_SwapWindow(gSdlWindow);
    return makeVoid();
#endif
}

#endif // SDL
