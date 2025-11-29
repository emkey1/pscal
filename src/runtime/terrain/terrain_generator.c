#if defined(__APPLE__)
#ifndef GLES_SILENCE_DEPRECATION
#define GLES_SILENCE_DEPRECATION 1
#endif
#endif

#include "runtime/terrain/terrain_generator.h"
#include <math.h>

#if defined(__has_include)
  #if __has_include("noise/noise.h")
    #include "noise/noise.h"
    #define PSCAL_NOISE_EXTERNAL 1
  #else
    #define PSCAL_NOISE_EXTERNAL 0
  #endif
#else
  /* Conservative default if __has_include is unavailable */
  #define PSCAL_NOISE_EXTERNAL 0
#endif

#if !PSCAL_NOISE_EXTERNAL
/* Fallback lightweight noise implementations (deterministic, fast, not high quality) */
static inline uint32_t pscal_hash_u32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x;
}
static inline float pscal_hash01(uint32_t x) {
    return (pscal_hash_u32(x) >> 8) * (1.0f / 16777216.0f); /* [0,1) */
}
static inline float pscal_lerp(float a, float b, float t) { return a + (b - a) * t; }
static inline float pscal_fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

static float pscal_valueNoise2D(float x, float y, uint32_t seed) {
    int xi = (int)floorf(x);
    int yi = (int)floorf(y);
    float xf = x - (float)xi;
    float yf = y - (float)yi;
    uint32_t s = seed * 0x9E3779B9u;
    float v00 = pscal_hash01((uint32_t)xi * 374761393u ^ (uint32_t)yi * 668265263u ^ s);
    float v10 = pscal_hash01((uint32_t)(xi+1) * 374761393u ^ (uint32_t)yi * 668265263u ^ s);
    float v01 = pscal_hash01((uint32_t)xi * 374761393u ^ (uint32_t)(yi+1) * 668265263u ^ s);
    float v11 = pscal_hash01((uint32_t)(xi+1) * 374761393u ^ (uint32_t)(yi+1) * 668265263u ^ s);
    float u = pscal_fade(xf);
    float v = pscal_fade(yf);
    float x1 = pscal_lerp(v00, v10, u);
    float x2 = pscal_lerp(v01, v11, u);
    return pscal_lerp(x1, x2, v) * 2.0f - 1.0f; /* map to [-1,1] */
}

/* Public fallbacks matching expected signatures */
static inline float pscalPerlin2D(float x, float y, uint32_t seed) {
    /* Use value noise with fade as a simple Perlin-like fallback */
    return pscal_valueNoise2D(x, y, seed);
}

static inline float pscalSimplex2D(float x, float y, uint32_t seed) {
    /* Simple alternate by rotating coordinates */
    float xr = x * 0.70710678f - y * 0.70710678f; /* rotate 45 degrees */
    float yr = x * 0.70710678f + y * 0.70710678f;
    return pscal_valueNoise2D(xr, yr, seed ^ 0xA5A5A5A5u);
}
#endif /* !PSCAL_NOISE_EXTERNAL */

#include "runtime/shaders/terrain/terrain_shader.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float clampf(float v, float minVal, float maxVal) {
    if (v < minVal) return minVal;
    if (v > maxVal) return maxVal;
    return v;
}

