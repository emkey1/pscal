#ifndef PSCAL_RUNTIME_SHADERS_TERRAIN_TERRAIN_SHADER_H
#define PSCAL_RUNTIME_SHADERS_TERRAIN_TERRAIN_SHADER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct TerrainPalette {
    float waterDeep[3];
    float waterShallow[3];
    float low[3];
    float mid[3];
    float high[3];
    float peak[3];
} TerrainPalette;

typedef struct TerrainLighting {
    float ambientColor[3];
    float lightColor[3];
    float lightDirection[3];
    float fogColor[3];
    float fogStart;
    float fogEnd;
    float skyHorizonColor[3];
    float skyZenithColor[3];
} TerrainLighting;

typedef enum TerrainPalettePreset {
    TERRAIN_PALETTE_TEMPERATE = 0,
    TERRAIN_PALETTE_DESERT,
    TERRAIN_PALETTE_ARCTIC,
    TERRAIN_PALETTE__COUNT
} TerrainPalettePreset;

typedef enum TerrainLightingPreset {
    TERRAIN_LIGHTING_NOON = 0,
    TERRAIN_LIGHTING_SUNSET,
    TERRAIN_LIGHTING_MIDNIGHT,
    TERRAIN_LIGHTING__COUNT
} TerrainLightingPreset;

void terrainShaderSetPalettePreset(TerrainPalettePreset preset);
bool terrainShaderPalettePresetFromName(const char *name, TerrainPalettePreset *outPreset);
TerrainPalettePreset terrainShaderCurrentPalettePreset(void);
size_t terrainShaderPalettePresetCount(void);
const char *terrainShaderPalettePresetLabel(size_t index);
const TerrainPalette *terrainShaderGetCurrentPalette(void);

void terrainShaderSetLightingPreset(TerrainLightingPreset preset);
bool terrainShaderLightingPresetFromName(const char *name, TerrainLightingPreset *outPreset);
TerrainLightingPreset terrainShaderCurrentLightingPreset(void);
size_t terrainShaderLightingPresetCount(void);
const char *terrainShaderLightingPresetLabel(size_t index);
const TerrainLighting *terrainShaderGetCurrentLighting(void);

void terrainShaderSampleGradient(float heightNormalized,
                                 float waterLevel,
                                 float slope,
                                 float outColor[3]);

const float *terrainShaderSkyHorizonColor(void);
const float *terrainShaderSkyZenithColor(void);

#ifdef SDL
#include "runtime/shaders/shader_common.h"

struct TerrainGenerator;

typedef struct TerrainShaderHandles {
    GLuint program;
    GLint attribPosition;
    GLint attribNormal;
    GLint attribColor;

    GLint uniformModelView;
    GLint uniformModelViewProjection;
    GLint uniformNormalMatrix;
    GLint uniformMinHeight;
    GLint uniformHeightRange;
    GLint uniformWaterLevel;
    GLint uniformLightDir;
    GLint uniformLightColor;
    GLint uniformAmbientColor;
    GLint uniformFogColor;
    GLint uniformFogStart;
    GLint uniformFogEnd;
    GLint uniformPaletteWaterDeep;
    GLint uniformPaletteWaterShallow;
    GLint uniformPaletteLow;
    GLint uniformPaletteMid;
    GLint uniformPaletteHigh;
    GLint uniformPalettePeak;
} TerrainShaderHandles;

const TerrainShaderHandles *terrainShaderBind(const struct TerrainGenerator *generator);
void terrainShaderUnbind(void);
void terrainShaderShutdown(void);
#endif

#endif // PSCAL_RUNTIME_SHADERS_TERRAIN_TERRAIN_SHADER_H
