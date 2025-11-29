#if defined(__APPLE__)
#ifndef GLES_SILENCE_DEPRECATION
#define GLES_SILENCE_DEPRECATION 1
#endif
#endif

#include "runtime/shaders/sky/cloud_layer.h"

#ifdef SDL

#include "runtime/shaders/shader_common.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct CloudVertex {
    float position[2];
    float texCoord[2];
} CloudVertex;

struct CloudLayerRenderer {
    GLuint program;
    GLint attribPosition;
    GLint attribTexCoord;
    GLint uniformTime;
    GLint uniformNoise;
    GLint uniformParallax;
    GLint uniformCoverage;
    GLint uniformSoftness;
    GLint uniformDayFactor;
    GLint uniformSunDirection;
    GLint uniformLayerScale;

    GLuint vertexBuffer;
    GLuint indexBuffer;
    GLuint noiseTexture;
    bool gpuReady;
};

static const char *kCloudVertexShader =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "attribute vec2 aPosition;\n"
    "attribute vec2 aTexCoord;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPosition, 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
    "}\n";

static const char *kCloudFragmentShader =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "varying vec2 vTexCoord;\n"
    "uniform sampler2D uNoise;\n"
    "uniform float uTime;\n"
    "uniform vec2 uParallax;\n"
    "uniform float uCoverage;\n"
    "uniform float uSoftness;\n"
    "uniform float uDayFactor;\n"
    "uniform vec3 uSunDirection;\n"
    "uniform vec4 uLayerScale;\n"
    "\n"
    "vec2 layerScroll(float speed, float offset) {\n"
    "    float t = uTime * speed;\n"
    "    return vec2(t, t * offset);\n"
    "}\n"
    "\n"
    "float sampleLayer(vec2 uv, vec2 scroll, float scale) {\n"
    "    return texture2D(uNoise, fract(uv * scale + scroll)).r;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec2 baseCoord = vTexCoord + uParallax;\n"
    "    vec2 scroll1 = layerScroll(0.010, 0.25);\n"
    "    vec2 scroll2 = layerScroll(0.018, -0.35);\n"
    "    vec2 scroll3 = layerScroll(0.032, 0.15);\n"
    "\n"
    "    float n1 = sampleLayer(baseCoord, scroll1, uLayerScale.x);\n"
    "    float n2 = sampleLayer(baseCoord, scroll2, uLayerScale.y);\n"
    "    float n3 = sampleLayer(baseCoord, scroll3, uLayerScale.z);\n"
    "    float combined = clamp(n1 * n2 * n3, 0.0, 1.0);\n"
    "\n"
    "    float cloudAlpha = smoothstep(uCoverage - uSoftness, uCoverage + uSoftness, combined);\n"
    "    if (cloudAlpha <= 0.003) discard;\n"
    "\n"
    "    float shading = smoothstep(uCoverage, 1.0, combined);\n"
    "    vec3 dayLight = vec3(1.0, 1.0, 0.98);\n"
    "    vec3 dayShadow = vec3(0.74, 0.78, 0.88);\n"
    "    vec3 nightLight = vec3(0.62, 0.70, 0.86);\n"
    "    vec3 nightShadow = vec3(0.28, 0.32, 0.48);\n"
    "\n"
    "    vec3 cloudLight = mix(nightLight, dayLight, clamp(uDayFactor, 0.0, 1.0));\n"
    "    vec3 cloudShadow = mix(nightShadow, dayShadow, clamp(uDayFactor, 0.0, 1.0));\n"
    "    vec3 color = mix(cloudShadow, cloudLight, shading);\n"
    "\n"
    "    float sunGlow = clamp(uSunDirection.y * 0.45 + 0.2, 0.0, 1.0);\n"
    "    color += sunGlow * 0.08 * vec3(1.0, 0.92, 0.80);\n"
    "    gl_FragColor = vec4(color, clamp(cloudAlpha, 0.0, 1.0));\n"
    "}\n";

static const int kCloudNoiseSize = 256;