static float saturatef(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static bool ensureCapacity(TerrainGenerator *generator, size_t vertexCount, size_t indexCount) {
    if (!generator) return false;
    if (vertexCount > generator->vertexCapacity) {
        TerrainVertex *newVertices = (TerrainVertex *)realloc(generator->vertices,
                                                              vertexCount * sizeof(TerrainVertex));
        if (!newVertices) return false;
        generator->vertices = newVertices;
        generator->vertexCapacity = vertexCount;
    }
    if (indexCount > generator->indexCapacity) {
        unsigned int *newIndices = (unsigned int *)realloc(generator->indices,
                                                           indexCount * sizeof(unsigned int));
        if (!newIndices) return false;
        generator->indices = newIndices;
        generator->indexCapacity = indexCount;
    }
    return true;
}

void terrainGeneratorInit(TerrainGenerator *generator) {
    if (!generator) return;
    memset(generator, 0, sizeof(*generator));
    generator->config.amplitude = 1.0f;
    generator->config.frequency = 0.015f;
    generator->config.octaves = 4;
    generator->config.lacunarity = 2.0f;
    generator->config.persistence = 0.5f;
}

void terrainGeneratorFree(TerrainGenerator *generator) {
    if (!generator) return;
    free(generator->vertices);
    free(generator->indices);
    generator->vertices = NULL;
    generator->indices = NULL;
    generator->vertexCapacity = 0;
    generator->indexCapacity = 0;
    generator->vertexCount = 0;
    generator->indexCount = 0;
#ifdef SDL
    if (generator->vbo) {
        glDeleteBuffers(1, &generator->vbo);
        generator->vbo = 0;
    }
    if (generator->ibo) {
        glDeleteBuffers(1, &generator->ibo);
        generator->ibo = 0;
    }
    generator->gpuVertexCapacity = 0;
    generator->gpuIndexCapacity = 0;
#endif
}

static float sampleNoise(const TerrainGeneratorConfig *config, float x, float z) {
    if (!config) return 0.0f;
    float sum = 0.0f;
    float amplitude = 1.0f;
    float frequency = config->frequency <= 0.0f ? 0.01f : config->frequency;
    float totalAmplitude = 0.0f;
    uint32_t seed = config->seed;
    for (int octave = 0; octave < config->octaves; ++octave) {
        float sampleX = (x + config->offsetX) * frequency;
        float sampleZ = (z + config->offsetZ) * frequency;
        float noise = config->useSimplex ? pscalSimplex2D(sampleX, sampleZ, seed)
                                         : pscalPerlin2D(sampleX, sampleZ, seed);
        sum += noise * amplitude;
        totalAmplitude += amplitude;
        amplitude *= (config->persistence <= 0.0f ? 0.5f : config->persistence);
        frequency *= (config->lacunarity <= 0.0f ? 2.0f : config->lacunarity);
        seed += 97u;
    }
    if (totalAmplitude <= 0.0001f) totalAmplitude = 1.0f;
    return sum / totalAmplitude;
}

static void computeNormals(TerrainGenerator *generator) {
    if (!generator || !generator->vertices) return;
    int stride = generator->resolution + 1;
    float invScale = generator->tileScale;
    float denom = invScale * 2.0f;
    if (fabsf(denom) < 1e-6f) denom = 1.0f;
    for (int z = 0; z <= generator->resolution; ++z) {
        for (int x = 0; x <= generator->resolution; ++x) {
            int idx = z * stride + x;
            TerrainVertex *v = &generator->vertices[idx];
            float left = generator->vertices[(z * stride) + (x > 0 ? x - 1 : x)].position[1];
            float right = generator->vertices[(z * stride) + (x < generator->resolution ? x + 1 : x)].position[1];
            float down = generator->vertices[((z > 0 ? z - 1 : z) * stride) + x].position[1];
            float up = generator->vertices[((z < generator->resolution ? z + 1 : z) * stride) + x].position[1];
            float dx = (right - left) / denom;
            float dz = (up - down) / denom;
            float nx = -dx;
            float ny = 1.0f;
            float nz = -dz;
            float len = sqrtf(nx * nx + ny * ny + nz * nz);
            if (len <= 0.0001f) len = 1.0f;
            v->normal[0] = nx / len;
            v->normal[1] = ny / len;
            v->normal[2] = nz / len;
        }
    }
}

static void computeColors(TerrainGenerator *generator) {
    if (!generator || !generator->vertices) return;
    float minH = generator->minHeight;
    float maxH = generator->maxHeight;
    float span = maxH - minH;
    if (span <= 0.0001f) span = 1.0f;
    for (size_t i = 0; i < generator->vertexCount; ++i) {
        TerrainVertex *v = &generator->vertices[i];
        float height = v->position[1];
        float normalized = (height - minH) / span;
        normalized = clampf(normalized, 0.0f, 1.0f);
        float slope = 1.0f - v->normal[1];
        slope = clampf(slope, 0.0f, 1.0f);
        float color[3];
        terrainShaderSampleGradient(normalized, generator->waterLevel, slope, color);
        v->color[0] = saturatef(color[0]);
        v->color[1] = saturatef(color[1]);
        v->color[2] = saturatef(color[2]);
    }
}

static void buildIndices(TerrainGenerator *generator) {
    if (!generator || !generator->indices) return;
    int stride = generator->resolution + 1;
    size_t idx = 0;
    for (int z = 0; z < generator->resolution; ++z) {
        for (int x = 0; x < generator->resolution; ++x) {
            unsigned int i0 = (unsigned int)(z * stride + x);
            unsigned int i1 = (unsigned int)(z * stride + x + 1);
            unsigned int i2 = (unsigned int)((z + 1) * stride + x);
            unsigned int i3 = (unsigned int)((z + 1) * stride + x + 1);
            generator->indices[idx++] = i0;
            generator->indices[idx++] = i2;
            generator->indices[idx++] = i1;
            generator->indices[idx++] = i1;
            generator->indices[idx++] = i2;
            generator->indices[idx++] = i3;
        }
    }
}

bool terrainGeneratorGenerate(TerrainGenerator *generator,
                              int resolution,
                              float minHeight,
                              float maxHeight,
                              float waterLevel,
                              float tileScale,
                              const TerrainGeneratorConfig *config) {
    if (!generator || resolution < 1) return false;
    if (!config) return false;

    int stride = resolution + 1;
    size_t vertexCount = (size_t)stride * (size_t)stride;
    size_t indexCount = (size_t)resolution * (size_t)resolution * 6u;
    if (!ensureCapacity(generator, vertexCount, indexCount)) {
        return false;
    }

    generator->config = *config;
    generator->resolution = resolution;
    generator->minHeight = minHeight;
    generator->maxHeight = maxHeight;
    generator->waterLevel = clampf(waterLevel, 0.0f, 1.0f);
    generator->tileScale = tileScale;
    generator->vertexCount = vertexCount;
    generator->indexCount = indexCount;

    float span = maxHeight - minHeight;
    if (span <= 0.0001f) span = 1.0f;
    float amplitude = config->amplitude;
    if (fabsf(amplitude) < 1e-6f) amplitude = 1.0f;

    float half = resolution * 0.5f;
    for (int z = 0; z <= resolution; ++z) {
        for (int x = 0; x <= resolution; ++x) {
            int idx = z * stride + x;
            TerrainVertex *v = &generator->vertices[idx];
            float heightNoise = sampleNoise(config, (float)x, (float)z);
            float normalized = heightNoise * 0.5f + 0.5f;
            normalized = clampf(normalized, 0.0f, 1.0f);
            float height = minHeight + normalized * span * amplitude;
            if (height < minHeight) height = minHeight;
            if (height > maxHeight) height = maxHeight;
            v->position[0] = (x - half) * tileScale;
            v->position[1] = height;
            v->position[2] = (z - half) * tileScale;
            v->uv[0] = (float)x / (float)resolution;
            v->uv[1] = (float)z / (float)resolution;
            v->normal[0] = 0.0f;
            v->normal[1] = 1.0f;
            v->normal[2] = 0.0f;
            v->color[0] = v->color[1] = v->color[2] = 1.0f;
        }
    }

    computeNormals(generator);
    computeColors(generator);
    buildIndices(generator);
    generator->gpuDirty = true;
    return true;
}

const TerrainVertex *terrainGeneratorVertices(const TerrainGenerator *generator) {
    if (!generator) return NULL;
    return generator->vertices;
}

size_t terrainGeneratorVertexCount(const TerrainGenerator *generator) {
    if (!generator) return 0;
    return generator->vertexCount;
}

bool terrainGeneratorCopyHeights(const TerrainGenerator *generator, float *out, size_t count) {
    if (!generator || !out) return false;
    if (count < generator->vertexCount) return false;
    for (size_t i = 0; i < generator->vertexCount; ++i) {
        out[i] = generator->vertices[i].position[1];
    }
    return true;
}

bool terrainGeneratorCopyNormals(const TerrainGenerator *generator,
                                 float *nx,
                                 float *ny,
                                 float *nz,
                                 size_t count) {
    if (!generator || !nx || !ny || !nz) return false;
    if (count < generator->vertexCount) return false;
    for (size_t i = 0; i < generator->vertexCount; ++i) {
        const TerrainVertex *v = &generator->vertices[i];
        nx[i] = v->normal[0];
        ny[i] = v->normal[1];
        nz[i] = v->normal[2];
    }
    return true;
}

bool terrainGeneratorCopyColors(const TerrainGenerator *generator,
                                float *r,
                                float *g,
                                float *b,
                                size_t count) {
    if (!generator || !r || !g || !b) return false;
    if (count < generator->vertexCount) return false;
    for (size_t i = 0; i < generator->vertexCount; ++i) {
        const TerrainVertex *v = &generator->vertices[i];
        r[i] = v->color[0];
        g[i] = v->color[1];
        b[i] = v->color[2];
    }
    return true;
}

bool terrainGeneratorCopyUVs(const TerrainGenerator *generator, float *u, float *v, size_t count) {
    if (!generator || !u || !v) return false;
    if (count < generator->vertexCount) return false;
    for (size_t i = 0; i < generator->vertexCount; ++i) {
        u[i] = generator->vertices[i].uv[0];
        v[i] = generator->vertices[i].uv[1];
    }
    return true;
}

#ifdef SDL
#include <stddef.h>

bool terrainGeneratorEnsureUploaded(TerrainGenerator *generator) {
    if (!generator || !generator->vertices || generator->vertexCount == 0) {
        return false;
    }
    if (!generator->vbo) {
        glGenBuffers(1, &generator->vbo);
        generator->gpuVertexCapacity = 0;
    }
    if (!generator->ibo) {
        glGenBuffers(1, &generator->ibo);
        generator->gpuIndexCapacity = 0;
    }

    size_t vertexBytes = generator->vertexCount * sizeof(TerrainVertex);
    size_t indexBytes = generator->indexCount * sizeof(unsigned int);

    glBindBuffer(GL_ARRAY_BUFFER, generator->vbo);
    if (generator->gpuDirty || generator->gpuVertexCapacity < vertexBytes) {
        glBufferData(GL_ARRAY_BUFFER, vertexBytes, generator->vertices, GL_DYNAMIC_DRAW);
        generator->gpuVertexCapacity = vertexBytes;
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, vertexBytes, generator->vertices);
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, generator->ibo);
    if (generator->gpuDirty || generator->gpuIndexCapacity < indexBytes) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indexBytes, generator->indices, GL_DYNAMIC_DRAW);
        generator->gpuIndexCapacity = indexBytes;
    } else {
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, indexBytes, generator->indices);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    generator->gpuDirty = false;
    return true;
}

