#include "runtime/terrain/terrain_generator.h"

#include "noise/noise.h"

#include <math.h>
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

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
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
    float waterLevel = generator->waterLevel;
    float span = maxH - minH;
    if (span <= 0.0001f) span = 1.0f;
    for (size_t i = 0; i < generator->vertexCount; ++i) {
        TerrainVertex *v = &generator->vertices[i];
        float height = v->position[1];
        float normalized = (height - minH) / span;
        normalized = clampf(normalized, 0.0f, 1.0f);
        float r, g, b;
        bool underwater = normalized < waterLevel;
        if (underwater) {
            float denom = waterLevel <= 1e-6f ? 1.0f : waterLevel;
            float depth = (waterLevel - normalized) / denom;
            depth = clampf(depth, 0.0f, 1.0f);
            float shore = 1.0f - depth;
            r = 0.05f + 0.08f * depth + 0.10f * shore;
            g = 0.32f + 0.36f * depth + 0.18f * shore;
            b = 0.52f + 0.40f * depth + 0.12f * shore;
        } else if (normalized < waterLevel + 0.06f) {
            float w = (normalized - waterLevel) / 0.06f;
            r = 0.36f + 0.14f * w;
            g = 0.34f + 0.20f * w;
            b = 0.20f + 0.09f * w;
        } else if (normalized < 0.62f) {
            float w = (normalized - (waterLevel + 0.06f)) / 0.16f;
            r = 0.24f + 0.18f * w;
            g = 0.46f + 0.32f * w;
            b = 0.22f + 0.12f * w;
        } else if (normalized < 0.82f) {
            float w = (normalized - 0.62f) / 0.20f;
            r = 0.46f + 0.26f * w;
            g = 0.40f + 0.22f * w;
            b = 0.30f + 0.20f * w;
        } else {
            float w = (normalized - 0.82f) / 0.18f;
            w = clampf(w, 0.0f, 1.0f);
            float base = 0.84f + 0.14f * w;
            r = base;
            g = base;
            b = base;
            float frost = saturatef((normalized - 0.88f) / 0.12f);
            float sunSpark = 0.75f + 0.25f * frost;
            r = lerpf(r, sunSpark, frost * 0.4f);
            g = lerpf(g, sunSpark, frost * 0.4f);
            b = lerpf(b, sunSpark, frost * 0.6f);
        }
        if (!underwater) {
            float slope = 1.0f - v->normal[1];
            slope = clampf(slope, 0.0f, 1.0f);
            float cool = saturatef((0.58f - normalized) * 3.5f);
            g += cool * 0.04f;
            b += cool * 0.06f;
            float alpine = saturatef((normalized - 0.68f) * 2.2f);
            r = lerpf(r, r * 0.92f, alpine * 0.3f);
            g = lerpf(g, g * 0.90f, alpine * 0.26f);
            b = lerpf(b, b * 1.05f, alpine * 0.24f);
            float slopeTint = slope * 0.6f;
            r = lerpf(r, r * 0.78f, slopeTint);
            g = lerpf(g, g * 0.74f, slopeTint);
            b = lerpf(b, b * 0.86f, slopeTint);
        }
        v->color[0] = saturatef(r);
        v->color[1] = saturatef(g);
        v->color[2] = saturatef(b);
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

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}
#endif
