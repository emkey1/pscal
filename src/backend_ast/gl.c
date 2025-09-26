#ifdef SDL
#include "backend_ast/gl.h"
#include "backend_ast/sdl.h"
#include "core/utils.h"
#include "vm/vm.h"

#include <SDL2/SDL_opengl.h>
#include <stdbool.h>
#include <strings.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static bool ensureGlContext(VM* vm, const char* name) {
    if (!gSdlInitialized || !gSdlWindow || !gSdlGLContext) {
        runtimeError(vm, "Runtime error: %s requires an active OpenGL window. Call InitGraph3D first.", name);
        return false;
    }
    return true;
}

static bool valueToFloat(Value v, float* out) {
    if (isRealType(v.type)) {
        *out = (float)AS_REAL(v);
        return true;
    }
    if (IS_INTLIKE(v)) {
        *out = (float)AS_INTEGER(v);
        return true;
    }
    return false;
}

static bool valueToDouble(Value v, double* out) {
    if (isRealType(v.type)) {
        *out = (double)AS_REAL(v);
        return true;
    }
    if (IS_INTLIKE(v)) {
        *out = (double)AS_INTEGER(v);
        return true;
    }
    return false;
}

static bool parseMatrixMode(Value arg, GLenum* mode) {
    if (IS_INTLIKE(arg)) {
        *mode = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (arg.type == TYPE_STRING && arg.s_val) {
        if (strcasecmp(arg.s_val, "modelview") == 0) {
            *mode = GL_MODELVIEW;
            return true;
        }
        if (strcasecmp(arg.s_val, "projection") == 0) {
            *mode = GL_PROJECTION;
            return true;
        }
        if (strcasecmp(arg.s_val, "texture") == 0) {
            *mode = GL_TEXTURE;
            return true;
        }
    }
    return false;
}

static bool parsePrimitive(Value arg, GLenum* primitive) {
    if (IS_INTLIKE(arg)) {
        *primitive = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (arg.type == TYPE_STRING && arg.s_val) {
        if (strcasecmp(arg.s_val, "points") == 0) {
            *primitive = GL_POINTS;
            return true;
        }
        if (strcasecmp(arg.s_val, "lines") == 0) {
            *primitive = GL_LINES;
            return true;
        }
        if (strcasecmp(arg.s_val, "line_strip") == 0 || strcasecmp(arg.s_val, "linestrip") == 0) {
            *primitive = GL_LINE_STRIP;
            return true;
        }
        if (strcasecmp(arg.s_val, "line_loop") == 0 || strcasecmp(arg.s_val, "lineloop") == 0) {
            *primitive = GL_LINE_LOOP;
            return true;
        }
        if (strcasecmp(arg.s_val, "triangles") == 0) {
            *primitive = GL_TRIANGLES;
            return true;
        }
        if (strcasecmp(arg.s_val, "triangle_strip") == 0 || strcasecmp(arg.s_val, "trianglestrip") == 0) {
            *primitive = GL_TRIANGLE_STRIP;
            return true;
        }
        if (strcasecmp(arg.s_val, "triangle_fan") == 0 || strcasecmp(arg.s_val, "trianglefan") == 0) {
            *primitive = GL_TRIANGLE_FAN;
            return true;
        }
#ifdef GL_QUADS
        if (strcasecmp(arg.s_val, "quads") == 0) {
            *primitive = GL_QUADS;
            return true;
        }
#endif
#ifdef GL_QUAD_STRIP
        if (strcasecmp(arg.s_val, "quad_strip") == 0 || strcasecmp(arg.s_val, "quadstrip") == 0) {
            *primitive = GL_QUAD_STRIP;
            return true;
        }
#endif
#ifdef GL_POLYGON
        if (strcasecmp(arg.s_val, "polygon") == 0) {
            *primitive = GL_POLYGON;
            return true;
        }
#endif
    }
    return false;
}

static bool parseCapability(Value arg, GLenum* cap) {
    if (IS_INTLIKE(arg)) {
        *cap = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (arg.type == TYPE_STRING && arg.s_val) {
#ifdef GL_LIGHTING
        if (strcasecmp(arg.s_val, "lighting") == 0) {
            *cap = GL_LIGHTING;
            return true;
        }
#endif
#ifdef GL_LIGHT0
        if (strcasecmp(arg.s_val, "light0") == 0) {
            *cap = GL_LIGHT0;
            return true;
        }
#endif
#ifdef GL_LIGHT1
        if (strcasecmp(arg.s_val, "light1") == 0) {
            *cap = GL_LIGHT1;
            return true;
        }
#endif
#ifdef GL_LIGHT2
        if (strcasecmp(arg.s_val, "light2") == 0) {
            *cap = GL_LIGHT2;
            return true;
        }
#endif
#ifdef GL_LIGHT3
        if (strcasecmp(arg.s_val, "light3") == 0) {
            *cap = GL_LIGHT3;
            return true;
        }
#endif
#ifdef GL_LIGHT4
        if (strcasecmp(arg.s_val, "light4") == 0) {
            *cap = GL_LIGHT4;
            return true;
        }
#endif
#ifdef GL_LIGHT5
        if (strcasecmp(arg.s_val, "light5") == 0) {
            *cap = GL_LIGHT5;
            return true;
        }
#endif
#ifdef GL_LIGHT6
        if (strcasecmp(arg.s_val, "light6") == 0) {
            *cap = GL_LIGHT6;
            return true;
        }
#endif
#ifdef GL_LIGHT7
        if (strcasecmp(arg.s_val, "light7") == 0) {
            *cap = GL_LIGHT7;
            return true;
        }
#endif
#ifdef GL_COLOR_MATERIAL
        if (strcasecmp(arg.s_val, "color_material") == 0) {
            *cap = GL_COLOR_MATERIAL;
            return true;
        }
#endif
#ifdef GL_NORMALIZE
        if (strcasecmp(arg.s_val, "normalize") == 0) {
            *cap = GL_NORMALIZE;
            return true;
        }
#endif
        if (strcasecmp(arg.s_val, "blend") == 0) {
            *cap = GL_BLEND;
            return true;
        }
        if (strcasecmp(arg.s_val, "cull_face") == 0 ||
            strcasecmp(arg.s_val, "cullface") == 0) {
            *cap = GL_CULL_FACE;
            return true;
        }
        if (strcasecmp(arg.s_val, "depth_test") == 0 ||
            strcasecmp(arg.s_val, "depthtest") == 0) {
            *cap = GL_DEPTH_TEST;
            return true;
        }
#ifdef GL_FOG
        if (strcasecmp(arg.s_val, "fog") == 0) {
            *cap = GL_FOG;
            return true;
        }
#endif
        if (strcasecmp(arg.s_val, "scissor_test") == 0 ||
            strcasecmp(arg.s_val, "scissortest") == 0) {
            *cap = GL_SCISSOR_TEST;
            return true;
        }
        if (strcasecmp(arg.s_val, "texture_2d") == 0) {
            *cap = GL_TEXTURE_2D;
            return true;
        }
    }
    return false;
}

static bool parseShadeModel(Value arg, GLenum* mode) {
    if (IS_INTLIKE(arg)) {
        *mode = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (arg.type == TYPE_STRING && arg.s_val) {
        if (strcasecmp(arg.s_val, "smooth") == 0) {
            *mode = GL_SMOOTH;
            return true;
        }
        if (strcasecmp(arg.s_val, "flat") == 0) {
            *mode = GL_FLAT;
            return true;
        }
    }
    return false;
}

static bool parseLight(Value arg, GLenum* light) {
    if (IS_INTLIKE(arg)) {
        *light = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (arg.type == TYPE_STRING && arg.s_val) {
#ifdef GL_LIGHT0
        if (strcasecmp(arg.s_val, "light0") == 0) {
            *light = GL_LIGHT0;
            return true;
        }
#endif
#ifdef GL_LIGHT1
        if (strcasecmp(arg.s_val, "light1") == 0) {
            *light = GL_LIGHT1;
            return true;
        }
#endif
#ifdef GL_LIGHT2
        if (strcasecmp(arg.s_val, "light2") == 0) {
            *light = GL_LIGHT2;
            return true;
        }
#endif
#ifdef GL_LIGHT3
        if (strcasecmp(arg.s_val, "light3") == 0) {
            *light = GL_LIGHT3;
            return true;
        }
#endif
#ifdef GL_LIGHT4
        if (strcasecmp(arg.s_val, "light4") == 0) {
            *light = GL_LIGHT4;
            return true;
        }
#endif
#ifdef GL_LIGHT5
        if (strcasecmp(arg.s_val, "light5") == 0) {
            *light = GL_LIGHT5;
            return true;
        }
#endif
#ifdef GL_LIGHT6
        if (strcasecmp(arg.s_val, "light6") == 0) {
            *light = GL_LIGHT6;
            return true;
        }
#endif
#ifdef GL_LIGHT7
        if (strcasecmp(arg.s_val, "light7") == 0) {
            *light = GL_LIGHT7;
            return true;
        }
#endif
    }
    return false;
}

static bool parseLightParam(Value arg, GLenum* pname) {
    if (IS_INTLIKE(arg)) {
        *pname = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (arg.type == TYPE_STRING && arg.s_val) {
        if (strcasecmp(arg.s_val, "position") == 0) {
            *pname = GL_POSITION;
            return true;
        }
        if (strcasecmp(arg.s_val, "diffuse") == 0) {
            *pname = GL_DIFFUSE;
            return true;
        }
        if (strcasecmp(arg.s_val, "specular") == 0) {
            *pname = GL_SPECULAR;
            return true;
        }
        if (strcasecmp(arg.s_val, "ambient") == 0) {
            *pname = GL_AMBIENT;
            return true;
        }
    }
    return false;
}

static bool parseMaterialFace(Value arg, GLenum* face) {
    if (IS_INTLIKE(arg)) {
        *face = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (arg.type == TYPE_STRING && arg.s_val) {
        if (strcasecmp(arg.s_val, "front") == 0) {
            *face = GL_FRONT;
            return true;
        }
        if (strcasecmp(arg.s_val, "back") == 0) {
            *face = GL_BACK;
            return true;
        }
        if (strcasecmp(arg.s_val, "front_and_back") == 0 ||
            strcasecmp(arg.s_val, "frontandback") == 0) {
            *face = GL_FRONT_AND_BACK;
            return true;
        }
    }
    return false;
}

static bool parseMaterialParam(Value arg, GLenum* pname) {
    if (IS_INTLIKE(arg)) {
        *pname = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (arg.type == TYPE_STRING && arg.s_val) {
        if (strcasecmp(arg.s_val, "ambient") == 0) {
            *pname = GL_AMBIENT;
            return true;
        }
        if (strcasecmp(arg.s_val, "diffuse") == 0) {
            *pname = GL_DIFFUSE;
            return true;
        }
        if (strcasecmp(arg.s_val, "specular") == 0) {
            *pname = GL_SPECULAR;
            return true;
        }
        if (strcasecmp(arg.s_val, "emission") == 0) {
            *pname = GL_EMISSION;
            return true;
        }
        if (strcasecmp(arg.s_val, "ambient_and_diffuse") == 0 ||
            strcasecmp(arg.s_val, "ambientdiffuse") == 0) {
            *pname = GL_AMBIENT_AND_DIFFUSE;
            return true;
        }
        if (strcasecmp(arg.s_val, "shininess") == 0) {
            *pname = GL_SHININESS;
            return true;
        }
    }
    return false;
}

static bool parseColorMaterialMode(Value arg, GLenum* mode) {
    if (IS_INTLIKE(arg)) {
        *mode = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (arg.type == TYPE_STRING && arg.s_val) {
        if (strcasecmp(arg.s_val, "ambient") == 0) {
            *mode = GL_AMBIENT;
            return true;
        }
        if (strcasecmp(arg.s_val, "diffuse") == 0) {
            *mode = GL_DIFFUSE;
            return true;
        }
        if (strcasecmp(arg.s_val, "ambient_and_diffuse") == 0 ||
            strcasecmp(arg.s_val, "ambientdiffuse") == 0) {
            *mode = GL_AMBIENT_AND_DIFFUSE;
            return true;
        }
        if (strcasecmp(arg.s_val, "specular") == 0) {
            *mode = GL_SPECULAR;
            return true;
        }
        if (strcasecmp(arg.s_val, "emission") == 0) {
            *mode = GL_EMISSION;
            return true;
        }
    }
    return false;
}

static bool parseBlendFactor(Value arg, GLenum* factor) {
    if (IS_INTLIKE(arg)) {
        *factor = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (arg.type == TYPE_STRING && arg.s_val) {
        if (strcasecmp(arg.s_val, "zero") == 0) {
            *factor = GL_ZERO;
            return true;
        }
        if (strcasecmp(arg.s_val, "one") == 0) {
            *factor = GL_ONE;
            return true;
        }
        if (strcasecmp(arg.s_val, "src_color") == 0 ||
            strcasecmp(arg.s_val, "srccolor") == 0) {
            *factor = GL_SRC_COLOR;
            return true;
        }
        if (strcasecmp(arg.s_val, "one_minus_src_color") == 0 ||
            strcasecmp(arg.s_val, "oneminussrccolor") == 0) {
            *factor = GL_ONE_MINUS_SRC_COLOR;
            return true;
        }
        if (strcasecmp(arg.s_val, "dst_color") == 0 ||
            strcasecmp(arg.s_val, "dstcolor") == 0) {
            *factor = GL_DST_COLOR;
            return true;
        }
        if (strcasecmp(arg.s_val, "one_minus_dst_color") == 0 ||
            strcasecmp(arg.s_val, "oneminusdstcolor") == 0) {
            *factor = GL_ONE_MINUS_DST_COLOR;
            return true;
        }
        if (strcasecmp(arg.s_val, "src_alpha") == 0 ||
            strcasecmp(arg.s_val, "srcalpha") == 0) {
            *factor = GL_SRC_ALPHA;
            return true;
        }
        if (strcasecmp(arg.s_val, "one_minus_src_alpha") == 0 ||
            strcasecmp(arg.s_val, "oneminussrcalpha") == 0) {
            *factor = GL_ONE_MINUS_SRC_ALPHA;
            return true;
        }
        if (strcasecmp(arg.s_val, "dst_alpha") == 0 ||
            strcasecmp(arg.s_val, "dstalpha") == 0) {
            *factor = GL_DST_ALPHA;
            return true;
        }
        if (strcasecmp(arg.s_val, "one_minus_dst_alpha") == 0 ||
            strcasecmp(arg.s_val, "oneminusdstalpha") == 0) {
            *factor = GL_ONE_MINUS_DST_ALPHA;
            return true;
        }
    }
    return false;
}

Value vmBuiltinGlclearcolor(VM* vm, int arg_count, Value* args) {
    if (arg_count != 4) {
        runtimeError(vm, "GLClearColor expects 4 numeric arguments (r, g, b, a).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLClearColor")) return makeVoid();

    float comps[4];
    for (int i = 0; i < 4; ++i) {
        if (!valueToFloat(args[i], &comps[i])) {
            runtimeError(vm, "GLClearColor component %d must be numeric.", i + 1);
            return makeVoid();
        }
        if (comps[i] < 0.0f) comps[i] = 0.0f;
        if (comps[i] > 1.0f) comps[i] = 1.0f;
    }

    glClearColor(comps[0], comps[1], comps[2], comps[3]);
    return makeVoid();
}

Value vmBuiltinGlclear(VM* vm, int arg_count, Value* args) {
    if (arg_count > 1) {
        runtimeError(vm, "GLClear expects 0 or 1 argument (GLbitfield mask).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLClear")) return makeVoid();

    GLbitfield mask = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT;
    if (arg_count == 1) {
        if (!IS_INTLIKE(args[0])) {
            runtimeError(vm, "GLClear mask must be an integer-like value.");
            return makeVoid();
        }
        mask = (GLbitfield)AS_INTEGER(args[0]);
    }

    glClear(mask);
    return makeVoid();
}

Value vmBuiltinGlcleardepth(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "GLClearDepth expects 1 numeric argument.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLClearDepth")) return makeVoid();

    double depth;
    if (!valueToDouble(args[0], &depth)) {
        runtimeError(vm, "GLClearDepth argument must be numeric.");
        return makeVoid();
    }
    if (depth < 0.0) depth = 0.0;
    if (depth > 1.0) depth = 1.0;

#ifdef GL_ES_VERSION_2_0
    glClearDepthf((GLfloat)depth);
#else
    glClearDepth((GLclampd)depth);
#endif
    return makeVoid();
}

Value vmBuiltinGlviewport(VM* vm, int arg_count, Value* args) {
    if (arg_count != 4) {
        runtimeError(vm, "GLViewport expects 4 integer arguments (x, y, width, height).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLViewport")) return makeVoid();

    for (int i = 0; i < 4; ++i) {
        if (!IS_INTLIKE(args[i])) {
            runtimeError(vm, "GLViewport argument %d must be integer-like.", i + 1);
            return makeVoid();
        }
    }

    glViewport((GLint)AS_INTEGER(args[0]), (GLint)AS_INTEGER(args[1]),
               (GLsizei)AS_INTEGER(args[2]), (GLsizei)AS_INTEGER(args[3]));
    return makeVoid();
}

Value vmBuiltinGlmatrixmode(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "GLMatrixMode expects 1 argument (string or GLenum).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLMatrixMode")) return makeVoid();

    GLenum mode;
    if (!parseMatrixMode(args[0], &mode)) {
        runtimeError(vm, "GLMatrixMode accepts 'modelview', 'projection', 'texture', or an integer GLenum.");
        return makeVoid();
    }

    glMatrixMode(mode);
    return makeVoid();
}

Value vmBuiltinGlloadidentity(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "GLLoadIdentity expects 0 arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLLoadIdentity")) return makeVoid();

    glLoadIdentity();
    return makeVoid();
}

Value vmBuiltinGltranslatef(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3) {
        runtimeError(vm, "GLTranslatef expects 3 numeric arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLTranslatef")) return makeVoid();

    float vals[3];
    for (int i = 0; i < 3; ++i) {
        if (!valueToFloat(args[i], &vals[i])) {
            runtimeError(vm, "GLTranslatef argument %d must be numeric.", i + 1);
            return makeVoid();
        }
    }

    glTranslatef(vals[0], vals[1], vals[2]);
    return makeVoid();
}

Value vmBuiltinGlrotatef(VM* vm, int arg_count, Value* args) {
    if (arg_count != 4) {
        runtimeError(vm, "GLRotatef expects 4 numeric arguments (angle, x, y, z).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLRotatef")) return makeVoid();

    float vals[4];
    for (int i = 0; i < 4; ++i) {
        if (!valueToFloat(args[i], &vals[i])) {
            runtimeError(vm, "GLRotatef argument %d must be numeric.", i + 1);
            return makeVoid();
        }
    }

    glRotatef(vals[0], vals[1], vals[2], vals[3]);
    return makeVoid();
}

Value vmBuiltinGlscalef(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3) {
        runtimeError(vm, "GLScalef expects 3 numeric arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLScalef")) return makeVoid();

    float vals[3];
    for (int i = 0; i < 3; ++i) {
        if (!valueToFloat(args[i], &vals[i])) {
            runtimeError(vm, "GLScalef argument %d must be numeric.", i + 1);
            return makeVoid();
        }
    }

    glScalef(vals[0], vals[1], vals[2]);
    return makeVoid();
}

Value vmBuiltinGlfrustum(VM* vm, int arg_count, Value* args) {
    if (arg_count != 6) {
        runtimeError(vm, "GLFrustum expects 6 numeric arguments (left, right, bottom, top, near, far).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLFrustum")) return makeVoid();

    double vals[6];
    for (int i = 0; i < 6; ++i) {
        if (!valueToDouble(args[i], &vals[i])) {
            runtimeError(vm, "GLFrustum argument %d must be numeric.", i + 1);
            return makeVoid();
        }
    }

    if (vals[4] <= 0.0 || vals[5] <= 0.0 || vals[4] >= vals[5]) {
        runtimeError(vm, "GLFrustum requires near > 0, far > 0, and far > near.");
        return makeVoid();
    }

#ifdef GL_ES_VERSION_2_0
    glFrustumf((GLfloat)vals[0], (GLfloat)vals[1], (GLfloat)vals[2], (GLfloat)vals[3], (GLfloat)vals[4], (GLfloat)vals[5]);
#else
    glFrustum(vals[0], vals[1], vals[2], vals[3], vals[4], vals[5]);
#endif
    return makeVoid();
}

Value vmBuiltinGlperspective(VM* vm, int arg_count, Value* args) {
    if (arg_count != 4) {
        runtimeError(vm, "GLPerspective expects 4 numeric arguments (fovY, aspect, near, far).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLPerspective")) return makeVoid();

    double fovY, aspect, nearPlane, farPlane;
    if (!valueToDouble(args[0], &fovY) || !valueToDouble(args[1], &aspect) ||
        !valueToDouble(args[2], &nearPlane) || !valueToDouble(args[3], &farPlane)) {
        runtimeError(vm, "GLPerspective arguments must be numeric.");
        return makeVoid();
    }

    if (aspect == 0.0) {
        runtimeError(vm, "GLPerspective aspect ratio cannot be zero.");
        return makeVoid();
    }
    if (nearPlane <= 0.0 || farPlane <= 0.0 || nearPlane >= farPlane) {
        runtimeError(vm, "GLPerspective requires near > 0, far > 0, and far > near.");
        return makeVoid();
    }

    if (fovY <= 0.0 || fovY >= 180.0) {
        runtimeError(vm, "GLPerspective fovY must be between 0 and 180 degrees.");
        return makeVoid();
    }

    double f = tan((fovY * 0.5) * (M_PI / 180.0));
    double top = nearPlane * f;
    double bottom = -top;
    double right = top * aspect;
    double left = -right;

#ifdef GL_ES_VERSION_2_0
    glFrustumf((GLfloat)left, (GLfloat)right, (GLfloat)bottom, (GLfloat)top, (GLfloat)nearPlane, (GLfloat)farPlane);
#else
    glFrustum(left, right, bottom, top, nearPlane, farPlane);
#endif
    return makeVoid();
}

Value vmBuiltinGlpushmatrix(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "GLPushMatrix expects 0 arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLPushMatrix")) return makeVoid();

    glPushMatrix();
    return makeVoid();
}

