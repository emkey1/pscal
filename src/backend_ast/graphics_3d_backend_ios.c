#include "backend_ast/graphics_3d_backend.h"

#if defined(PSCAL_TARGET_IOS) || defined(__APPLE__)

#if defined(PSCAL_TARGET_IOS)
#ifndef GLES_SILENCE_DEPRECATION
#define GLES_SILENCE_DEPRECATION
#endif
#include <OpenGLES/ES1/gl.h>
#include <OpenGLES/ES1/glext.h>
#else
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#include <OpenGL/gl.h>
#endif
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "backend_ast/pscal_sdl_runtime.h"
#include "core/sdl_headers.h"

typedef struct {
    float m[16];
} Mat4;

typedef struct {
    float position[3];
    float color[4];
    float screen[2];
    float depth;
    bool valid;
    float normal[3];
    float eyePos[3];
} ImmediateVertex;
static bool transformVertexData(ImmediateVertex* v);

typedef struct {
    float position[3];
    float color[4];
    float normal[3];
} RecordedVertex;

typedef struct {
    GLenum primitive;
    size_t firstVertex;
    size_t vertexCount;
} RecordedCommand;

typedef struct {
    unsigned int id;
    RecordedVertex* vertices;
    size_t vertexCount;
    size_t vertexCapacity;
    RecordedCommand* commands;
    size_t commandCount;
    size_t commandCapacity;
} DisplayList;

static DisplayList* gDisplayLists = NULL;
static size_t gDisplayListCount = 0;

static bool gRecordingList = false;
static unsigned int gRecordingListId = 0;
static RecordedVertex* gRecordingVertices = NULL;
static size_t gRecordingVertexCount = 0;
static size_t gRecordingVertexCapacity = 0;
static RecordedCommand* gRecordingCommands = NULL;
static size_t gRecordingCommandCount = 0;
static size_t gRecordingCommandCapacity = 0;
static RecordedCommand* gRecordingCurrentCommand = NULL;

#ifndef GL_QUADS
#define GL_QUADS 0x0007
#endif

#ifndef GL_LINE_LOOP
#define GL_LINE_LOOP 0x0002
#endif

#ifndef GL_LINE_STRIP
#define GL_LINE_STRIP 0x0003
#endif

#ifndef GL_TRIANGLE_FAN
#define GL_TRIANGLE_FAN 0x0006
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

#if !defined(PSCAL_TARGET_IOS)
static inline void pscalGlClearDepthf(GLfloat depth) {
    glClearDepth((GLdouble)depth);
}
static inline void pscalGlFrustumf(GLfloat left, GLfloat right, GLfloat bottom,
                                   GLfloat top, GLfloat zNear, GLfloat zFar) {
    glFrustum((GLdouble)left, (GLdouble)right, (GLdouble)bottom,
              (GLdouble)top, (GLdouble)zNear, (GLdouble)zFar);
}
static inline void pscalGlOrthof(GLfloat left, GLfloat right, GLfloat bottom,
                                 GLfloat top, GLfloat zNear, GLfloat zFar) {
    glOrtho((GLdouble)left, (GLdouble)right, (GLdouble)bottom,
            (GLdouble)top, (GLdouble)zNear, (GLdouble)zFar);
}
#define glClearDepthf pscalGlClearDepthf
#define glFrustumf pscalGlFrustumf
#define glOrthof pscalGlOrthof
#endif

static Mat4 gProjectionStack[16];
static int gProjectionTop = 0;
static Mat4 gModelviewStack[32];
static int gModelviewTop = 0;
static GLenum gMatrixMode = GL_MODELVIEW;
static bool gStacksInitialized = false;

static float gClearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
static float gCurrentColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
static float gClearDepthValue = 1.0f;
static int gViewport[4] = {0, 0, 640, 480};

static ImmediateVertex* gImmediateVertices = NULL;
static size_t gImmediateCount = 0;
static size_t gImmediateCapacity = 0;
static ImmediateVertex* gImmediateQuadVertices = NULL;
static size_t gImmediateQuadCapacity = 0;
static GLenum gImmediatePrimitive = GL_TRIANGLES;
static bool gImmediateRecording = false;

static bool gBlendEnabled = false;
static GLenum gBlendSrc = GL_ONE;
static GLenum gBlendDst = GL_ZERO;

static Uint32* gColorBuffer = NULL;
static float* gDepthBuffer = NULL;
static int gFramebufferWidth = 0;
static int gFramebufferHeight = 0;
static SDL_Texture* gFramebufferTexture = NULL;
static GLuint gFramebufferGlTexture = 0;
static int gFramebufferGlTextureWidth = 0;
static int gFramebufferGlTextureHeight = 0;
static bool gFramebufferDirty = false;

typedef struct {
    bool enabled;
    float ambient[4];
    float diffuse[4];
    float specular[4];
    float position[4];
} GfxLight;

static GfxLight gLights[8];
static bool gLightingEnabled = false;
static bool gNormalizeEnabled = false;
static bool gLightingStateInitialised = false;