static float fadef(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

static uint32_t hash2(int x, int y, int seed) {
    uint32_t h = (uint32_t)(x * 374761 + y * 668265 + seed * 69069);
    h = (h ^ (h >> 13)) * 1274126177u;
    h = (h ^ (h >> 16));
    return h;
}

static float randomValue(int x, int y, int seed) {
    return (hash2(x, y, seed) & 0xFFFF) / 65535.0f;
}

static float tileableNoiseSample(float x, float y, int period, int seed) {
    float xf = floorf(x);
    float yf = floorf(y);
    float tx = x - xf;
    float ty = y - yf;
    int xi0 = ((int)xf) % period;
    int yi0 = ((int)yf) % period;
    if (xi0 < 0) xi0 += period;
    if (yi0 < 0) yi0 += period;
    int xi1 = (xi0 + 1) % period;
    int yi1 = (yi0 + 1) % period;

    float v00 = randomValue(xi0, yi0, seed);
    float v10 = randomValue(xi1, yi0, seed);
    float v01 = randomValue(xi0, yi1, seed);
    float v11 = randomValue(xi1, yi1, seed);

    float u = fadef(tx);
    float v = fadef(ty);
    float nx0 = lerpf(v00, v10, u);
    float nx1 = lerpf(v01, v11, u);
    return lerpf(nx0, nx1, v);
}

static bool generateNoiseTexture(uint8_t *buffer, size_t bufferSize) {
    if (!buffer) return false;
    size_t required = (size_t)kCloudNoiseSize * (size_t)kCloudNoiseSize;
    if (bufferSize < required) return false;

    const int octaves = 4;
    const float persistence = 0.55f;
    const int period = kCloudNoiseSize;

    for (int y = 0; y < kCloudNoiseSize; ++y) {
        for (int x = 0; x < kCloudNoiseSize; ++x) {
            float value = 0.0f;
            float amplitude = 1.0f;
            float frequency = 1.0f;
            float norm = 0.0f;
            for (int octave = 0; octave < octaves; ++octave) {
                float sample = tileableNoiseSample(x * frequency,
                                                   y * frequency,
                                                   period,
                                                   1337 + octave * 97);
                value += sample * amplitude;
                norm += amplitude;
                amplitude *= persistence;
                frequency *= 2.0f;
            }
            value = (norm > 0.0f) ? value / norm : value;
            if (value < 0.0f) value = 0.0f;
            if (value > 1.0f) value = 1.0f;
            buffer[y * kCloudNoiseSize + x] = (uint8_t)(value * 255.0f + 0.5f);
        }
    }
    return true;
}

static bool uploadNoiseTexture(GLuint *textureOut) {
    if (!textureOut) return false;
    uint8_t *pixels = (uint8_t *)malloc((size_t)kCloudNoiseSize * (size_t)kCloudNoiseSize);
    if (!pixels) return false;
    bool ok = generateNoiseTexture(pixels, (size_t)kCloudNoiseSize * (size_t)kCloudNoiseSize);
    if (!ok) {
        free(pixels);
        return false;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
#ifdef GL_LUMINANCE
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_LUMINANCE,
                 kCloudNoiseSize,
                 kCloudNoiseSize,
                 0,
                 GL_LUMINANCE,
                 GL_UNSIGNED_BYTE,
                 pixels);
#else
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RED,
                 kCloudNoiseSize,
                 kCloudNoiseSize,
                 0,
                 GL_RED,
                 GL_UNSIGNED_BYTE,
                 pixels);
#endif
    glBindTexture(GL_TEXTURE_2D, 0);
    free(pixels);
    if (!texture) return false;
    *textureOut = texture;
    return true;
}