Value vmBuiltinGlpopmatrix(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "GLPopMatrix expects 0 arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLPopMatrix")) return makeVoid();

    glPopMatrix();
    return makeVoid();
}

Value vmBuiltinGlbegin(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "GLBegin expects 1 argument (string or GLenum).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLBegin")) return makeVoid();

    GLenum primitive;
    if (!parsePrimitive(args[0], &primitive)) {
        runtimeError(vm, "GLBegin accepts primitive names like 'triangles', 'quads', 'lines', or an integer GLenum.");
        return makeVoid();
    }

    glBegin(primitive);
    return makeVoid();
}

Value vmBuiltinGlend(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "GLEnd expects 0 arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLEnd")) return makeVoid();

    glEnd();
    return makeVoid();
}

Value vmBuiltinGlcolor3f(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3) {
        runtimeError(vm, "GLColor3f expects 3 numeric arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLColor3f")) return makeVoid();

    float vals[3];
    for (int i = 0; i < 3; ++i) {
        if (!valueToFloat(args[i], &vals[i])) {
            runtimeError(vm, "GLColor3f argument %d must be numeric.", i + 1);
            return makeVoid();
        }
        if (vals[i] < 0.0f) vals[i] = 0.0f;
        if (vals[i] > 1.0f) vals[i] = 1.0f;
    }

    glColor3f(vals[0], vals[1], vals[2]);
    return makeVoid();
}