static float gSceneAmbient[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
static float gMaterialAmbient[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
static float gMaterialDiffuse[4] = { 0.8f, 0.8f, 0.8f, 1.0f };
static float gMaterialSpecular[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
static float gMaterialEmission[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
static float gMaterialShininess = 32.0f;
static bool gColorMaterialEnabled = false;
static GLenum gColorMaterialFace = GL_FRONT;
static GLenum gColorMaterialMode = GL_AMBIENT_AND_DIFFUSE;
static float gCurrentNormal[3] = { 0.0f, 0.0f, 1.0f };

static inline Mat4 matIdentity(void) {
    Mat4 m = { .m = {
        1.f, 0.f, 0.f, 0.f,
        0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        0.f, 0.f, 0.f, 1.f,
    }};
    return m;
}

static void initLightingState(void) {
    if (gLightingStateInitialised) {
        return;
    }
    for (int i = 0; i < 8; ++i) {
        gLights[i].enabled = (i == 0);
        gLights[i].ambient[0] = gLights[i].ambient[1] = gLights[i].ambient[2] = 0.0f;
        gLights[i].ambient[3] = 1.0f;
        gLights[i].diffuse[0] = gLights[i].diffuse[1] = gLights[i].diffuse[2] = (i == 0) ? 1.0f : 0.0f;
        gLights[i].diffuse[3] = 1.0f;
        gLights[i].specular[0] = gLights[i].specular[1] = gLights[i].specular[2] = (i == 0) ? 1.0f : 0.0f;
        gLights[i].specular[3] = 1.0f;
        gLights[i].position[0] = 0.0f;
        gLights[i].position[1] = 0.0f;
        gLights[i].position[2] = -1.0f;
        gLights[i].position[3] = 0.0f;
    }
    gLightingStateInitialised = true;
}

static void ensureStacks(void) {
    if (!gStacksInitialized) {
        gModelviewStack[0] = matIdentity();
        gProjectionStack[0] = matIdentity();
        gMatrixMode = GL_MODELVIEW;
        gStacksInitialized = true;
    }
}

static Mat4* currentMatrix(void) {
    ensureStacks();
    if (gMatrixMode == GL_PROJECTION) {
        return &gProjectionStack[gProjectionTop];
    }
    return &gModelviewStack[gModelviewTop];
}

static void matrixMultiply(Mat4* lhs, const Mat4* rhs) {
    Mat4 result;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            result.m[col * 4 + row] =
                lhs->m[0 * 4 + row] * rhs->m[col * 4 + 0] +
                lhs->m[1 * 4 + row] * rhs->m[col * 4 + 1] +
                lhs->m[2 * 4 + row] * rhs->m[col * 4 + 2] +
                lhs->m[3 * 4 + row] * rhs->m[col * 4 + 3];
        }
    }
    *lhs = result;
}

static void matrixTranslate(Mat4* accum, float x, float y, float z) {
    Mat4 t = matIdentity();
    t.m[12] = x;
    t.m[13] = y;
    t.m[14] = z;
    matrixMultiply(accum, &t);
}

static void matrixScale(Mat4* accum, float x, float y, float z) {
    Mat4 s = matIdentity();
    s.m[0] = x;
    s.m[5] = y;
    s.m[10] = z;
    matrixMultiply(accum, &s);
}

static void matrixRotate(Mat4* accum, float angleDeg, float x, float y, float z) {
    float radians = angleDeg * (float)M_PI / 180.0f;
    float c = cosf(radians);
    float s = sinf(radians);
    float mag = sqrtf(x * x + y * y + z * z);
    if (mag < 1e-6f) {
        return;
    }
    x /= mag;
    y /= mag;
    z /= mag;
    Mat4 r = matIdentity();
    r.m[0] = x * x * (1 - c) + c;
    r.m[1] = x * y * (1 - c) + z * s;
    r.m[2] = x * z * (1 - c) - y * s;
    r.m[4] = y * x * (1 - c) - z * s;
    r.m[5] = y * y * (1 - c) + c;
    r.m[6] = y * z * (1 - c) + x * s;
    r.m[8] = z * x * (1 - c) + y * s;
    r.m[9] = z * y * (1 - c) - x * s;
    r.m[10] = z * z * (1 - c) + c;
    matrixMultiply(accum, &r);
}

static void matrixFrustum(Mat4* accum, double left, double right,
                          double bottom, double top,
                          double zNear, double zFar) {
    Mat4 f = { .m = {
        (float)((2.0 * zNear) / (right - left)), 0, 0, 0,
        0, (float)((2.0 * zNear) / (top - bottom)), 0, 0,
        (float)((right + left) / (right - left)),
        (float)((top + bottom) / (top - bottom)),
        (float)(-(zFar + zNear) / (zFar - zNear)), -1.0f,
        0, 0,
        (float)(-(2.0 * zFar * zNear) / (zFar - zNear)), 0
    }};
    matrixMultiply(accum, &f);
}

static void applyMatrix(const Mat4* m, const float in[4], float out[4]) {
    for (int row = 0; row < 4; ++row) {
        out[row] =
            m->m[0 * 4 + row] * in[0] +
            m->m[1 * 4 + row] * in[1] +
            m->m[2 * 4 + row] * in[2] +
            m->m[3 * 4 + row] * in[3];
    }
}

static void ensureImmediateCapacity(size_t count) {
    if (count <= gImmediateCapacity) {
        return;
    }
    size_t newCap = gImmediateCapacity == 0 ? 64 : gImmediateCapacity;
    while (newCap < count) {
        newCap *= 2;
    }
    ImmediateVertex* resized = realloc(gImmediateVertices, newCap * sizeof(ImmediateVertex));
    if (!resized) {
        return;
    }
    gImmediateVertices = resized;
    gImmediateCapacity = newCap;
}

static bool usingNativeGlPath(void) {
    return gSdlGLContext != NULL;
}

static bool ensureImmediateQuadCapacity(size_t count) {
    if (count <= gImmediateQuadCapacity) {
        return true;
    }
    size_t newCap = gImmediateQuadCapacity == 0 ? 128 : gImmediateQuadCapacity;
    while (newCap < count) {
        newCap *= 2;
    }
    ImmediateVertex* resized = realloc(gImmediateQuadVertices, newCap * sizeof(ImmediateVertex));
    if (!resized) {
        return false;
    }
    gImmediateQuadVertices = resized;
    gImmediateQuadCapacity = newCap;
    return true;
}

static Uint8 floatToByte(float c) {
    if (c < 0.0f) c = 0.0f;
    if (c > 1.0f) c = 1.0f;
    return (Uint8)lrintf(c * 255.0f);
}

static Uint32 packColor(const float color[4]) {
    Uint8 r = floatToByte(color[0]);
    Uint8 g = floatToByte(color[1]);
    Uint8 b = floatToByte(color[2]);
    Uint8 a = floatToByte(color[3]);
    return ((Uint32)a << 24) | ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
}

static void clampColor(float* c) {
    if (*c < 0.0f) *c = 0.0f;
    if (*c > 1.0f) *c = 1.0f;
}

static void applyColorMaterial(const float color[4]) {
    if (!gColorMaterialEnabled) {
        return;
    }
    if (gColorMaterialMode == GL_AMBIENT || gColorMaterialMode == GL_AMBIENT_AND_DIFFUSE) {
        memcpy(gMaterialAmbient, color, sizeof(float) * 4);
    }
    if (gColorMaterialMode == GL_DIFFUSE || gColorMaterialMode == GL_AMBIENT_AND_DIFFUSE) {
        memcpy(gMaterialDiffuse, color, sizeof(float) * 4);
    }
    if (gColorMaterialMode == GL_SPECULAR) {
        memcpy(gMaterialSpecular, color, sizeof(float) * 4);
    }
    if (gColorMaterialMode == GL_EMISSION) {
        memcpy(gMaterialEmission, color, sizeof(float) * 4);
    }
}

static void shadeVertex(ImmediateVertex* v) {
    if (!gLightingEnabled) {
        return;
    }
    initLightingState();
    float normal[3] = { v->normal[0], v->normal[1], v->normal[2] };
    float len = sqrtf(normal[0]*normal[0] + normal[1]*normal[1] + normal[2]*normal[2]);
    if (len > 1e-6f) {
        normal[0] /= len;
        normal[1] /= len;
        normal[2] /= len;
    } else {
        normal[0] = 0.0f; normal[1] = 0.0f; normal[2] = 1.0f;
    }
    if (gNormalizeEnabled) {
        float l = sqrtf(normal[0]*normal[0] + normal[1]*normal[1] + normal[2]*normal[2]);
        if (l > 1e-6f) {
            normal[0] /= l;
            normal[1] /= l;
            normal[2] /= l;
        }
    }

    float result[4];
    for (int i = 0; i < 4; ++i) {
        result[i] = gMaterialEmission[i] + gMaterialAmbient[i] * gSceneAmbient[i];
    }

    for (int li = 0; li < 8; ++li) {
        if (!gLights[li].enabled) {
            continue;
        }
        float lightDir[3];
        if (fabsf(gLights[li].position[3]) < 1e-6f) {
            lightDir[0] = -gLights[li].position[0];
            lightDir[1] = -gLights[li].position[1];
            lightDir[2] = -gLights[li].position[2];
        } else {
            lightDir[0] = gLights[li].position[0] - v->eyePos[0];
            lightDir[1] = gLights[li].position[1] - v->eyePos[1];
            lightDir[2] = gLights[li].position[2] - v->eyePos[2];
        }
        float lLen = sqrtf(lightDir[0]*lightDir[0] + lightDir[1]*lightDir[1] + lightDir[2]*lightDir[2]);
        if (lLen < 1e-6f) {
            continue;
        }
        lightDir[0] /= lLen;
        lightDir[1] /= lLen;
        lightDir[2] /= lLen;

        float ndotl = normal[0]*lightDir[0] + normal[1]*lightDir[1] + normal[2]*lightDir[2];
        float diffuseFactor = fmaxf(ndotl, 0.0f);

        float halfVec[3] = { lightDir[0], lightDir[1], lightDir[2] };
        halfVec[2] += 1.0f; // viewer direction approx (0,0,1)
        float hLen = sqrtf(halfVec[0]*halfVec[0] + halfVec[1]*halfVec[1] + halfVec[2]*halfVec[2]);
        if (hLen > 1e-6f) {
            halfVec[0] /= hLen;
            halfVec[1] /= hLen;
            halfVec[2] /= hLen;
        }
        float ndoth = fmaxf(normal[0]*halfVec[0] + normal[1]*halfVec[1] + normal[2]*halfVec[2], 0.0f);
        float specFactor = (diffuseFactor > 0.0f && gMaterialShininess > 0.0f)
                               ? powf(ndoth, gMaterialShininess)
                               : 0.0f;

        for (int c = 0; c < 4; ++c) {
            result[c] += gMaterialAmbient[c] * gLights[li].ambient[c];
            result[c] += gMaterialDiffuse[c] * gLights[li].diffuse[c] * diffuseFactor;
            result[c] += gMaterialSpecular[c] * gLights[li].specular[c] * specFactor;
        }
    }
    for (int i = 0; i < 4; ++i) {
        clampColor(&result[i]);
        v->color[i] = result[i];
    }
}

static void emitImmediateVertex(const float position[3],
                                const float normal[3],
                                const float color[4]) {
    ensureImmediateCapacity(gImmediateCount + 1);
    ImmediateVertex* v = &gImmediateVertices[gImmediateCount++];
    memcpy(v->position, position, sizeof(float) * 3);
    memcpy(v->normal, normal, sizeof(float) * 3);
    memcpy(v->color, color, sizeof(float) * 4);
    if (usingNativeGlPath()) {
        v->valid = true;
        return;
    }
    v->valid = transformVertexData(v);
    shadeVertex(v);
}

static void ensureRecordingVertexCapacity(size_t count) {
    if (count <= gRecordingVertexCapacity) {
        return;
    }
    size_t newCap = gRecordingVertexCapacity == 0 ? 64 : gRecordingVertexCapacity;
    while (newCap < count) {
        newCap *= 2;
    }
    RecordedVertex* resized = realloc(gRecordingVertices, newCap * sizeof(RecordedVertex));
    if (!resized) {
        return;
    }
    gRecordingVertices = resized;
    gRecordingVertexCapacity = newCap;
}

static void ensureRecordingCommandCapacity(size_t count) {
    if (count <= gRecordingCommandCapacity) {
        return;
    }
    size_t newCap = gRecordingCommandCapacity == 0 ? 8 : gRecordingCommandCapacity;
    while (newCap < count) {
        newCap *= 2;
    }
    RecordedCommand* resized = realloc(gRecordingCommands, newCap * sizeof(RecordedCommand));
    if (!resized) {
        return;
    }
    gRecordingCommands = resized;
    gRecordingCommandCapacity = newCap;
}

static void resetRecordingBuffers(void) {
    free(gRecordingVertices);
    free(gRecordingCommands);
    gRecordingVertices = NULL;
    gRecordingCommands = NULL;
    gRecordingVertexCount = 0;
    gRecordingVertexCapacity = 0;
    gRecordingCommandCount = 0;
    gRecordingCommandCapacity = 0;
    gRecordingCurrentCommand = NULL;
}

static DisplayList* findDisplayList(unsigned int id) {
    for (size_t i = 0; i < gDisplayListCount; ++i) {
        if (gDisplayLists[i].id == id) {
            return &gDisplayLists[i];
        }
    }
    return NULL;
}

static DisplayList* getOrCreateDisplayList(unsigned int id) {
    DisplayList* existing = findDisplayList(id);
    if (existing) {
        return existing;
    }
    DisplayList* resized = realloc(gDisplayLists, (gDisplayListCount + 1) * sizeof(DisplayList));
    if (!resized) {
        return NULL;
    }
    gDisplayLists = resized;
    DisplayList* list = &gDisplayLists[gDisplayListCount++];
    memset(list, 0, sizeof(DisplayList));
    list->id = id;
    return list;
}

static void freeDisplayList(DisplayList* list) {
    if (!list) {
        return;
    }
    free(list->vertices);
    free(list->commands);
    list->vertices = NULL;
    list->commands = NULL;
    list->vertexCount = list->vertexCapacity = 0;
    list->commandCount = list->commandCapacity = 0;
}

static bool ensureFramebuffer(void) {
    int width = gViewport[2];
    int height = gViewport[3];
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (width != gFramebufferWidth || height != gFramebufferHeight || !gColorBuffer || !gDepthBuffer) {
        free(gColorBuffer);
        free(gDepthBuffer);
        if (gFramebufferTexture) {
            SDL_DestroyTexture(gFramebufferTexture);
            gFramebufferTexture = NULL;
        }
        if (gFramebufferGlTexture != 0) {
            if (gSdlGLContext) {
                glDeleteTextures(1, &gFramebufferGlTexture);
            }
            gFramebufferGlTexture = 0;
            gFramebufferGlTextureWidth = 0;
            gFramebufferGlTextureHeight = 0;
        }
        gColorBuffer = calloc((size_t)width * (size_t)height, sizeof(Uint32));
        gDepthBuffer = malloc((size_t)width * (size_t)height * sizeof(float));
        if (!gColorBuffer || !gDepthBuffer) {
            free(gColorBuffer);
            free(gDepthBuffer);
            gColorBuffer = NULL;
            gDepthBuffer = NULL;
            gFramebufferWidth = gFramebufferHeight = 0;
            return false;
        }
        gFramebufferWidth = width;
        gFramebufferHeight = height;
        for (size_t i = 0; i < (size_t)width * (size_t)height; ++i) {
            gDepthBuffer[i] = gClearDepthValue;
        }
    }
    if (!gFramebufferTexture && gSdlRenderer) {
        gFramebufferTexture = SDL_CreateTexture(gSdlRenderer,
                                                SDL_PIXELFORMAT_ABGR8888,
                                                SDL_TEXTUREACCESS_STREAMING,
                                                gFramebufferWidth,
                                                gFramebufferHeight);
    }
    return gColorBuffer && gDepthBuffer;
}

static inline void drawPixel(int x, int y, float depth, const float color[4]) {
    if (x < 0 || y < 0 || x >= gFramebufferWidth || y >= gFramebufferHeight) {
        return;
    }
    size_t idx = (size_t)y * (size_t)gFramebufferWidth + (size_t)x;
    if (!gDepthBuffer || !gColorBuffer) {
        return;
    }
    if (depth > gClearDepthValue) {
        depth = gClearDepthValue;
    }
    if (depth < 0.0f) depth = 0.0f;
    if (depth > gDepthBuffer[idx]) {
        return;
    }
    gDepthBuffer[idx] = depth;
    gColorBuffer[idx] = packColor(color);
    gFramebufferDirty = true;
}

static void presentFramebuffer(void) {
    if (!gFramebufferDirty || !gFramebufferTexture || !gSdlRenderer || !gColorBuffer) {
        return;
    }
    SDL_UpdateTexture(gFramebufferTexture, NULL, gColorBuffer, gFramebufferWidth * (int)sizeof(Uint32));
    SDL_RenderCopy(gSdlRenderer, gFramebufferTexture, NULL, NULL);
    gFramebufferDirty = false;
}

static bool ensureFramebufferGlTexture(void) {
    if (!gSdlGLContext || !gColorBuffer || gFramebufferWidth <= 0 || gFramebufferHeight <= 0) {
        return false;
    }

    if (gFramebufferGlTexture == 0) {
        glGenTextures(1, &gFramebufferGlTexture);
        if (gFramebufferGlTexture == 0) {
            return false;
        }
        glBindTexture(GL_TEXTURE_2D, gFramebufferGlTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     gFramebufferWidth, gFramebufferHeight, 0,
                     GL_BGRA_EXT, GL_UNSIGNED_BYTE, gColorBuffer);
        gFramebufferGlTextureWidth = gFramebufferWidth;
        gFramebufferGlTextureHeight = gFramebufferHeight;
        return true;
    }

    glBindTexture(GL_TEXTURE_2D, gFramebufferGlTexture);
    if (gFramebufferGlTextureWidth != gFramebufferWidth ||
        gFramebufferGlTextureHeight != gFramebufferHeight) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     gFramebufferWidth, gFramebufferHeight, 0,
                     GL_BGRA_EXT, GL_UNSIGNED_BYTE, gColorBuffer);
        gFramebufferGlTextureWidth = gFramebufferWidth;
        gFramebufferGlTextureHeight = gFramebufferHeight;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        gFramebufferWidth, gFramebufferHeight,
                        GL_BGRA_EXT, GL_UNSIGNED_BYTE, gColorBuffer);
    }
    return true;
}

