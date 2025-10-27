#ifdef SDL
#include "sdl.h"
#include "core/utils.h"
#include "vm/vm.h"

Value vmBuiltinInitgraph3d(VM* vm, int arg_count, Value* args) {
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

        SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
        SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
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

    if (SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8) != 0 ||
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8) != 0 ||
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8) != 0 ||
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8) != 0 ||
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depthBits) != 0 ||
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, stencilBits) != 0 ||
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1) != 0) {
        runtimeError(vm, "Runtime error: SDL_GL_SetAttribute failed: %s", SDL_GetError());
        return makeVoid();
    }

    gSdlWindow = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
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

    if (SDL_GL_MakeCurrent(gSdlWindow, gSdlGLContext) != 0) {
        runtimeError(vm, "Runtime error: SDL_GL_MakeCurrent failed: %s", SDL_GetError());
        SDL_GL_DeleteContext(gSdlGLContext); gSdlGLContext = NULL;
        SDL_DestroyWindow(gSdlWindow); gSdlWindow = NULL;
        return makeVoid();
    }

    SDL_GL_SetSwapInterval(1);

    gSdlRenderer = NULL;
    initializeTextureSystem();

    SDL_PumpEvents();
    SDL_RaiseWindow(gSdlWindow);
#if SDL_VERSION_ATLEAST(2,0,5)
    SDL_SetWindowInputFocus(gSdlWindow);
#endif
    if (!SDL_IsTextInputActive()) {
        SDL_StartTextInput();
    }
    sdlEnsureInputWatch();

    return makeVoid();
}

Value vmBuiltinClosegraph3d(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) runtimeError(vm, "CloseGraph3D expects 0 arguments.");
    cleanupSdlWindowResources();
    return makeVoid();
}

Value vmBuiltinGlsetswapinterval(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "GLSetSwapInterval expects 1 integer argument.");
        return makeVoid();
    }
    if (!gSdlInitialized || !gSdlWindow || !gSdlGLContext) {
        runtimeError(vm, "Runtime error: GLSetSwapInterval requires an active OpenGL window. Call InitGraph3D first.");
        return makeVoid();
    }

    int interval = (int)AS_INTEGER(args[0]);
    if (SDL_GL_SetSwapInterval(interval) != 0) {
        runtimeError(vm, "Runtime error: SDL_GL_SetSwapInterval failed: %s", SDL_GetError());
    }

    return makeVoid();
}

Value vmBuiltinGlswapwindow(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "GLSwapWindow expects 0 arguments.");
        return makeVoid();
    }
    if (!gSdlInitialized || !gSdlWindow || !gSdlGLContext) {
        runtimeError(vm, "Runtime error: GLSwapWindow requires an active OpenGL window. Call InitGraph3D first.");
        return makeVoid();
    }

    SDL_GL_SwapWindow(gSdlWindow);
    return makeVoid();
}

#endif // SDL
