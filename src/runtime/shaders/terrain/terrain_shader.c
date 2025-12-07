#if defined(__APPLE__)
#ifndef GLES_SILENCE_DEPRECATION
#define GLES_SILENCE_DEPRECATION 1
#endif
#endif

#include "runtime/shaders/terrain/terrain_shader.h"

#include <math.h>
#include <string.h>
#include <ctype.h>

static float clampf(float value, float minValue, float maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

static float saturatef(float value) {
    return clampf(value, 0.0f, 1.0f);
}

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

static const TerrainPalette kPalettePresets[TERRAIN_PALETTE__COUNT] = {
    // Temperate
    {
        {0.05f, 0.20f, 0.35f},
        {0.12f, 0.38f, 0.58f},
        {0.19f, 0.36f, 0.19f},
        {0.36f, 0.50f, 0.26f},
        {0.46f, 0.44f, 0.36f},
        {0.90f, 0.92f, 0.95f},
    },
    // Desert
    {
        {0.10f, 0.18f, 0.30f},
        {0.20f, 0.34f, 0.48f},
        {0.58f, 0.45f, 0.28f},
        {0.76f, 0.58f, 0.34f},
        {0.72f, 0.54f, 0.42f},
        {0.96f, 0.88f, 0.74f},
    },
    // Arctic
    {
        {0.06f, 0.16f, 0.30f},
        {0.14f, 0.32f, 0.52f},
        {0.70f, 0.78f, 0.82f},
        {0.78f, 0.84f, 0.88f},
        {0.82f, 0.86f, 0.92f},
        {0.96f, 0.98f, 1.0f},
    },
};

static const char *kPaletteNames[TERRAIN_PALETTE__COUNT] = {
    "temperate",
    "desert",
    "arctic",
};

static const TerrainLighting kLightingPresets[TERRAIN_LIGHTING__COUNT] = {
    // Noon
    {
        {0.32f, 0.35f, 0.40f},
        {0.95f, 0.96f, 0.92f},
        {-0.35f, -1.0f, -0.28f},
        {0.78f, 0.86f, 0.96f},
        120.0f,
        480.0f,
        {0.60f, 0.74f, 0.92f},
        {0.08f, 0.26f, 0.52f},
    },
    // Sunset
    {
        {0.28f, 0.22f, 0.20f},
        {1.00f, 0.58f, 0.36f},
        {-0.25f, -0.85f, 0.15f},
        {0.76f, 0.52f, 0.44f},
        90.0f,
        320.0f,
        {0.86f, 0.56f, 0.40f},
        {0.26f, 0.08f, 0.26f},
    },
    // Midnight
    {
        {0.12f, 0.14f, 0.20f},
        {0.42f, 0.48f, 0.70f},
        {-0.15f, -0.60f, -0.28f},
        {0.06f, 0.08f, 0.16f},
        80.0f,
        240.0f,
        {0.08f, 0.10f, 0.20f},
        {0.02f, 0.04f, 0.08f},
    },
};

static const char *kLightingNames[TERRAIN_LIGHTING__COUNT] = {
    "noon",
    "sunset",
    "midnight",
};

static bool gStyleInitialised = false;
static TerrainPalette gCurrentPalette = {0};
static TerrainLighting gCurrentLighting = {0};
static TerrainPalettePreset gCurrentPalettePreset = TERRAIN_PALETTE_TEMPERATE;
static TerrainLightingPreset gCurrentLightingPreset = TERRAIN_LIGHTING_NOON;

static void ensureStyleState(void) {
    if (gStyleInitialised) return;
    memcpy(&gCurrentPalette, &kPalettePresets[TERRAIN_PALETTE_TEMPERATE], sizeof(TerrainPalette));
    memcpy(&gCurrentLighting, &kLightingPresets[TERRAIN_LIGHTING_NOON], sizeof(TerrainLighting));
    gCurrentPalettePreset = TERRAIN_PALETTE_TEMPERATE;
    gCurrentLightingPreset = TERRAIN_LIGHTING_NOON;
    gStyleInitialised = true;
}

void terrainShaderSetPalettePreset(TerrainPalettePreset preset) {
    ensureStyleState();
    if (preset < 0 || preset >= TERRAIN_PALETTE__COUNT) {
        return;
    }
    memcpy(&gCurrentPalette, &kPalettePresets[preset], sizeof(TerrainPalette));
    gCurrentPalettePreset = preset;
}

bool terrainShaderPalettePresetFromName(const char *name, TerrainPalettePreset *outPreset) {
    if (!name) return false;
    for (int i = 0; i < TERRAIN_PALETTE__COUNT; ++i) {
        const char *presetName = kPaletteNames[i];
        const char *lhs = name;
        const char *rhs = presetName;
        bool match = true;
        while (*lhs && *rhs) {
            if (tolower((unsigned char)*lhs) != tolower((unsigned char)*rhs)) {
                match = false;
                break;
            }
            ++lhs;
            ++rhs;
        }
        if (match && *lhs == '\0' && *rhs == '\0') {
            if (outPreset) *outPreset = (TerrainPalettePreset)i;
            return true;
        }
    }
    return false;
}

TerrainPalettePreset terrainShaderCurrentPalettePreset(void) {
    ensureStyleState();
    return gCurrentPalettePreset;
}

size_t terrainShaderPalettePresetCount(void) {
    return (size_t)TERRAIN_PALETTE__COUNT;
}

const char *terrainShaderPalettePresetLabel(size_t index) {
    if (index >= TERRAIN_PALETTE__COUNT) return NULL;
    return kPaletteNames[index];
}

const TerrainPalette *terrainShaderGetCurrentPalette(void) {
    ensureStyleState();
    return &gCurrentPalette;
}

void terrainShaderSetLightingPreset(TerrainLightingPreset preset) {
    ensureStyleState();
    if (preset < 0 || preset >= TERRAIN_LIGHTING__COUNT) {
        return;
    }
    memcpy(&gCurrentLighting, &kLightingPresets[preset], sizeof(TerrainLighting));
    gCurrentLightingPreset = preset;
}

bool terrainShaderLightingPresetFromName(const char *name, TerrainLightingPreset *outPreset) {
    if (!name) return false;
    for (int i = 0; i < TERRAIN_LIGHTING__COUNT; ++i) {
        const char *presetName = kLightingNames[i];
        const char *lhs = name;
        const char *rhs = presetName;
        bool match = true;
        while (*lhs && *rhs) {
            if (tolower((unsigned char)*lhs) != tolower((unsigned char)*rhs)) {
                match = false;
                break;
            }
            ++lhs;
            ++rhs;
        }
        if (match && *lhs == '\0' && *rhs == '\0') {
            if (outPreset) *outPreset = (TerrainLightingPreset)i;
            return true;
        }
    }
    return false;
}

TerrainLightingPreset terrainShaderCurrentLightingPreset(void) {
    ensureStyleState();
    return gCurrentLightingPreset;
}

size_t terrainShaderLightingPresetCount(void) {
    return (size_t)TERRAIN_LIGHTING__COUNT;
}

const char *terrainShaderLightingPresetLabel(size_t index) {
    if (index >= TERRAIN_LIGHTING__COUNT) return NULL;
    return kLightingNames[index];
}

const TerrainLighting *terrainShaderGetCurrentLighting(void) {
    ensureStyleState();
    return &gCurrentLighting;
}

void terrainShaderSampleGradient(float heightNormalized,
                                 float waterLevel,
                                 float slope,
                                 float outColor[3]) {
    ensureStyleState();
    if (!outColor) return;
    const TerrainPalette *palette = &gCurrentPalette;

    float h = saturatef(heightNormalized);
    float wLevel = saturatef(waterLevel);
    float slopeFactor = saturatef(slope);

    if (h < wLevel) {
        float safeLevel = wLevel > 1e-4f ? wLevel : 1.0f;
        float t = h / safeLevel;
        float blend = powf(saturatef(t), 0.75f);
        for (int i = 0; i < 3; ++i) {
            outColor[i] = lerpf(palette->waterDeep[i], palette->waterShallow[i], blend);
        }
        return;
    }

    float landSpan = 1.0f - wLevel;
    if (landSpan < 1e-4f) landSpan = 1.0f;
    float landT = (h - wLevel) / landSpan;
    landT = saturatef(landT);

    float lowBlend = saturatef((landT - 0.0f) / 0.45f);
    float midBlend = saturatef((landT - 0.25f) / 0.55f);
    float highBlend = saturatef((landT - 0.10f) / 0.80f);
    float peakBlend = saturatef((landT - 0.70f) / 0.30f);
    float slopeMix = slopeFactor * 0.35f;

    for (int i = 0; i < 3; ++i) {
        float lowMid = lerpf(palette->low[i], palette->mid[i], lowBlend);
        float midHigh = lerpf(palette->mid[i], palette->high[i], midBlend);
        float grad = lerpf(lowMid, midHigh, highBlend);
        grad = lerpf(grad, palette->peak[i], peakBlend);
        grad = lerpf(grad, palette->high[i], slopeMix);
        outColor[i] = saturatef(grad);
    }
}

const float *terrainShaderSkyHorizonColor(void) {
    ensureStyleState();
    return gCurrentLighting.skyHorizonColor;
}

const float *terrainShaderSkyZenithColor(void) {
    ensureStyleState();
    return gCurrentLighting.skyZenithColor;
}

#ifdef SDL

#include "runtime/terrain/terrain_generator.h"

static TerrainShaderHandles gShaderHandles = {0};
static bool gShaderInitialised = false;

static const char *kTerrainVertexShader =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "attribute vec3 aPosition;\n"
    "attribute vec3 aNormal;\n"
    "attribute vec3 aColor;\n"
    "uniform mat4 uModelViewMatrix;\n"
    "uniform mat4 uModelViewProjectionMatrix;\n"
    "uniform mat3 uNormalMatrix;\n"
    "uniform float uMinHeight;\n"
    "uniform float uHeightRange;\n"
    "varying vec3 vNormal;\n"
    "varying vec3 vBaseColor;\n"
    "varying vec3 vViewPos;\n"
    "varying float vHeight01;\n"
    "varying float vSlope;\n"
    "void main() {\n"
    "    vec4 modelPos = vec4(aPosition, 1.0);\n"
    "    vec4 viewPos = uModelViewMatrix * modelPos;\n"
    "    gl_Position = uModelViewProjectionMatrix * modelPos;\n"
    "    vViewPos = viewPos.xyz;\n"
    "    vec3 normal = normalize(uNormalMatrix * aNormal);\n"
    "    vNormal = normal;\n"
    "    vBaseColor = aColor;\n"
    "    vSlope = clamp(1.0 - normal.y, 0.0, 1.0);\n"
    "    float range = max(uHeightRange, 1e-5);\n"
    "    vHeight01 = clamp((aPosition.y - uMinHeight) / range, 0.0, 1.0);\n"
    "}\n";

static const char *kTerrainFragmentShader =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "varying vec3 vNormal;\n"
    "varying vec3 vBaseColor;\n"
    "varying vec3 vViewPos;\n"
    "varying float vHeight01;\n"
    "varying float vSlope;\n"
    "uniform vec3 uLightDir;\n"
    "uniform vec3 uLightColor;\n"
    "uniform vec3 uAmbientColor;\n"
    "uniform vec3 uFogColor;\n"
    "uniform float uFogStart;\n"
    "uniform float uFogEnd;\n"
    "uniform float uWaterLevel;\n"
    "uniform vec3 uPaletteWaterDeep;\n"
    "uniform vec3 uPaletteWaterShallow;\n"
    "uniform vec3 uPaletteLow;\n"
    "uniform vec3 uPaletteMid;\n"
    "uniform vec3 uPaletteHigh;\n"
    "uniform vec3 uPalettePeak;\n"
    "float saturate(float v) { return clamp(v, 0.0, 1.0); }\n"
    "vec3 sampleGradient(float height01, float slope) {\n"
    "    if (height01 < uWaterLevel) {\n"
    "        float safeLevel = max(uWaterLevel, 1e-4);\n"
    "        float t = saturate(height01 / safeLevel);\n"
    "        float blend = pow(t, 0.75);\n"
    "        return mix(uPaletteWaterDeep, uPaletteWaterShallow, blend);\n"
    "    }\n"
    "    float landSpan = max(1.0 - uWaterLevel, 1e-4);\n"
    "    float landT = saturate((height01 - uWaterLevel) / landSpan);\n"
    "    float lowBlend = smoothstep(0.0, 0.45, landT);\n"
    "    float midBlend = smoothstep(0.25, 0.8, landT);\n"
    "    float highBlend = smoothstep(0.1, 0.9, landT);\n"
    "    vec3 lowMid = mix(uPaletteLow, uPaletteMid, lowBlend);\n"
    "    vec3 midHigh = mix(uPaletteMid, uPaletteHigh, midBlend);\n"
    "    vec3 grad = mix(lowMid, midHigh, highBlend);\n"
    "    float peakBlend = smoothstep(0.7, 1.0, landT);\n"
    "    grad = mix(grad, uPalettePeak, peakBlend);\n"
    "    float slopeMix = saturate(slope) * 0.35;\n"
    "    grad = mix(grad, uPaletteHigh, slopeMix);\n"
    "    return grad;\n"
    "}\n"
    "void main() {\n"
    "    vec3 normal = normalize(vNormal);\n"
    "    vec3 lightDir = normalize(uLightDir);\n"
    "    float diffuse = max(dot(normal, -lightDir), 0.0);\n"
    "    vec3 paletteColor = sampleGradient(vHeight01, vSlope);\n"
    "    vec3 albedo = mix(paletteColor, vBaseColor, 0.25);\n"
    "    vec3 lighting = uAmbientColor + uLightColor * diffuse;\n"
    "    vec3 litColor = albedo * lighting;\n"
    "    float distance = length(vViewPos);\n"
    "    float fogRange = max(uFogEnd - uFogStart, 1e-4);\n"
    "    float fogFactor = saturate((uFogEnd - distance) / fogRange);\n"
    "    vec3 fogged = mix(uFogColor, litColor, fogFactor);\n"
    "    gl_FragColor = vec4(fogged, 1.0);\n"
    "}\n";

static bool ensureShaderProgram(void) {
    if (gShaderInitialised && gShaderHandles.program) {
        return true;
    }

    char infoLog[512];
    GLuint program = runtimeCreateProgram(kTerrainVertexShader, kTerrainFragmentShader, infoLog, sizeof(infoLog));
    if (!program) {
        return false;
    }

    glBindAttribLocation(program, 0, "aPosition");
    glBindAttribLocation(program, 1, "aNormal");
    glBindAttribLocation(program, 2, "aColor");

    if (!runtimeLinkProgram(program, infoLog, sizeof(infoLog))) {
        runtimeDestroyProgram(program);
        return false;
    }

    gShaderHandles.program = program;
    gShaderHandles.attribPosition = glGetAttribLocation(program, "aPosition");
    gShaderHandles.attribNormal = glGetAttribLocation(program, "aNormal");
    gShaderHandles.attribColor = glGetAttribLocation(program, "aColor");

    gShaderHandles.uniformModelView = glGetUniformLocation(program, "uModelViewMatrix");
    gShaderHandles.uniformModelViewProjection = glGetUniformLocation(program, "uModelViewProjectionMatrix");
    gShaderHandles.uniformNormalMatrix = glGetUniformLocation(program, "uNormalMatrix");
    gShaderHandles.uniformMinHeight = glGetUniformLocation(program, "uMinHeight");
    gShaderHandles.uniformHeightRange = glGetUniformLocation(program, "uHeightRange");
    gShaderHandles.uniformWaterLevel = glGetUniformLocation(program, "uWaterLevel");
    gShaderHandles.uniformLightDir = glGetUniformLocation(program, "uLightDir");
    gShaderHandles.uniformLightColor = glGetUniformLocation(program, "uLightColor");
    gShaderHandles.uniformAmbientColor = glGetUniformLocation(program, "uAmbientColor");
    gShaderHandles.uniformFogColor = glGetUniformLocation(program, "uFogColor");
    gShaderHandles.uniformFogStart = glGetUniformLocation(program, "uFogStart");
    gShaderHandles.uniformFogEnd = glGetUniformLocation(program, "uFogEnd");
    gShaderHandles.uniformPaletteWaterDeep = glGetUniformLocation(program, "uPaletteWaterDeep");
    gShaderHandles.uniformPaletteWaterShallow = glGetUniformLocation(program, "uPaletteWaterShallow");
    gShaderHandles.uniformPaletteLow = glGetUniformLocation(program, "uPaletteLow");
    gShaderHandles.uniformPaletteMid = glGetUniformLocation(program, "uPaletteMid");
    gShaderHandles.uniformPaletteHigh = glGetUniformLocation(program, "uPaletteHigh");
    gShaderHandles.uniformPalettePeak = glGetUniformLocation(program, "uPalettePeak");

    gShaderInitialised = true;
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

static void computeNormalMatrix(const float modelView[16], float out[9]) {
    float m00 = modelView[0];
    float m01 = modelView[4];
    float m02 = modelView[8];
    float m10 = modelView[1];
    float m11 = modelView[5];
    float m12 = modelView[9];
    float m20 = modelView[2];
    float m21 = modelView[6];
    float m22 = modelView[10];

    float det = m00 * (m11 * m22 - m12 * m21)
              - m01 * (m10 * m22 - m12 * m20)
              + m02 * (m10 * m21 - m11 * m20);
    if (fabsf(det) < 1e-6f) {
        out[0] = 1.0f; out[1] = 0.0f; out[2] = 0.0f;
        out[3] = 0.0f; out[4] = 1.0f; out[5] = 0.0f;
        out[6] = 0.0f; out[7] = 0.0f; out[8] = 1.0f;
        return;
    }

    float invDet = 1.0f / det;

    float i00 = (m11 * m22 - m12 * m21) * invDet;
    float i01 = (m02 * m21 - m01 * m22) * invDet;
    float i02 = (m01 * m12 - m02 * m11) * invDet;
    float i10 = (m12 * m20 - m10 * m22) * invDet;
    float i11 = (m00 * m22 - m02 * m20) * invDet;
    float i12 = (m02 * m10 - m00 * m12) * invDet;
    float i20 = (m10 * m21 - m11 * m20) * invDet;
    float i21 = (m01 * m20 - m00 * m21) * invDet;
    float i22 = (m00 * m11 - m01 * m10) * invDet;

    out[0] = i00;
    out[1] = i10;
    out[2] = i20;
    out[3] = i01;
    out[4] = i11;
    out[5] = i21;
    out[6] = i02;
    out[7] = i12;
    out[8] = i22;
}

const TerrainShaderHandles *terrainShaderBind(const TerrainGenerator *generator) {
    ensureStyleState();
    if (!generator) return NULL;
    if (!ensureShaderProgram()) {
        return NULL;
    }

    glUseProgram(gShaderHandles.program);

    float modelView[16];
    float projection[16];
#if defined(PSCAL_TARGET_IOS)
    terrainSetIdentity(modelView);
    terrainSetIdentity(projection);
#else
    glGetFloatv(GL_MODELVIEW_MATRIX, modelView);
    glGetFloatv(GL_PROJECTION_MATRIX, projection);
#endif

    float mvp[16];
    multiplyMat4(projection, modelView, mvp);

    float normalMatrix[9];
    computeNormalMatrix(modelView, normalMatrix);

    if (gShaderHandles.uniformModelView >= 0) {
        glUniformMatrix4fv(gShaderHandles.uniformModelView, 1, GL_FALSE, modelView);
    }
    if (gShaderHandles.uniformModelViewProjection >= 0) {
        glUniformMatrix4fv(gShaderHandles.uniformModelViewProjection, 1, GL_FALSE, mvp);
    }
    if (gShaderHandles.uniformNormalMatrix >= 0) {
        glUniformMatrix3fv(gShaderHandles.uniformNormalMatrix, 1, GL_FALSE, normalMatrix);
    }

    float minHeight = generator->minHeight;
    float maxHeight = generator->maxHeight;
    float heightRange = maxHeight - minHeight;
    if (fabsf(heightRange) < 1e-5f) heightRange = 1.0f;

    if (gShaderHandles.uniformMinHeight >= 0) {
        glUniform1f(gShaderHandles.uniformMinHeight, minHeight);
    }
    if (gShaderHandles.uniformHeightRange >= 0) {
        glUniform1f(gShaderHandles.uniformHeightRange, heightRange);
    }
    if (gShaderHandles.uniformWaterLevel >= 0) {
        glUniform1f(gShaderHandles.uniformWaterLevel, generator->waterLevel);
    }

    const TerrainLighting *lighting = &gCurrentLighting;
    float lightDir[3] = {lighting->lightDirection[0], lighting->lightDirection[1], lighting->lightDirection[2]};
    float lengthSq = lightDir[0] * lightDir[0] + lightDir[1] * lightDir[1] + lightDir[2] * lightDir[2];
    if (lengthSq < 1e-6f) {
        lightDir[0] = 0.0f;
        lightDir[1] = -1.0f;
        lightDir[2] = 0.0f;
    } else {
        float invLen = 1.0f / sqrtf(lengthSq);
        lightDir[0] *= invLen;
        lightDir[1] *= invLen;
        lightDir[2] *= invLen;
    }

    if (gShaderHandles.uniformLightDir >= 0) {
        glUniform3fv(gShaderHandles.uniformLightDir, 1, lightDir);
    }
    if (gShaderHandles.uniformLightColor >= 0) {
        glUniform3fv(gShaderHandles.uniformLightColor, 1, lighting->lightColor);
    }
    if (gShaderHandles.uniformAmbientColor >= 0) {
        glUniform3fv(gShaderHandles.uniformAmbientColor, 1, lighting->ambientColor);
    }
    if (gShaderHandles.uniformFogColor >= 0) {
        glUniform3fv(gShaderHandles.uniformFogColor, 1, lighting->fogColor);
    }
    if (gShaderHandles.uniformFogStart >= 0) {
        glUniform1f(gShaderHandles.uniformFogStart, lighting->fogStart);
    }
    if (gShaderHandles.uniformFogEnd >= 0) {
        glUniform1f(gShaderHandles.uniformFogEnd, lighting->fogEnd);
    }

    const TerrainPalette *palette = &gCurrentPalette;
    if (gShaderHandles.uniformPaletteWaterDeep >= 0) {
        glUniform3fv(gShaderHandles.uniformPaletteWaterDeep, 1, palette->waterDeep);
    }
    if (gShaderHandles.uniformPaletteWaterShallow >= 0) {
        glUniform3fv(gShaderHandles.uniformPaletteWaterShallow, 1, palette->waterShallow);
    }
    if (gShaderHandles.uniformPaletteLow >= 0) {
        glUniform3fv(gShaderHandles.uniformPaletteLow, 1, palette->low);
    }
    if (gShaderHandles.uniformPaletteMid >= 0) {
        glUniform3fv(gShaderHandles.uniformPaletteMid, 1, palette->mid);
    }
    if (gShaderHandles.uniformPaletteHigh >= 0) {
        glUniform3fv(gShaderHandles.uniformPaletteHigh, 1, palette->high);
    }
    if (gShaderHandles.uniformPalettePeak >= 0) {
        glUniform3fv(gShaderHandles.uniformPalettePeak, 1, palette->peak);
    }

    return &gShaderHandles;
}

void terrainShaderUnbind(void) {
    glUseProgram(0);
}

void terrainShaderShutdown(void) {
    if (gShaderHandles.program) {
        runtimeDestroyProgram(gShaderHandles.program);
        gShaderHandles.program = 0;
    }
    gShaderInitialised = false;
}

#endif // SDL