static bool transformVertexData(ImmediateVertex* v) {
    float pos[4] = { v->position[0], v->position[1], v->position[2], 1.0f };
    float mv[4];
    float clip[4];
    applyMatrix(&gModelviewStack[gModelviewTop], pos, mv);
    float invWmv = fabsf(mv[3]) > 1e-6f ? (1.0f / mv[3]) : 1.0f;
    v->eyePos[0] = mv[0] * invWmv;
    v->eyePos[1] = mv[1] * invWmv;
    v->eyePos[2] = mv[2] * invWmv;
    applyMatrix(&gProjectionStack[gProjectionTop], mv, clip);
    if (fabsf(clip[3]) < 1e-6f) {
        return false;
    }
    float invW = 1.0f / clip[3];
    float ndcX = clip[0] * invW;
    float ndcY = clip[1] * invW;
    float ndcZ = clip[2] * invW;
    v->screen[0] = (ndcX * 0.5f + 0.5f) * gViewport[2] + gViewport[0];
    v->screen[1] = (-ndcY * 0.5f + 0.5f) * gViewport[3] + gViewport[1];
    v->depth = ndcZ * 0.5f + 0.5f;
    const Mat4* mvMat = &gModelviewStack[gModelviewTop];
    float nx = v->normal[0];
    float ny = v->normal[1];
    float nz = v->normal[2];
    v->normal[0] = mvMat->m[0] * nx + mvMat->m[4] * ny + mvMat->m[8]  * nz;
    v->normal[1] = mvMat->m[1] * nx + mvMat->m[5] * ny + mvMat->m[9]  * nz;
    v->normal[2] = mvMat->m[2] * nx + mvMat->m[6] * ny + mvMat->m[10] * nz;
    v->valid = true;
    return true;
}