static bool ensureGeometry(CloudLayerRenderer *renderer) {
    if (!renderer) return false;
    if (renderer->vertexBuffer && renderer->indexBuffer) {
        return true;
    }

    static const CloudVertex vertices[4] = {
        {{-1.0f, -1.0f}, {0.0f, 0.0f}},
        {{1.0f, -1.0f}, {1.0f, 0.0f}},
        {{1.0f, 1.0f}, {1.0f, 1.0f}},
        {{-1.0f, 1.0f}, {0.0f, 1.0f}},
    };
    static const GLushort indices[6] = {0, 1, 2, 0, 2, 3};

    glGenBuffers(1, &renderer->vertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, renderer->vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenBuffers(1, &renderer->indexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, renderer->indexBuffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    return true;
}

static bool ensureProgram(CloudLayerRenderer *renderer) {
    if (!renderer) return false;
    if (renderer->program) return true;

    char infoLog[512];
    GLuint program = runtimeCreateProgram(kCloudVertexShader, kCloudFragmentShader,
                                          infoLog, sizeof(infoLog));
    if (!program) {
        return false;
    }

    glBindAttribLocation(program, 0, "aPosition");
    glBindAttribLocation(program, 1, "aTexCoord");
    if (!runtimeLinkProgram(program, infoLog, sizeof(infoLog))) {
        runtimeDestroyProgram(program);
        return false;
    }

    renderer->program = program;
    renderer->attribPosition = glGetAttribLocation(program, "aPosition");
    renderer->attribTexCoord = glGetAttribLocation(program, "aTexCoord");
    renderer->uniformTime = glGetUniformLocation(program, "uTime");
    renderer->uniformNoise = glGetUniformLocation(program, "uNoise");
    renderer->uniformParallax = glGetUniformLocation(program, "uParallax");
    renderer->uniformCoverage = glGetUniformLocation(program, "uCoverage");
    renderer->uniformSoftness = glGetUniformLocation(program, "uSoftness");
    renderer->uniformDayFactor = glGetUniformLocation(program, "uDayFactor");
    renderer->uniformSunDirection = glGetUniformLocation(program, "uSunDirection");
    renderer->uniformLayerScale = glGetUniformLocation(program, "uLayerScale");
    return true;
}

void cloudLayerRendererInit(CloudLayerRenderer **rendererPtr) {
    if (!rendererPtr || *rendererPtr) return;
    CloudLayerRenderer *renderer = (CloudLayerRenderer *)calloc(1, sizeof(CloudLayerRenderer));
    if (!renderer) return;
    *rendererPtr = renderer;
}

void cloudLayerRendererShutdown(CloudLayerRenderer **rendererPtr) {
    if (!rendererPtr || !*rendererPtr) return;
    CloudLayerRenderer *renderer = *rendererPtr;
    if (renderer->noiseTexture) {
        glDeleteTextures(1, &renderer->noiseTexture);
        renderer->noiseTexture = 0;
    }
    if (renderer->vertexBuffer) {
        glDeleteBuffers(1, &renderer->vertexBuffer);
        renderer->vertexBuffer = 0;
    }
    if (renderer->indexBuffer) {
        glDeleteBuffers(1, &renderer->indexBuffer);
        renderer->indexBuffer = 0;
    }
    if (renderer->program) {
        runtimeDestroyProgram(renderer->program);
        renderer->program = 0;
    }
    free(renderer);
    *rendererPtr = NULL;
}

static bool ensureResources(CloudLayerRenderer **rendererPtr) {
    if (!rendererPtr) return false;
    if (!*rendererPtr) {
        cloudLayerRendererInit(rendererPtr);
        if (!*rendererPtr) return false;
    }
    CloudLayerRenderer *renderer = *rendererPtr;
    if (!ensureProgram(renderer)) return false;
    if (!ensureGeometry(renderer)) return false;
    if (!renderer->noiseTexture) {
        if (!uploadNoiseTexture(&renderer->noiseTexture)) {
            return false;
        }
    }
    return true;
}

static float clampf(float value, float minValue, float maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

bool cloudLayerRendererDraw(CloudLayerRenderer **rendererPtr, const CloudLayerParams *params) {
    if (!params) return false;
    if (!ensureResources(rendererPtr)) return false;
    CloudLayerRenderer *renderer = *rendererPtr;

    GLboolean depthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    GLboolean blendEnabled = glIsEnabled(GL_BLEND);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    glUseProgram(renderer->program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, renderer->noiseTexture);
    glUniform1i(renderer->uniformNoise, 0);
    glUniform1f(renderer->uniformTime, params->timeSeconds);

    float parallaxX = params->cameraOffsetX * params->parallaxScale;
    float parallaxZ = params->cameraOffsetZ * params->parallaxScale;
    glUniform2f(renderer->uniformParallax, parallaxX, parallaxZ);
    glUniform1f(renderer->uniformCoverage, clampf(params->coverage, 0.05f, 0.95f));
    glUniform1f(renderer->uniformSoftness, clampf(params->softness, 0.01f, 0.45f));
    glUniform1f(renderer->uniformDayFactor, clampf(params->dayFactor, 0.0f, 1.0f));
    glUniform3f(renderer->uniformSunDirection,
                params->sunDirection[0],
                params->sunDirection[1],
                params->sunDirection[2]);

    glUniform4f(renderer->uniformLayerScale, 0.5f, 1.0f, 2.0f, 0.0f);

    glBindBuffer(GL_ARRAY_BUFFER, renderer->vertexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, renderer->indexBuffer);

    glEnableVertexAttribArray((GLuint)renderer->attribPosition);
    glVertexAttribPointer((GLuint)renderer->attribPosition,
                          2,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(CloudVertex),
                          (const GLvoid *)(uintptr_t)offsetof(CloudVertex, position));
    glEnableVertexAttribArray((GLuint)renderer->attribTexCoord);
    glVertexAttribPointer((GLuint)renderer->attribTexCoord,
                          2,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(CloudVertex),
                          (const GLvoid *)(uintptr_t)offsetof(CloudVertex, texCoord));

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);

    glDisableVertexAttribArray((GLuint)renderer->attribPosition);
    glDisableVertexAttribArray((GLuint)renderer->attribTexCoord);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    glDepthMask(depthMask);
    if (!blendEnabled) {
        glDisable(GL_BLEND);
    }
    return true;
}

#endif // SDL