Value vmBuiltinGlcolor4f(VM* vm, int arg_count, Value* args) {
    if (arg_count != 4) {
        runtimeError(vm, "GLColor4f expects 4 numeric arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLColor4f")) return makeVoid();

    float vals[4];
    for (int i = 0; i < 4; ++i) {
        if (!valueToFloat(args[i], &vals[i])) {
            runtimeError(vm, "GLColor4f argument %d must be numeric.", i + 1);
            return makeVoid();
        }
        if (vals[i] < 0.0f) vals[i] = 0.0f;
        if (vals[i] > 1.0f) vals[i] = 1.0f;
    }

    glColor4f(vals[0], vals[1], vals[2], vals[3]);
    return makeVoid();
}

Value vmBuiltinGlvertex3f(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3) {
        runtimeError(vm, "GLVertex3f expects 3 numeric arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLVertex3f")) return makeVoid();

    float vals[3];
    for (int i = 0; i < 3; ++i) {
        if (!valueToFloat(args[i], &vals[i])) {
            runtimeError(vm, "GLVertex3f argument %d must be numeric.", i + 1);
            return makeVoid();
        }
    }

    glVertex3f(vals[0], vals[1], vals[2]);
    return makeVoid();
}

Value vmBuiltinGlnormal3f(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3) {
        runtimeError(vm, "GLNormal3f expects 3 numeric arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLNormal3f")) return makeVoid();

    float vals[3];
    for (int i = 0; i < 3; ++i) {
        if (!valueToFloat(args[i], &vals[i])) {
            runtimeError(vm, "GLNormal3f argument %d must be numeric.", i + 1);
            return makeVoid();
        }
    }

    glNormal3f(vals[0], vals[1], vals[2]);
    return makeVoid();
}