static void drawLineSegment(const ImmediateVertex* a, const ImmediateVertex* b) {
    int x0 = (int)roundf(a->screen[0]);
    int y0 = (int)roundf(a->screen[1]);
    int x1 = (int)roundf(b->screen[0]);
    int y1 = (int)roundf(b->screen[1]);
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int steps = dx > -dy ? dx : -dy;
    for (int i = 0; i <= steps; ++i) {
        float t = steps == 0 ? 0.0f : (float)i / (float)steps;
        float depth = a->depth + t * (b->depth - a->depth);
        float color[4];
        for (int c = 0; c < 4; ++c) {
            color[c] = a->color[c] + t * (b->color[c] - a->color[c]);
        }
        drawPixel(x0, y0, depth, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void renderLines(void) {
    if (gImmediateCount < 2 || !ensureFramebuffer()) {
        return;
    }
    if (gImmediatePrimitive == GL_LINE_LOOP || gImmediatePrimitive == GL_LINE_STRIP) {
        for (size_t i = 1; i < gImmediateCount; ++i) {
            const ImmediateVertex* prev = &gImmediateVertices[i - 1];
            const ImmediateVertex* cur = &gImmediateVertices[i];
            if (!prev->valid || !cur->valid) continue;
            drawLineSegment(prev, cur);
        }
        if (gImmediatePrimitive == GL_LINE_LOOP && gImmediateCount >= 2) {
            const ImmediateVertex* first = &gImmediateVertices[0];
            const ImmediateVertex* last = &gImmediateVertices[gImmediateCount - 1];
            if (first->valid && last->valid) {
                drawLineSegment(last, first);
            }
        }
    } else {
        for (size_t i = 0; i + 1 < gImmediateCount; i += 2) {
            const ImmediateVertex* v0 = &gImmediateVertices[i];
            const ImmediateVertex* v1 = &gImmediateVertices[i + 1];
            if (!v0->valid || !v1->valid) continue;
            drawLineSegment(v0, v1);
        }
    }
}

static void rasterizeTriangle(const ImmediateVertex* a,
                              const ImmediateVertex* b,
                              const ImmediateVertex* c) {
    if (!a->valid || !b->valid || !c->valid) {
        return;
    }
    int minX = (int)floorf(fminf(fminf(a->screen[0], b->screen[0]), c->screen[0]));
    int maxX = (int)ceilf(fmaxf(fmaxf(a->screen[0], b->screen[0]), c->screen[0]));
    int minY = (int)floorf(fminf(fminf(a->screen[1], b->screen[1]), c->screen[1]));
    int maxY = (int)ceilf(fmaxf(fmaxf(a->screen[1], b->screen[1]), c->screen[1]));
    if (maxX < 0 || maxY < 0 || minX >= gFramebufferWidth || minY >= gFramebufferHeight) {
        return;
    }
    minX = fmax(minX, 0);
    minY = fmax(minY, 0);
    maxX = fmin(maxX, gFramebufferWidth - 1);
    maxY = fmin(maxY, gFramebufferHeight - 1);
    float ax = a->screen[0];
    float ay = a->screen[1];
    float bx = b->screen[0];
    float by = b->screen[1];
    float cx = c->screen[0];
    float cy = c->screen[1];
    float denom = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    if (fabsf(denom) < 1e-6f) {
        return;
    }
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;
            float w0 = (by - cy) * (px - cx) + (cx - bx) * (py - cy);
            float w1 = (cy - ay) * (px - cx) + (ax - cx) * (py - cy);
            float w2 = denom - w0 - w1;
            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) ||
                (w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                w0 /= denom;
                w1 /= denom;
                w2 /= denom;
                float depth = w0 * a->depth + w1 * b->depth + w2 * c->depth;
                float color[4];
                for (int i = 0; i < 4; ++i) {
                    color[i] = w0 * a->color[i] + w1 * b->color[i] + w2 * c->color[i];
                }
                drawPixel(x, y, depth, color);
            }
        }
    }
}

