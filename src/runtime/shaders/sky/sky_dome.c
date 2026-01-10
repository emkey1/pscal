#if defined(__APPLE__)
#ifndef GLES_SILENCE_DEPRECATION
#define GLES_SILENCE_DEPRECATION 1
#endif
#endif

#include "runtime/shaders/sky/sky_dome.h"

#ifdef SDL

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(PSCAL_TARGET_IOS)
static void skySetIdentity(float m[16]) {
    if (!m) return;
    for (int i = 0; i < 16; ++i) {
        m[i] = 0.0f;
    }
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct SkyVertex {
    float position[3];
} SkyVertex;

static struct {
    GLuint program;
    GLint attribPosition;
    GLint uniformModelViewProjection;
    GLint uniformRadius;
    GLint uniformHorizonColor;
    GLint uniformZenithColor;
} gSkyShader = {0};

static const char *kSkyVertexShader =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "attribute vec3 aPosition;\n"
    "uniform mat4 uModelViewProjection;\n"
    "uniform float uRadius;\n"
    "varying float vHeight;\n"
    "void main() {\n"
    "    vec4 position = vec4(aPosition * uRadius, 1.0);\n"
    "    gl_Position = uModelViewProjection * position;\n"
    "    vHeight = clamp(aPosition.y, 0.0, 1.0);\n"
    "}\n";

static const char *kSkyFragmentShader =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "varying float vHeight;\n"
    "uniform vec3 uHorizonColor;\n"
    "uniform vec3 uZenithColor;\n"
    "void main() {\n"
    "    float blend = pow(vHeight, 1.4);\n"
    "    vec3 color = mix(uHorizonColor, uZenithColor, clamp(blend, 0.0, 1.0));\n"
    "    gl_FragColor = vec4(color, 1.0);\n"
    "}\n";

static bool ensureSkyShader(void) {
    if (gSkyShader.program) {
        return true;
    }

    char infoLog[256];
    GLuint program = runtimeCreateProgram(kSkyVertexShader, kSkyFragmentShader, infoLog, sizeof(infoLog));
    if (!program) {
        return false;
    }

    glBindAttribLocation(program, 0, "aPosition");
    if (!runtimeLinkProgram(program, infoLog, sizeof(infoLog))) {
        runtimeDestroyProgram(program);
        return false;
    }

    gSkyShader.program = program;
    gSkyShader.attribPosition = glGetAttribLocation(program, "aPosition");
    gSkyShader.uniformModelViewProjection = glGetUniformLocation(program, "uModelViewProjection");
    gSkyShader.uniformRadius = glGetUniformLocation(program, "uRadius");
    gSkyShader.uniformHorizonColor = glGetUniformLocation(program, "uHorizonColor");
    gSkyShader.uniformZenithColor = glGetUniformLocation(program, "uZenithColor");
    return true;
}

static void multiplyMat4(const float a[16], const float b[16], float out[16]) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
}

void skyDomeInit(SkyDome *dome) {
    if (!dome) return;
    memset(dome, 0, sizeof(*dome));
}

void skyDomeFree(SkyDome *dome) {
    if (!dome) return;
    if (dome->vbo) {
        glDeleteBuffers(1, &dome->vbo);
        dome->vbo = 0;
    }
    if (dome->ibo) {
        glDeleteBuffers(1, &dome->ibo);
        dome->ibo = 0;
    }
    dome->indexCount = 0;
    dome->gpuReady = false;
    dome->slices = 0;
    dome->stacks = 0;
}

