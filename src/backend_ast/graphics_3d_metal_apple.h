#ifndef PSCAL_GRAPHICS_3D_METAL_APPLE_H
#define PSCAL_GRAPHICS_3D_METAL_APPLE_H

#include <stdbool.h>
#include <stddef.h>

struct SDL_Renderer;

typedef struct {
    float clipX;
    float clipY;
    float depth;
    float r;
    float g;
    float b;
    float a;
} PscalMetalVertex;

bool pscalMetal3DIsSupported(void);
bool pscalMetal3DEnsureRenderer(struct SDL_Renderer* renderer);
void pscalMetal3DSetViewport(int x, int y, int width, int height);
bool pscalMetal3DBeginFrame(bool clearColor, const float clearColorRgba[4],
                            bool clearDepth, float clearDepthValue);
bool pscalMetal3DDrawTriangles(const PscalMetalVertex* vertices, size_t vertexCount,
                               bool depthTestEnabled, bool depthWriteEnabled, unsigned int depthFunc,
                               bool blendEnabled, unsigned int blendSrc, unsigned int blendDst);
bool pscalMetal3DDrawLines(const PscalMetalVertex* vertices, size_t vertexCount,
                           bool depthTestEnabled, bool depthWriteEnabled, unsigned int depthFunc,
                           bool blendEnabled, unsigned int blendSrc, unsigned int blendDst);
void pscalMetal3DPresent(void);
void pscalMetal3DShutdown(void);

#endif