static void renderTriangles(void) {
    if (!ensureFramebuffer()) {
        return;
    }
    if (gImmediatePrimitive == GL_TRIANGLES) {
        for (size_t i = 0; i + 2 < gImmediateCount; i += 3) {
            rasterizeTriangle(&gImmediateVertices[i],
                              &gImmediateVertices[i + 1],
                              &gImmediateVertices[i + 2]);
        }
    } else if (gImmediatePrimitive == GL_TRIANGLE_STRIP) {
        for (size_t i = 0; i + 2 < gImmediateCount; ++i) {
            if (i % 2 == 0) {
                rasterizeTriangle(&gImmediateVertices[i],
                                  &gImmediateVertices[i + 1],
                                  &gImmediateVertices[i + 2]);
            } else {
                rasterizeTriangle(&gImmediateVertices[i + 1],
                                  &gImmediateVertices[i],
                                  &gImmediateVertices[i + 2]);
            }
        }
    } else if (gImmediatePrimitive == GL_TRIANGLE_FAN) {
        const ImmediateVertex* center = &gImmediateVertices[0];
        for (size_t i = 1; i + 1 < gImmediateCount; ++i) {
            rasterizeTriangle(center,
                              &gImmediateVertices[i],
                              &gImmediateVertices[i + 1]);
        }
    } else if (gImmediatePrimitive == GL_QUADS) {
        for (size_t i = 0; i + 3 < gImmediateCount; i += 4) {
            rasterizeTriangle(&gImmediateVertices[i],
                              &gImmediateVertices[i + 1],
                              &gImmediateVertices[i + 2]);
            rasterizeTriangle(&gImmediateVertices[i],
                              &gImmediateVertices[i + 2],
                              &gImmediateVertices[i + 3]);
        }
    }
}

static void flushImmediateNative(void) {
    if (gImmediateCount == 0) {
        return;
    }

    GLenum primitive = gImmediatePrimitive;
    ImmediateVertex* vertices = gImmediateVertices;
    size_t vertexCount = gImmediateCount;

    if (primitive == GL_QUADS) {
        size_t quadCount = vertexCount / 4;
        size_t expandedCount = quadCount * 6;
        if (expandedCount == 0) {
            return;
        }
        if (!ensureImmediateQuadCapacity(expandedCount)) {
            return;
        }
        size_t out = 0;
        for (size_t i = 0; i + 3 < vertexCount; i += 4) {
            const ImmediateVertex* v0 = &vertices[i];
            const ImmediateVertex* v1 = &vertices[i + 1];
            const ImmediateVertex* v2 = &vertices[i + 2];
            const ImmediateVertex* v3 = &vertices[i + 3];
            gImmediateQuadVertices[out++] = *v0;
            gImmediateQuadVertices[out++] = *v1;
            gImmediateQuadVertices[out++] = *v2;
            gImmediateQuadVertices[out++] = *v0;
            gImmediateQuadVertices[out++] = *v2;
            gImmediateQuadVertices[out++] = *v3;
        }
        vertices = gImmediateQuadVertices;
        vertexCount = out;
        primitive = GL_TRIANGLES;
    }

    if (vertexCount == 0) {
        return;
    }

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(ImmediateVertex), (const GLvoid*)vertices[0].position);
    glColorPointer(4, GL_FLOAT, sizeof(ImmediateVertex), (const GLvoid*)vertices[0].color);
    glNormalPointer(GL_FLOAT, sizeof(ImmediateVertex), (const GLvoid*)vertices[0].normal);
    glDrawArrays(primitive, 0, (GLsizei)vertexCount);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

static void flushImmediate(void) {
    if (!gImmediateRecording || gImmediateCount == 0) {
        return;
    }
    if (usingNativeGlPath()) {
        flushImmediateNative();
        gImmediateCount = 0;
        gImmediateRecording = false;
        return;
    }
    switch (gImmediatePrimitive) {
        case GL_LINES:
        case GL_LINE_LOOP:
        case GL_LINE_STRIP:
            renderLines();
            break;
        case GL_TRIANGLES:
        case GL_TRIANGLE_STRIP:
        case GL_QUADS:
            renderTriangles();
            break;
        default:
            break;
    }
    gImmediateCount = 0;
    gImmediateRecording = false;
    if (usingNativeGlPath()) {
        presentFramebuffer();
    }
}

void gfx3dClearColor(float r, float g, float b, float a) {
    if (usingNativeGlPath()) {
        glClearColor(r, g, b, a);
    }
    gClearColor[0] = r;
    gClearColor[1] = g;
    gClearColor[2] = b;
    gClearColor[3] = a;
}