static bool uploadSkyGeometry(SkyDome *dome, int slices, int stacks) {
    size_t vertexCount = (size_t)(slices + 1) * (size_t)(stacks + 1);
    size_t indexCount = (size_t)slices * (size_t)stacks * 6;

    SkyVertex *vertices = (SkyVertex *)malloc(vertexCount * sizeof(SkyVertex));
    unsigned int *indices = (unsigned int *)malloc(indexCount * sizeof(unsigned int));
    if (!vertices || !indices) {
        free(vertices);
        free(indices);
        return false;
    }

    size_t vertexIndex = 0;
    for (int stack = 0; stack <= stacks; ++stack) {
        float v = (float)stack / (float)stacks;
        float theta = v * (float)(M_PI * 0.5);
        float y = sinf(theta);
        float radius = cosf(theta);
        for (int slice = 0; slice <= slices; ++slice) {
            float u = (float)slice / (float)slices;
            float phi = u * (float)(M_PI * 2.0);
            float x = radius * cosf(phi);
            float z = radius * sinf(phi);
            vertices[vertexIndex].position[0] = x;
            vertices[vertexIndex].position[1] = y;
            vertices[vertexIndex].position[2] = z;
            ++vertexIndex;
        }
    }

    size_t indexCursor = 0;
    for (int stack = 0; stack < stacks; ++stack) {
        for (int slice = 0; slice < slices; ++slice) {
            unsigned int i0 = (unsigned int)(stack * (slices + 1) + slice);
            unsigned int i1 = i0 + 1;
            unsigned int i2 = (unsigned int)((stack + 1) * (slices + 1) + slice);
            unsigned int i3 = i2 + 1;

            indices[indexCursor++] = i0;
            indices[indexCursor++] = i2;
            indices[indexCursor++] = i1;
            indices[indexCursor++] = i1;
            indices[indexCursor++] = i2;
            indices[indexCursor++] = i3;
        }
    }

    if (!dome->vbo) {
        glGenBuffers(1, &dome->vbo);
    }
    if (!dome->ibo) {
        glGenBuffers(1, &dome->ibo);
    }

    glBindBuffer(GL_ARRAY_BUFFER, dome->vbo);
    glBufferData(GL_ARRAY_BUFFER, vertexCount * sizeof(SkyVertex), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, dome->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indexCount * sizeof(unsigned int), indices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    free(vertices);
    free(indices);

    dome->indexCount = indexCount;
    dome->slices = slices;
    dome->stacks = stacks;
    dome->gpuReady = true;
    return true;
}

bool skyDomeEnsureUploaded(SkyDome *dome, int slices, int stacks) {
    if (!dome) return false;
    if (slices < 4) slices = 4;
    if (stacks < 2) stacks = 2;
    if (dome->gpuReady && dome->slices == slices && dome->stacks == stacks) {
        return true;
    }
    return uploadSkyGeometry(dome, slices, stacks);
}

void skyDomeDraw(SkyDome *dome, float radius, const float horizonColor[3], const float zenithColor[3]) {
    if (!dome || !dome->gpuReady) return;
    if (!ensureSkyShader()) return;
    if (radius <= 0.0f) radius = 500.0f;

    glUseProgram(gSkyShader.program);

    float modelView[16];
    float projection[16];
#if defined(PSCAL_TARGET_IOS)
    skySetIdentity(modelView);
    skySetIdentity(projection);
#else
    glGetFloatv(GL_MODELVIEW_MATRIX, modelView);
    glGetFloatv(GL_PROJECTION_MATRIX, projection);
#endif

    float mvp[16];
    multiplyMat4(projection, modelView, mvp);

    if (gSkyShader.uniformModelViewProjection >= 0) {
        glUniformMatrix4fv(gSkyShader.uniformModelViewProjection, 1, GL_FALSE, mvp);
    }
    if (gSkyShader.uniformRadius >= 0) {
        glUniform1f(gSkyShader.uniformRadius, radius);
    }
    if (horizonColor && gSkyShader.uniformHorizonColor >= 0) {
        glUniform3fv(gSkyShader.uniformHorizonColor, 1, horizonColor);
    }
    if (zenithColor && gSkyShader.uniformZenithColor >= 0) {
        glUniform3fv(gSkyShader.uniformZenithColor, 1, zenithColor);
    }

    GLboolean depthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    glDepthMask(GL_FALSE);

    GLboolean cullEnabled = glIsEnabled(GL_CULL_FACE);
    if (cullEnabled) {
        glDisable(GL_CULL_FACE);
    }

    glBindBuffer(GL_ARRAY_BUFFER, dome->vbo);
    glEnableVertexAttribArray((GLuint)gSkyShader.attribPosition);
    glVertexAttribPointer((GLuint)gSkyShader.attribPosition, 3, GL_FLOAT, GL_FALSE, sizeof(SkyVertex), (const void *)0);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, dome->ibo);
    glDrawElements(GL_TRIANGLES, (GLsizei)dome->indexCount, GL_UNSIGNED_INT, (const void *)0);

    glDisableVertexAttribArray((GLuint)gSkyShader.attribPosition);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    if (cullEnabled) {
        glEnable(GL_CULL_FACE);
    }
    glDepthMask(depthMask);

    glUseProgram(0);
}

#endif // SDL