Value vmBuiltinGlenable(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "GLEnable expects 1 argument (GL capability).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLEnable")) return makeVoid();

    GLenum cap;
    if (!parseCapability(args[0], &cap)) {
        runtimeError(vm, "GLEnable argument must be a known capability name or GLenum value.");
        return makeVoid();
    }

    glEnable(cap);
    return makeVoid();
}

Value vmBuiltinGldisable(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "GLDisable expects 1 argument (GL capability).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLDisable")) return makeVoid();

    GLenum cap;
    if (!parseCapability(args[0], &cap)) {
        runtimeError(vm, "GLDisable argument must be a known capability name or GLenum value.");
        return makeVoid();
    }

    glDisable(cap);
    return makeVoid();
}

Value vmBuiltinGlshademodel(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "GLShadeModel expects 1 argument (string or GLenum).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLShadeModel")) return makeVoid();

    GLenum mode;
    if (!parseShadeModel(args[0], &mode)) {
        runtimeError(vm, "GLShadeModel argument must be 'flat', 'smooth', or a GLenum value.");
        return makeVoid();
    }

    glShadeModel(mode);
    return makeVoid();
}

Value vmBuiltinGllightfv(VM* vm, int arg_count, Value* args) {
    if (arg_count != 6) {
        runtimeError(vm, "GLLightfv expects 6 arguments (light, pname, x, y, z, w).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLLightfv")) return makeVoid();

    GLenum light;
    if (!parseLight(args[0], &light)) {
        runtimeError(vm, "GLLightfv light must be 'light0'..'light7' or a GLenum value.");
        return makeVoid();
    }

    GLenum pname;
    if (!parseLightParam(args[1], &pname)) {
        runtimeError(vm, "GLLightfv pname must be 'position', 'ambient', 'diffuse', 'specular', or a GLenum value.");
        return makeVoid();
    }

    float values[4];
    for (int i = 0; i < 4; ++i) {
        if (!valueToFloat(args[2 + i], &values[i])) {
            runtimeError(vm, "GLLightfv component %d must be numeric.", i + 1);
            return makeVoid();
        }
    }

    glLightfv(light, pname, values);
    return makeVoid();
}