void gfx3dClear(unsigned int mask) {
    if (usingNativeGlPath()) {
        glClear((GLbitfield)mask);
        return;
    }
    if (!ensureFramebuffer()) {
        return;
    }
    if (mask & GL_COLOR_BUFFER_BIT) {
        Uint32 packed = packColor(gClearColor);
        for (int y = 0; y < gFramebufferHeight; ++y) {
            for (int x = 0; x < gFramebufferWidth; ++x) {
                gColorBuffer[(size_t)y * (size_t)gFramebufferWidth + (size_t)x] = packed;
            }
        }
    }
    if (mask & GL_DEPTH_BUFFER_BIT) {
        for (int y = 0; y < gFramebufferHeight; ++y) {
            for (int x = 0; x < gFramebufferWidth; ++x) {
                gDepthBuffer[(size_t)y * (size_t)gFramebufferWidth + (size_t)x] = gClearDepthValue;
            }
        }
    }
    gFramebufferDirty = true;
}

void gfx3dClearDepth(double depth) {
    if (usingNativeGlPath()) {
        glClearDepthf((GLfloat)depth);
    }
    gClearDepthValue = (float)depth;
}

void gfx3dViewport(int x, int y, int width, int height) {
    if (usingNativeGlPath()) {
        glViewport((GLint)x, (GLint)y, (GLsizei)width, (GLsizei)height);
    }
    gViewport[0] = x;
    gViewport[1] = y;
    gViewport[2] = width > 0 ? width : 1;
    gViewport[3] = height > 0 ? height : 1;
    ensureFramebuffer();
}

void gfx3dMatrixMode(int mode) {
    if (usingNativeGlPath()) {
        glMatrixMode((GLenum)mode);
    }
    ensureStacks();
    if (mode == GL_PROJECTION) {
        gMatrixMode = GL_PROJECTION;
    } else {
        gMatrixMode = GL_MODELVIEW;
    }
}

void gfx3dLoadIdentity(void) {
    if (usingNativeGlPath()) {
        glLoadIdentity();
    }
    Mat4* m = currentMatrix();
    *m = matIdentity();
}

void gfx3dTranslatef(float x, float y, float z) {
    if (usingNativeGlPath()) {
        glTranslatef(x, y, z);
    }
    matrixTranslate(currentMatrix(), x, y, z);
}

void gfx3dRotatef(float angle, float x, float y, float z) {
    if (usingNativeGlPath()) {
        glRotatef(angle, x, y, z);
    }
    matrixRotate(currentMatrix(), angle, x, y, z);
}

void gfx3dScalef(float x, float y, float z) {
    if (usingNativeGlPath()) {
        glScalef(x, y, z);
    }
    matrixScale(currentMatrix(), x, y, z);
}

void gfx3dFrustum(double left, double right, double bottom, double top,
                  double zNear, double zFar) {
    if (usingNativeGlPath()) {
        glFrustumf((GLfloat)left, (GLfloat)right, (GLfloat)bottom, (GLfloat)top,
                   (GLfloat)zNear, (GLfloat)zFar);
    }
    matrixFrustum(currentMatrix(), left, right, bottom, top, zNear, zFar);
}

void gfx3dPushMatrix(void) {
    if (usingNativeGlPath()) {
        glPushMatrix();
    }
    ensureStacks();
    Mat4* stack = (gMatrixMode == GL_PROJECTION)
                    ? gProjectionStack
                    : gModelviewStack;
    int* top = (gMatrixMode == GL_PROJECTION)
                    ? &gProjectionTop
                    : &gModelviewTop;
    if (*top < (gMatrixMode == GL_PROJECTION ? 15 : 31)) {
        stack[*top + 1] = stack[*top];
        (*top)++;
    }
}

void gfx3dPopMatrix(void) {
    if (usingNativeGlPath()) {
        glPopMatrix();
    }
    ensureStacks();
    int* top = (gMatrixMode == GL_PROJECTION)
                    ? &gProjectionTop
                    : &gModelviewTop;
    if (*top > 0) {
        (*top)--;
    }
}

void gfx3dBegin(unsigned int primitive) {
    gImmediatePrimitive = primitive;
    gImmediateCount = 0;
    gImmediateRecording = true;
    if (gRecordingList) {
        ensureRecordingCommandCapacity(gRecordingCommandCount + 1);
        if (gRecordingCommandCount < gRecordingCommandCapacity) {
            RecordedCommand* cmd = &gRecordingCommands[gRecordingCommandCount++];
            cmd->primitive = primitive;
            cmd->firstVertex = gRecordingVertexCount;
            cmd->vertexCount = 0;
            gRecordingCurrentCommand = cmd;
        }
    }
}

void gfx3dEnd(void) {
    flushImmediate();
    if (gRecordingList && gRecordingCurrentCommand) {
        gRecordingCurrentCommand->vertexCount =
            gRecordingVertexCount - gRecordingCurrentCommand->firstVertex;
        gRecordingCurrentCommand = NULL;
    }
}

void gfx3dColor3f(float r, float g, float b) {
    gCurrentColor[0] = r;
    gCurrentColor[1] = g;
    gCurrentColor[2] = b;
    applyColorMaterial(gCurrentColor);
}

void gfx3dColor4f(float r, float g, float b, float a) {
    gCurrentColor[0] = r;
    gCurrentColor[1] = g;
    gCurrentColor[2] = b;
    gCurrentColor[3] = a;
    applyColorMaterial(gCurrentColor);
}

void gfx3dVertex3f(float x, float y, float z) {
    if (!gImmediateRecording) {
        return;
    }
    float pos[3] = { x, y, z };
    float normal[3] = { gCurrentNormal[0], gCurrentNormal[1], gCurrentNormal[2] };
    emitImmediateVertex(pos, normal, gCurrentColor);
    if (gRecordingList && gRecordingCurrentCommand) {
        ensureRecordingVertexCapacity(gRecordingVertexCount + 1);
        if (gRecordingVertexCount < gRecordingVertexCapacity) {
            RecordedVertex* rv = &gRecordingVertices[gRecordingVertexCount++];
            memcpy(rv->position, pos, sizeof(float) * 3);
            memcpy(rv->normal, normal, sizeof(float) * 3);
            memcpy(rv->color, gCurrentColor, sizeof(float) * 4);
            gRecordingCurrentCommand->vertexCount =
                gRecordingVertexCount - gRecordingCurrentCommand->firstVertex;
        }
    }
}

void gfx3dNormal3f(float x, float y, float z) {
    gCurrentNormal[0] = x;
    gCurrentNormal[1] = y;
    gCurrentNormal[2] = z;
    if (gNormalizeEnabled) {
        float len = sqrtf(x*x + y*y + z*z);
        if (len > 1e-6f) {
            gCurrentNormal[0] /= len;
            gCurrentNormal[1] /= len;
            gCurrentNormal[2] /= len;
        }
    }
}

