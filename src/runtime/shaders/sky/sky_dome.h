#ifndef PSCAL_RUNTIME_SHADERS_SKY_SKY_DOME_H
#define PSCAL_RUNTIME_SHADERS_SKY_SKY_DOME_H

#include <stdbool.h>
#include <stddef.h>

#ifdef SDL
#include "runtime/shaders/shader_common.h"
#endif

typedef struct SkyDome {
#ifdef SDL
    GLuint vbo;
    GLuint ibo;
    size_t indexCount;
    int slices;
    int stacks;
    bool gpuReady;
#endif
} SkyDome;

#ifdef SDL

void skyDomeInit(SkyDome *dome);
void skyDomeFree(SkyDome *dome);
bool skyDomeEnsureUploaded(SkyDome *dome, int slices, int stacks);
void skyDomeDraw(SkyDome *dome, float radius, const float horizonColor[3], const float zenithColor[3]);
#endif

#endif // PSCAL_RUNTIME_SHADERS_SKY_SKY_DOME_H
