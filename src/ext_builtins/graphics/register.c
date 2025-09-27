#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"
#include "core/utils.h"
#include "vm/vm.h"

#ifdef SDL
#include "backend_ast/sdl.h"
#include "backend_ast/audio.h"
#include "backend_ast/gl.h"
#endif

typedef struct {
    const char *group;
    const char *display_name;
    const char *vm_name;
    BuiltinRoutineType type;
    VmBuiltinFn handler;
} GraphicsBuiltin;

#ifndef SDL
static Value graphicsUnavailableStub(struct VM_s* vm, int arg_count, Value* args) {
    (void)arg_count;
    (void)args;
    runtimeError(vm, "Graphics built-ins require SDL support. Rebuild with -DSDL=ON.");
    vm->abort_requested = true;
    return makeNil();
}
#endif

#ifdef SDL
#define GRAPHICS_HANDLER(fn) fn
#else
#define GRAPHICS_HANDLER(fn) graphicsUnavailableStub
#endif

static const GraphicsBuiltin graphics_builtins[] = {
    {"window", "InitGraph", "initgraph", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinInitgraph)},
    {"window", "CloseGraph", "closegraph", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinClosegraph)},
    {"window", "InitGraph3D", "initgraph3d", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinInitgraph3d)},
    {"window", "CloseGraph3D", "closegraph3d", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinClosegraph3d)},
    {"window", "GraphLoop", "graphloop", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGraphloop)},
    {"window", "UpdateScreen", "updatescreen", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinUpdatescreen)},
    {"window", "ClearDevice", "cleardevice", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinCleardevice)},
    {"window", "SetAlphaBlend", "setalphablend", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinSetalphablend)},
    {"window", "SetRenderTarget", "setrendertarget", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinSetrendertarget)},
    {"window", "GetMaxX", "getmaxx", BUILTIN_TYPE_FUNCTION, GRAPHICS_HANDLER(vmBuiltinGetmaxx)},
    {"window", "GetMaxY", "getmaxy", BUILTIN_TYPE_FUNCTION, GRAPHICS_HANDLER(vmBuiltinGetmaxy)},
    {"window", "GetTicks", "getticks", BUILTIN_TYPE_FUNCTION, GRAPHICS_HANDLER(vmBuiltinGetticks)},

    {"drawing", "SetColor", "setcolor", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinSetcolor)},
    {"drawing", "SetRGBColor", "setrgbcolor", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinSetrgbcolor)},
    {"drawing", "PutPixel", "putpixel", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinPutpixel)},
    {"drawing", "DrawLine", "drawline", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinDrawline)},
    {"drawing", "DrawRect", "drawrect", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinDrawrect)},
    {"drawing", "FillRect", "fillrect", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinFillrect)},
    {"drawing", "DrawCircle", "drawcircle", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinDrawcircle)},
    {"drawing", "FillCircle", "fillcircle", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinFillcircle)},
    {"drawing", "DrawPolygon", "drawpolygon", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinDrawpolygon)},
    {"drawing", "GetPixelColor", "getpixelcolor", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGetpixelcolor)},

    {"textures", "CreateTexture", "createtexture", BUILTIN_TYPE_FUNCTION, GRAPHICS_HANDLER(vmBuiltinCreatetexture)},
    {"textures", "CreateTargetTexture", "createtargettexture", BUILTIN_TYPE_FUNCTION, GRAPHICS_HANDLER(vmBuiltinCreatetargettexture)},
    {"textures", "DestroyTexture", "destroytexture", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinDestroytexture)},
    {"textures", "LoadImageToTexture", "loadimagetotexture", BUILTIN_TYPE_FUNCTION, GRAPHICS_HANDLER(vmBuiltinLoadimagetotexture)},
    {"textures", "RenderCopy", "rendercopy", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinRendercopy)},
    {"textures", "RenderCopyEx", "rendercopyex", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinRendercopyex)},
    {"textures", "RenderCopyRect", "rendercopyrect", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinRendercopyrect)},
    {"textures", "UpdateTexture", "updatetexture", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinUpdatetexture)},

    {"text", "InitTextSystem", "inittextsystem", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinInittextsystem)},
    {"text", "QuitTextSystem", "quittextsystem", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinQuittextsystem)},
    {"text", "OutTextXY", "outtextxy", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinOuttextxy)},
    {"text", "GetTextSize", "gettextsize", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGettextsize)},
    {"text", "RenderTextToTexture", "rendertexttotexture", BUILTIN_TYPE_FUNCTION, GRAPHICS_HANDLER(vmBuiltinRendertexttotexture)},

    {"input", "PollKey", "pollkey", BUILTIN_TYPE_FUNCTION, GRAPHICS_HANDLER(vmBuiltinPollkey)},
    {"input", "IsKeyDown", "iskeydown", BUILTIN_TYPE_FUNCTION, GRAPHICS_HANDLER(vmBuiltinIskeydown)},
    {"input", "WaitKeyEvent", "waitkeyevent", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinWaitkeyevent)},
    {"input", "GetMouseState", "getmousestate", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGetmousestate)},

    {"audio", "InitSoundSystem", "initsoundsystem", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinInitsoundsystem)},
    {"audio", "LoadSound", "loadsound", BUILTIN_TYPE_FUNCTION, GRAPHICS_HANDLER(vmBuiltinLoadsound)},
    {"audio", "PlaySound", "playsound", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinPlaysound)},
    {"audio", "FreeSound", "freesound", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinFreesound)},
    {"audio", "StopAllSounds", "stopallsounds", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinStopallsounds)},
    {"audio", "QuitSoundSystem", "quitsoundsystem", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinQuitsoundsystem)},
    {"audio", "IsSoundPlaying", "issoundplaying", BUILTIN_TYPE_FUNCTION, GRAPHICS_HANDLER(vmBuiltinIssoundplaying)},

    {"opengl", "GLBegin", "glbegin", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlbegin)},
    {"opengl", "GLClear", "glclear", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlclear)},
    {"opengl", "GLClearColor", "glclearcolor", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlclearcolor)},
    {"opengl", "GLClearDepth", "glcleardepth", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlcleardepth)},
    {"opengl", "GLColor3f", "glcolor3f", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlcolor3f)},
    {"opengl", "GLDepthTest", "gldepthtest", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGldepthtest)},
    {"opengl", "GLEnd", "glend", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlend)},
    {"opengl", "GLFrustum", "glfrustum", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlfrustum)},
    {"opengl", "GLLoadIdentity", "glloadidentity", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlloadidentity)},
    {"opengl", "GLMatrixMode", "glmatrixmode", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlmatrixmode)},
    {"opengl", "GLPopMatrix", "glpopmatrix", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlpopmatrix)},
    {"opengl", "GLPushMatrix", "glpushmatrix", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlpushmatrix)},
    {"opengl", "GLRotatef", "glrotatef", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlrotatef)},
    {"opengl", "GLScalef", "glscalef", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlscalef)},
    {"opengl", "GLPerspective", "glperspective", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlperspective)},
    {"opengl", "GLSetSwapInterval", "glsetswapinterval", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlsetswapinterval)},
    {"opengl", "GLSwapWindow", "glswapwindow", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlswapwindow)},
    {"opengl", "GLTranslatef", "gltranslatef", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGltranslatef)},
    {"opengl", "GLVertex3f", "glvertex3f", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlvertex3f)},
    {"opengl", "GLViewport", "glviewport", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlviewport)},
    {"opengl", "GLColor4f", "glcolor4f", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlcolor4f)},
    {"opengl", "GLNormal3f", "glnormal3f", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlnormal3f)},
    {"opengl", "GLEnable", "glenable", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlenable)},
    {"opengl", "GLDisable", "gldisable", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGldisable)},
    {"opengl", "GLShadeModel", "glshademodel", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlshademodel)},
    {"opengl", "GLLightfv", "gllightfv", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGllightfv)},
    {"opengl", "GLMaterialfv", "glmaterialfv", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlmaterialfv)},
    {"opengl", "GLMaterialf", "glmaterialf", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlmaterialf)},
    {"opengl", "GLColorMaterial", "glcolormaterial", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlcolormaterial)},
    {"opengl", "GLBlendFunc", "glblendfunc", BUILTIN_TYPE_PROCEDURE, GRAPHICS_HANDLER(vmBuiltinGlblendfunc)},
    {"opengl", "GLIsHardwareAccelerated", "glishardwareaccelerated", BUILTIN_TYPE_FUNCTION, GRAPHICS_HANDLER(vmBuiltinGlishardwareaccelerated)},
};

void registerGraphicsBuiltins(void) {
    const char *category = "graphics";
    extBuiltinRegisterCategory(category);
    extBuiltinRegisterGroup(category, "window");
    extBuiltinRegisterGroup(category, "drawing");
    extBuiltinRegisterGroup(category, "textures");
    extBuiltinRegisterGroup(category, "text");
    extBuiltinRegisterGroup(category, "input");
    extBuiltinRegisterGroup(category, "audio");
    extBuiltinRegisterGroup(category, "opengl");

    for (size_t i = 0; i < sizeof(graphics_builtins) / sizeof(graphics_builtins[0]); ++i) {
        const GraphicsBuiltin *entry = &graphics_builtins[i];
        extBuiltinRegisterFunction(category, entry->group, entry->display_name);
        registerVmBuiltin(entry->vm_name, entry->handler, entry->type, entry->display_name);
    }
}
