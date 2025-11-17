#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"
#include "core/utils.h"
#include "vm/vm.h"

#ifdef SDL
#include "backend_ast/pscal_sdl_runtime.h"
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

#ifdef SDL
static const GraphicsBuiltin graphics_builtins[] = {
    {"window", "InitGraph", "initgraph", BUILTIN_TYPE_PROCEDURE, vmBuiltinInitgraph},
    {"window", "CloseGraph", "closegraph", BUILTIN_TYPE_PROCEDURE, vmBuiltinClosegraph},
    {"window", "InitGraph3D", "initgraph3d", BUILTIN_TYPE_PROCEDURE, vmBuiltinInitgraph3d},
    {"window", "CloseGraph3D", "closegraph3d", BUILTIN_TYPE_PROCEDURE, vmBuiltinClosegraph3d},
    {"window", "GraphLoop", "graphloop", BUILTIN_TYPE_PROCEDURE, vmBuiltinGraphloop},
    {"window", "UpdateScreen", "updatescreen", BUILTIN_TYPE_PROCEDURE, vmBuiltinUpdatescreen},
    {"window", "ClearDevice", "cleardevice", BUILTIN_TYPE_PROCEDURE, vmBuiltinCleardevice},
    {"window", "SetAlphaBlend", "setalphablend", BUILTIN_TYPE_PROCEDURE, vmBuiltinSetalphablend},
    {"window", "SetRenderTarget", "setrendertarget", BUILTIN_TYPE_PROCEDURE, vmBuiltinSetrendertarget},
    {"window", "GetMaxX", "getmaxx", BUILTIN_TYPE_FUNCTION, vmBuiltinGetmaxx},
    {"window", "GetMaxY", "getmaxy", BUILTIN_TYPE_FUNCTION, vmBuiltinGetmaxy},
    {"window", "GetTicks", "getticks", BUILTIN_TYPE_FUNCTION, vmBuiltinGetticks},

    {"drawing", "SetColor", "setcolor", BUILTIN_TYPE_PROCEDURE, vmBuiltinSetcolor},
    {"drawing", "SetRGBColor", "setrgbcolor", BUILTIN_TYPE_PROCEDURE, vmBuiltinSetrgbcolor},
    {"drawing", "PutPixel", "putpixel", BUILTIN_TYPE_PROCEDURE, vmBuiltinPutpixel},
    {"drawing", "DrawLine", "drawline", BUILTIN_TYPE_PROCEDURE, vmBuiltinDrawline},
    {"drawing", "DrawRect", "drawrect", BUILTIN_TYPE_PROCEDURE, vmBuiltinDrawrect},
    {"drawing", "FillRect", "fillrect", BUILTIN_TYPE_PROCEDURE, vmBuiltinFillrect},
    {"drawing", "DrawCircle", "drawcircle", BUILTIN_TYPE_PROCEDURE, vmBuiltinDrawcircle},
    {"drawing", "FillCircle", "fillcircle", BUILTIN_TYPE_PROCEDURE, vmBuiltinFillcircle},
    {"drawing", "DrawPolygon", "drawpolygon", BUILTIN_TYPE_PROCEDURE, vmBuiltinDrawpolygon},
    {"drawing", "GetPixelColor", "getpixelcolor", BUILTIN_TYPE_PROCEDURE, vmBuiltinGetpixelcolor},

    {"textures", "CreateTexture", "createtexture", BUILTIN_TYPE_FUNCTION, vmBuiltinCreatetexture},
    {"textures", "CreateTargetTexture", "createtargettexture", BUILTIN_TYPE_FUNCTION, vmBuiltinCreatetargettexture},
    {"textures", "DestroyTexture", "destroytexture", BUILTIN_TYPE_PROCEDURE, vmBuiltinDestroytexture},
    {"textures", "LoadImageToTexture", "loadimagetotexture", BUILTIN_TYPE_FUNCTION, vmBuiltinLoadimagetotexture},
    {"textures", "RenderCopy", "rendercopy", BUILTIN_TYPE_PROCEDURE, vmBuiltinRendercopy},
    {"textures", "RenderCopyEx", "rendercopyex", BUILTIN_TYPE_PROCEDURE, vmBuiltinRendercopyex},
    {"textures", "RenderCopyRect", "rendercopyrect", BUILTIN_TYPE_PROCEDURE, vmBuiltinRendercopyrect},
    {"textures", "UpdateTexture", "updatetexture", BUILTIN_TYPE_PROCEDURE, vmBuiltinUpdatetexture},

    {"text", "InitTextSystem", "inittextsystem", BUILTIN_TYPE_PROCEDURE, vmBuiltinInittextsystem},
    {"text", "QuitTextSystem", "quittextsystem", BUILTIN_TYPE_PROCEDURE, vmBuiltinQuittextsystem},
    {"text", "OutTextXY", "outtextxy", BUILTIN_TYPE_PROCEDURE, vmBuiltinOuttextxy},
    {"text", "GetTextSize", "gettextsize", BUILTIN_TYPE_PROCEDURE, vmBuiltinGettextsize},
    {"text", "RenderTextToTexture", "rendertexttotexture", BUILTIN_TYPE_FUNCTION, vmBuiltinRendertexttotexture},

    {"input", "PollKey", "pollkey", BUILTIN_TYPE_FUNCTION, vmBuiltinPollkey},
    {"input", "IsKeyDown", "iskeydown", BUILTIN_TYPE_FUNCTION, vmBuiltinIskeydown},
    {"input", "WaitKeyEvent", "waitkeyevent", BUILTIN_TYPE_PROCEDURE, vmBuiltinWaitkeyevent},
    {"input", "GetMouseState", "getmousestate", BUILTIN_TYPE_PROCEDURE, vmBuiltinGetmousestate},

    {"audio", "InitSoundSystem", "initsoundsystem", BUILTIN_TYPE_PROCEDURE, vmBuiltinInitsoundsystem},
    {"audio", "LoadSound", "loadsound", BUILTIN_TYPE_FUNCTION, vmBuiltinLoadsound},
    {"audio", "PlaySound", "playsound", BUILTIN_TYPE_PROCEDURE, vmBuiltinPlaysound},
    {"audio", "FreeSound", "freesound", BUILTIN_TYPE_PROCEDURE, vmBuiltinFreesound},
    {"audio", "StopAllSounds", "stopallsounds", BUILTIN_TYPE_PROCEDURE, vmBuiltinStopallsounds},
    {"audio", "QuitSoundSystem", "quitsoundsystem", BUILTIN_TYPE_PROCEDURE, vmBuiltinQuitsoundsystem},
    {"audio", "IsSoundPlaying", "issoundplaying", BUILTIN_TYPE_FUNCTION, vmBuiltinIssoundplaying},

    {"opengl", "GLBegin", "glbegin", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlbegin},
    {"opengl", "GLClear", "glclear", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlclear},
    {"opengl", "GLClearColor", "glclearcolor", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlclearcolor},
    {"opengl", "GLClearDepth", "glcleardepth", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlcleardepth},
    {"opengl", "GLColor3f", "glcolor3f", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlcolor3f},
    {"opengl", "GLDepthTest", "gldepthtest", BUILTIN_TYPE_PROCEDURE, vmBuiltinGldepthtest},
    {"opengl", "GLEnd", "glend", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlend},
    {"opengl", "GLFrustum", "glfrustum", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlfrustum},
    {"opengl", "GLLoadIdentity", "glloadidentity", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlloadidentity},
    {"opengl", "GLMatrixMode", "glmatrixmode", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlmatrixmode},
    {"opengl", "GLPopMatrix", "glpopmatrix", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlpopmatrix},
    {"opengl", "GLPushMatrix", "glpushmatrix", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlpushmatrix},
    {"opengl", "GLRotatef", "glrotatef", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlrotatef},
    {"opengl", "GLScalef", "glscalef", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlscalef},
    {"opengl", "GLPerspective", "glperspective", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlperspective},
    {"opengl", "GLSetSwapInterval", "glsetswapinterval", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlsetswapinterval},
    {"opengl", "GLSwapWindow", "glswapwindow", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlswapwindow},
    {"opengl", "GLTranslatef", "gltranslatef", BUILTIN_TYPE_PROCEDURE, vmBuiltinGltranslatef},
    {"opengl", "GLVertex3f", "glvertex3f", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlvertex3f},
    {"opengl", "GLViewport", "glviewport", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlviewport},
    {"opengl", "GLColor4f", "glcolor4f", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlcolor4f},
    {"opengl", "GLNormal3f", "glnormal3f", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlnormal3f},
    {"opengl", "GLEnable", "glenable", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlenable},
    {"opengl", "GLDisable", "gldisable", BUILTIN_TYPE_PROCEDURE, vmBuiltinGldisable},
    {"opengl", "GLShadeModel", "glshademodel", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlshademodel},
    {"opengl", "GLLightfv", "gllightfv", BUILTIN_TYPE_PROCEDURE, vmBuiltinGllightfv},
    {"opengl", "GLMaterialfv", "glmaterialfv", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlmaterialfv},
    {"opengl", "GLMaterialf", "glmaterialf", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlmaterialf},
    {"opengl", "GLColorMaterial", "glcolormaterial", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlcolormaterial},
    {"opengl", "GLBlendFunc", "glblendfunc", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlblendfunc},
    {"opengl", "GLCullFace", "glcullface", BUILTIN_TYPE_PROCEDURE, vmBuiltinGlcullface},
    {"opengl", "GLIsHardwareAccelerated", "glishardwareaccelerated", BUILTIN_TYPE_FUNCTION, vmBuiltinGlishardwareaccelerated},
    {"window", "GetScreenSize", "getscreensize", BUILTIN_TYPE_FUNCTION, vmBuiltinGetscreensize}, // Append new builtins to preserve existing IDs.
    {"input", "PollKeyAny", "pollkeyany", BUILTIN_TYPE_FUNCTION, vmBuiltinPollkeyany}, // Append new builtins to preserve existing IDs.
    {"opengl", "GLLineWidth", "gllinewidth", BUILTIN_TYPE_PROCEDURE, vmBuiltinGllinewidth}, // Append new builtins to preserve existing IDs.
    {"opengl", "GLSaveFramebufferPng", "glsaveframebufferpng", BUILTIN_TYPE_FUNCTION, vmBuiltinGlsaveframebufferpng}, // Append new builtins to preserve existing IDs.
    {"opengl", "GLDepthMask", "gldepthmask", BUILTIN_TYPE_PROCEDURE, vmBuiltinGldepthmask}, // Append new builtins to preserve existing IDs.
    {"opengl", "GLDepthFunc", "gldepthfunc", BUILTIN_TYPE_PROCEDURE, vmBuiltinGldepthfunc}, // Append new builtins to preserve existing IDs.
};
#endif

void registerGraphicsBuiltins(void) {
#ifdef SDL
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
#endif
}