void gfx3dEnable(unsigned int capability) {
    if (usingNativeGlPath()) {
        glEnable((GLenum)capability);
    }
    if (capability == GL_BLEND) {
        gBlendEnabled = true;
        if (usingNativeGlPath() && gSdlRenderer) {
            SDL_SetRenderDrawBlendMode(gSdlRenderer, SDL_BLENDMODE_BLEND);
        }
    } else if (capability == GL_LIGHTING) {
        gLightingEnabled = true;
        initLightingState();
    } else if (capability >= GL_LIGHT0 && capability <= GL_LIGHT7) {
        initLightingState();
        gLights[capability - GL_LIGHT0].enabled = true;
    } else if (capability == GL_COLOR_MATERIAL) {
        gColorMaterialEnabled = true;
    } else if (capability == GL_NORMALIZE) {
        gNormalizeEnabled = true;
    }
}

void gfx3dDisable(unsigned int capability) {
    if (usingNativeGlPath()) {
        glDisable((GLenum)capability);
    }
    if (capability == GL_BLEND) {
        gBlendEnabled = false;
        if (usingNativeGlPath() && gSdlRenderer) {
            SDL_SetRenderDrawBlendMode(gSdlRenderer, SDL_BLENDMODE_NONE);
        }
    } else if (capability == GL_LIGHTING) {
        gLightingEnabled = false;
    } else if (capability >= GL_LIGHT0 && capability <= GL_LIGHT7) {
        gLights[capability - GL_LIGHT0].enabled = false;
    } else if (capability == GL_COLOR_MATERIAL) {
        gColorMaterialEnabled = false;
    } else if (capability == GL_NORMALIZE) {
        gNormalizeEnabled = false;
    }
}

void gfx3dShadeModel(unsigned int mode) {
    if (usingNativeGlPath()) {
        glShadeModel((GLenum)mode);
    }
}

void gfx3dLightfv(unsigned int light, unsigned int pname, const float* params) {
    if (usingNativeGlPath()) {
        glLightfv((GLenum)light, (GLenum)pname, params);
    }
    initLightingState();
    if (!params) return;
    if (light < GL_LIGHT0 || light > GL_LIGHT7) return;
    GfxLight* l = &gLights[light - GL_LIGHT0];
    switch (pname) {
        case GL_AMBIENT:
            memcpy(l->ambient, params, sizeof(float) * 4);
            break;
        case GL_DIFFUSE:
            memcpy(l->diffuse, params, sizeof(float) * 4);
            break;
        case GL_SPECULAR:
            memcpy(l->specular, params, sizeof(float) * 4);
            break;
        case GL_POSITION:
            memcpy(l->position, params, sizeof(float) * 4);
            break;
        default:
            break;
    }
}

void gfx3dMaterialfv(unsigned int face, unsigned int pname, const float* params) {
    if (usingNativeGlPath()) {
        glMaterialfv((GLenum)face, (GLenum)pname, params);
    }
    (void)face;
    if (!params) return;
    switch (pname) {
        case GL_AMBIENT:
            memcpy(gMaterialAmbient, params, sizeof(float) * 4);
            break;
        case GL_DIFFUSE:
            memcpy(gMaterialDiffuse, params, sizeof(float) * 4);
            break;
        case GL_SPECULAR:
            memcpy(gMaterialSpecular, params, sizeof(float) * 4);
            break;
        case GL_EMISSION:
            memcpy(gMaterialEmission, params, sizeof(float) * 4);
            break;
        default:
            break;
    }
}

void gfx3dMaterialf(unsigned int face, unsigned int pname, float value) {
    if (usingNativeGlPath()) {
        glMaterialf((GLenum)face, (GLenum)pname, value);
    }
    (void)face;
    if (pname == GL_SHININESS) {
        gMaterialShininess = value;
    }
}

void gfx3dColorMaterial(unsigned int face, unsigned int mode) {
    /* iOS 26.2 ES1 headers no longer expose glColorMaterial. Keep the state
       cached so software lighting paths still see the requested mode. */
    gColorMaterialFace = face;
    gColorMaterialMode = mode;
}

void gfx3dBlendFunc(unsigned int src, unsigned int dst) {
    if (usingNativeGlPath()) {
        glBlendFunc((GLenum)src, (GLenum)dst);
    }
    gBlendSrc = src;
    gBlendDst = dst;
    if (usingNativeGlPath() && gSdlRenderer) {
        if (src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA) {
            SDL_SetRenderDrawBlendMode(gSdlRenderer, SDL_BLENDMODE_BLEND);
        } else {
            SDL_SetRenderDrawBlendMode(gSdlRenderer, SDL_BLENDMODE_NONE);
        }
    }
}

void gfx3dCullFace(unsigned int mode) {
    if (usingNativeGlPath()) {
        glCullFace((GLenum)mode);
    }
}

void gfx3dDepthMask(bool enable) {
    if (usingNativeGlPath()) {
        glDepthMask(enable ? GL_TRUE : GL_FALSE);
    }
}

void gfx3dDepthFunc(unsigned int func) {
    if (usingNativeGlPath()) {
        glDepthFunc((GLenum)func);
    }
}

void gfx3dLineWidth(float width) {
    if (usingNativeGlPath()) {
        glLineWidth(width);
    }
}

unsigned int gfx3dGenLists(int range) {
    if (range <= 0) {
        return 0;
    }
    static unsigned int nextId = 1;
    unsigned int base = nextId;
    nextId += (unsigned int)range;
    return base;
}

void gfx3dDeleteLists(unsigned int list, int range) {
    if (range <= 0) {
        return;
    }
    unsigned int start = list;
    unsigned int end = list + (unsigned int)range;
    for (unsigned int id = start; id < end; ++id) {
        DisplayList* dl = findDisplayList(id);
        if (dl) {
            freeDisplayList(dl);
            dl->id = 0;
        }
    }
}

void gfx3dNewList(unsigned int list, unsigned int mode) {
    (void)mode;
    if (gRecordingList) {
        return;
    }
    gRecordingList = true;
    gRecordingListId = list;
    resetRecordingBuffers();
}

void gfx3dEndList(void) {
    if (!gRecordingList) {
        return;
    }
    DisplayList* dl = getOrCreateDisplayList(gRecordingListId);
    if (!dl) {
        resetRecordingBuffers();
        gRecordingList = false;
        return;
    }
    freeDisplayList(dl);
    dl->vertices = gRecordingVertices;
    dl->vertexCount = gRecordingVertexCount;
    dl->vertexCapacity = gRecordingVertexCapacity;
    dl->commands = gRecordingCommands;
    dl->commandCount = gRecordingCommandCount;
    dl->commandCapacity = gRecordingCommandCapacity;
    gRecordingVertices = NULL;
    gRecordingCommands = NULL;
    gRecordingVertexCount = gRecordingVertexCapacity = 0;
    gRecordingCommandCount = gRecordingCommandCapacity = 0;
    gRecordingList = false;
    gRecordingCurrentCommand = NULL;
}