void terrainGeneratorDraw(const TerrainGenerator *generator) {
    if (!generator || !generator->vbo || !generator->ibo) return;
    glBindBuffer(GL_ARRAY_BUFFER, generator->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, generator->ibo);

    const TerrainShaderHandles *shader = terrainShaderBind(generator);
    bool usedShader = shader && shader->program;
    if (usedShader) {
        if (shader->attribPosition >= 0) {
            glEnableVertexAttribArray((GLuint)shader->attribPosition);
            glVertexAttribPointer((GLuint)shader->attribPosition,
                                  3,
                                  GL_FLOAT,
                                  GL_FALSE,
                                  sizeof(TerrainVertex),
                                  (const void *)offsetof(TerrainVertex, position));
        }
        if (shader->attribNormal >= 0) {
            glEnableVertexAttribArray((GLuint)shader->attribNormal);
            glVertexAttribPointer((GLuint)shader->attribNormal,
                                  3,
                                  GL_FLOAT,
                                  GL_FALSE,
                                  sizeof(TerrainVertex),
                                  (const void *)offsetof(TerrainVertex, normal));
        }
        if (shader->attribColor >= 0) {
            glEnableVertexAttribArray((GLuint)shader->attribColor);
            glVertexAttribPointer((GLuint)shader->attribColor,
                                  3,
                                  GL_FLOAT,
                                  GL_FALSE,
                                  sizeof(TerrainVertex),
                                  (const void *)offsetof(TerrainVertex, color));
        }

        glDrawElements(GL_TRIANGLES, (GLsizei)generator->indexCount, GL_UNSIGNED_INT, (const void *)0);

        if (shader->attribColor >= 0) {
            glDisableVertexAttribArray((GLuint)shader->attribColor);
        }
        if (shader->attribNormal >= 0) {
            glDisableVertexAttribArray((GLuint)shader->attribNormal);
        }
        if (shader->attribPosition >= 0) {
            glDisableVertexAttribArray((GLuint)shader->attribPosition);
        }

        terrainShaderUnbind();
    } else {
#if defined(PSCAL_TARGET_IOS)
        /* Fixed-function fallback path is unavailable on OpenGLES/iOS.
         * Terrain rendering requires shader support on this platform. */
#else
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(3, GL_FLOAT, sizeof(TerrainVertex), (const void *)offsetof(TerrainVertex, position));

        glEnableClientState(GL_NORMAL_ARRAY);
        glNormalPointer(GL_FLOAT, sizeof(TerrainVertex), (const void *)offsetof(TerrainVertex, normal));

        glClientActiveTexture(GL_TEXTURE0);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, sizeof(TerrainVertex), (const void *)offsetof(TerrainVertex, uv));

        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(3, GL_FLOAT, sizeof(TerrainVertex), (const void *)offsetof(TerrainVertex, color));

        glDrawElements(GL_TRIANGLES, (GLsizei)generator->indexCount, GL_UNSIGNED_INT, (const void *)0);

        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_NORMAL_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
#endif
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}
#endif