Value vmBuiltinGlmaterialfv(VM* vm, int arg_count, Value* args) {
    if (arg_count != 6) {
        runtimeError(vm, "GLMaterialfv expects 6 arguments (face, pname, r, g, b, a).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLMaterialfv")) return makeVoid();

    GLenum face;
    if (!parseMaterialFace(args[0], &face)) {
        runtimeError(vm, "GLMaterialfv face must be 'front', 'back', 'front_and_back', or a GLenum value.");
        return makeVoid();
    }

    GLenum pname;
    if (!parseMaterialParam(args[1], &pname)) {
        runtimeError(vm, "GLMaterialfv pname must be 'ambient', 'diffuse', 'specular', 'emission', 'ambient_and_diffuse', or a GLenum value.");
        return makeVoid();
    }

    float values[4];
    for (int i = 0; i < 4; ++i) {
        if (!valueToFloat(args[2 + i], &values[i])) {
            runtimeError(vm, "GLMaterialfv component %d must be numeric.", i + 1);
            return makeVoid();
        }
    }

    glMaterialfv(face, pname, values);
    return makeVoid();
}

Value vmBuiltinGlmaterialf(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3) {
        runtimeError(vm, "GLMaterialf expects 3 arguments (face, pname, value).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLMaterialf")) return makeVoid();

    GLenum face;
    if (!parseMaterialFace(args[0], &face)) {
        runtimeError(vm, "GLMaterialf face must be 'front', 'back', 'front_and_back', or a GLenum value.");
        return makeVoid();
    }

    GLenum pname;
    if (!parseMaterialParam(args[1], &pname)) {
        runtimeError(vm, "GLMaterialf pname must be 'shininess' or a GLenum value.");
        return makeVoid();
    }
    if (pname != GL_SHININESS) {
        runtimeError(vm, "GLMaterialf currently supports only the 'shininess' parameter.");
        return makeVoid();
    }

    float value;
    if (!valueToFloat(args[2], &value)) {
        runtimeError(vm, "GLMaterialf value must be numeric.");
        return makeVoid();
    }

    glMaterialf(face, pname, value);
    return makeVoid();
}