void gfx3dCallList(unsigned int list) {
    DisplayList* dl = findDisplayList(list);
    if (!dl || dl->vertexCount == 0 || dl->commandCount == 0) {
        return;
    }
    for (size_t ci = 0; ci < dl->commandCount; ++ci) {
        RecordedCommand* cmd = &dl->commands[ci];
        if (cmd->vertexCount == 0) {
            continue;
        }
        gImmediatePrimitive = cmd->primitive;
        gImmediateCount = 0;
        for (size_t vi = 0; vi < cmd->vertexCount; ++vi) {
            RecordedVertex* rv = &dl->vertices[cmd->firstVertex + vi];
            emitImmediateVertex(rv->position, rv->normal, rv->color);
        }
        flushImmediate();
    }
}

void gfx3dPixelStorei(unsigned int pname, int param) {
    if (usingNativeGlPath()) {
        glPixelStorei((GLenum)pname, param);
    }
}

void gfx3dReadBuffer(unsigned int mode) {
    if (usingNativeGlPath()) {
#ifdef GL_READ_BUFFER
        glReadBuffer((GLenum)mode);
#else
        (void)mode;
#endif
    }
}

void gfx3dReadPixels(int x, int y, int width, int height,
                     unsigned int format, unsigned int type, void* pixels) {
    if (usingNativeGlPath()) {
        glReadPixels(x, y, width, height, (GLenum)format, (GLenum)type, pixels);
        return;
    }
    (void)format;
    (void)type;
    if (!pixels || width <= 0 || height <= 0 || !ensureFramebuffer()) {
        return;
    }
    for (int row = 0; row < height; ++row) {
        int srcY = y + row;
        if (srcY < 0 || srcY >= gFramebufferHeight) {
            continue;
        }
        Uint32* dst = (Uint32*)((Uint8*)pixels + row * width * sizeof(Uint32));
        for (int col = 0; col < width; ++col) {
            int srcX = x + col;
            if (srcX < 0 || srcX >= gFramebufferWidth) {
                dst[col] = 0;
            } else {
                dst[col] = gColorBuffer[(size_t)srcY * (size_t)gFramebufferWidth + (size_t)srcX];
            }
        }
    }
}

unsigned int gfx3dGetError(void) {
    if (usingNativeGlPath()) {
        return (unsigned int)glGetError();
    }
    return 0;
}

void gfx3dPresent(void) {
    if (!gFramebufferDirty || !gColorBuffer || gFramebufferWidth <= 0 || gFramebufferHeight <= 0) {
        return;
    }

    if (gSdlRenderer) {
        if (!gFramebufferTexture) {
            gFramebufferTexture = SDL_CreateTexture(gSdlRenderer,
                                                    SDL_PIXELFORMAT_ABGR8888,
                                                    SDL_TEXTUREACCESS_STREAMING,
                                                    gFramebufferWidth,
                                                    gFramebufferHeight);
            if (!gFramebufferTexture) {
                return;
            }
        }
        SDL_UpdateTexture(gFramebufferTexture, NULL, gColorBuffer, gFramebufferWidth * (int)sizeof(Uint32));
        SDL_RenderCopy(gSdlRenderer, gFramebufferTexture, NULL, NULL);
        SDL_RenderPresent(gSdlRenderer);
        gFramebufferDirty = false;
        return;
    }

    if (!ensureFramebufferGlTexture()) {
        return;
    }

    GLint savedViewport[4] = {0, 0, 0, 0};
    GLint savedMatrixMode = GL_MODELVIEW;
    GLfloat savedProjection[16];
    GLfloat savedModelview[16];
    GLint savedTextureBinding = 0;

    GLboolean wasDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean wasBlend = glIsEnabled(GL_BLEND);
    GLboolean wasLighting = glIsEnabled(GL_LIGHTING);
    GLboolean wasCullFace = glIsEnabled(GL_CULL_FACE);
    GLboolean wasTexture2D = glIsEnabled(GL_TEXTURE_2D);
    glGetIntegerv(GL_VIEWPORT, savedViewport);
    glGetIntegerv(GL_MATRIX_MODE, &savedMatrixMode);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTextureBinding);

    glMatrixMode(GL_PROJECTION);
    glGetFloatv(GL_PROJECTION_MATRIX, savedProjection);
    glMatrixMode(GL_MODELVIEW);
    glGetFloatv(GL_MODELVIEW_MATRIX, savedModelview);

    glViewport(0, 0, gFramebufferWidth, gFramebufferHeight);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, gFramebufferGlTexture);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0.0f, (GLfloat)gFramebufferWidth, (GLfloat)gFramebufferHeight, 0.0f, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    const GLfloat verts[8] = {
        0.0f, 0.0f,
        (GLfloat)gFramebufferWidth, 0.0f,
        0.0f, (GLfloat)gFramebufferHeight,
        (GLfloat)gFramebufferWidth, (GLfloat)gFramebufferHeight
    };
    const GLfloat texCoords[8] = {
        0.0f, 1.0f,
        1.0f, 1.0f,
        0.0f, 0.0f,
        1.0f, 0.0f
    };

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(savedProjection);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(savedModelview);

    if (wasDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (wasBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (wasLighting) glEnable(GL_LIGHTING); else glDisable(GL_LIGHTING);
    if (wasCullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (wasTexture2D) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);

    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTextureBinding);
    glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
    glMatrixMode(savedMatrixMode);

    gFramebufferDirty = false;
}

void gfx3dReleaseResources(void) {
    free(gImmediateVertices);
    gImmediateVertices = NULL;
    gImmediateCapacity = 0;
    gImmediateCount = 0;
    gImmediateRecording = false;

    free(gImmediateQuadVertices);
    gImmediateQuadVertices = NULL;
    gImmediateQuadCapacity = 0;

    free(gColorBuffer);
    free(gDepthBuffer);
    gColorBuffer = NULL;
    gDepthBuffer = NULL;
    gFramebufferWidth = 0;
    gFramebufferHeight = 0;
    gFramebufferDirty = false;

    if (gFramebufferTexture) {
        SDL_DestroyTexture(gFramebufferTexture);
        gFramebufferTexture = NULL;
    }

    if (gFramebufferGlTexture != 0) {
        if (gSdlGLContext) {
            glDeleteTextures(1, &gFramebufferGlTexture);
        }
        gFramebufferGlTexture = 0;
    }
    gFramebufferGlTextureWidth = 0;
    gFramebufferGlTextureHeight = 0;
}

#endif // defined(PSCAL_TARGET_IOS) || defined(__APPLE__)
