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