Value vmBuiltinGlcolormaterial(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "GLColorMaterial expects 2 arguments (face, mode).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLColorMaterial")) return makeVoid();

    GLenum face;
    if (!parseMaterialFace(args[0], &face)) {
        runtimeError(vm, "GLColorMaterial face must be 'front', 'back', 'front_and_back', or a GLenum value.");
        return makeVoid();
    }

    GLenum mode;
    if (!parseColorMaterialMode(args[1], &mode)) {
        runtimeError(vm, "GLColorMaterial mode must be 'ambient', 'diffuse', 'ambient_and_diffuse', 'specular', 'emission', or a GLenum value.");
        return makeVoid();
    }

    glColorMaterial(face, mode);
    return makeVoid();
}

Value vmBuiltinGlblendfunc(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "GLBlendFunc expects 2 arguments (sfactor, dfactor).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLBlendFunc")) return makeVoid();

    GLenum sfactor;
    if (!parseBlendFactor(args[0], &sfactor)) {
        runtimeError(vm, "GLBlendFunc sfactor must be a known blend factor name or GLenum value.");
        return makeVoid();
    }

    GLenum dfactor;
    if (!parseBlendFactor(args[1], &dfactor)) {
        runtimeError(vm, "GLBlendFunc dfactor must be a known blend factor name or GLenum value.");
        return makeVoid();
    }

    glBlendFunc(sfactor, dfactor);
    return makeVoid();
}

Value vmBuiltinGldepthtest(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "GLDepthTest expects 1 boolean or integer argument.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLDepthTest")) return makeVoid();

    bool enable;
    if (args[0].type == TYPE_BOOLEAN) {
        enable = AS_BOOLEAN(args[0]);
    } else if (IS_INTLIKE(args[0])) {
        enable = AS_INTEGER(args[0]) != 0;
    } else if (isRealType(args[0].type)) {
        enable = AS_REAL(args[0]) != 0.0;
    } else {
        runtimeError(vm, "GLDepthTest argument must be boolean or numeric.");
        return makeVoid();
    }

    if (enable) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    return makeVoid();
}

#endif // SDL
