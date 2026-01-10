#ifndef PSCAL_RUNTIME_TERRAIN_GENERATOR_H
#define PSCAL_RUNTIME_TERRAIN_GENERATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef SDL
#include "core/sdl_headers.h"
#if defined(PSCAL_TARGET_IOS)
#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>
#else
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include PSCALI_SDL_OPENGL_HEADER
#if defined(__has_include)
#if __has_include(PSCALI_SDL_OPENGL_GLEXT_HEADER)
#include PSCALI_SDL_OPENGL_GLEXT_HEADER
#endif
#endif
#endif
#endif

typedef struct TerrainGeneratorConfig {
    uint32_t seed;
    float amplitude;
    float frequency;
    int octaves;
    float lacunarity;
    float persistence;
    float offsetX;
    float offsetZ;
    bool useSimplex;
} TerrainGeneratorConfig;

typedef struct TerrainVertex {
    float position[3];
    float normal[3];
    float uv[2];
    float color[3];
} TerrainVertex;

typedef struct TerrainGenerator {
    TerrainGeneratorConfig config;
    int resolution;
    float minHeight;
    float maxHeight;
    float waterLevel;
    float tileScale;
    TerrainVertex *vertices;
    unsigned int *indices;
    size_t vertexCount;
    size_t indexCount;
    size_t vertexCapacity;
    size_t indexCapacity;
#ifdef SDL
    GLuint vbo;
    GLuint ibo;
    size_t gpuVertexCapacity;
    size_t gpuIndexCapacity;
#endif
    bool gpuDirty;
} TerrainGenerator;

void terrainGeneratorInit(TerrainGenerator *generator);
void terrainGeneratorFree(TerrainGenerator *generator);

bool terrainGeneratorGenerate(TerrainGenerator *generator,
                              int resolution,
                              float minHeight,
                              float maxHeight,
                              float waterLevel,
                              float tileScale,
                              const TerrainGeneratorConfig *config);

const TerrainVertex *terrainGeneratorVertices(const TerrainGenerator *generator);
size_t terrainGeneratorVertexCount(const TerrainGenerator *generator);

bool terrainGeneratorCopyHeights(const TerrainGenerator *generator, float *out, size_t count);
bool terrainGeneratorCopyNormals(const TerrainGenerator *generator,
                                 float *nx,
                                 float *ny,
                                 float *nz,
                                 size_t count);
bool terrainGeneratorCopyColors(const TerrainGenerator *generator,
                                float *r,
                                float *g,
                                float *b,
                                size_t count);
bool terrainGeneratorCopyUVs(const TerrainGenerator *generator, float *u, float *v, size_t count);

#ifdef SDL
bool terrainGeneratorEnsureUploaded(TerrainGenerator *generator);
void terrainGeneratorDraw(const TerrainGenerator *generator);
#endif

#endif // PSCAL_RUNTIME_TERRAIN_GENERATOR_H
