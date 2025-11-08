#ifndef PSCAL_RUNTIME_CLOUD_LAYER_H
#define PSCAL_RUNTIME_CLOUD_LAYER_H

#ifdef SDL

#include "runtime/shaders/shader_common.h"
#include <stdbool.h>

typedef struct CloudLayerRenderer CloudLayerRenderer;

typedef struct CloudLayerParams {
    float timeSeconds;
    float cameraOffsetX;
    float cameraOffsetZ;
    float parallaxScale;
    float coverage;
    float softness;
    float dayFactor;
    float sunDirection[3];
} CloudLayerParams;

void cloudLayerRendererInit(CloudLayerRenderer **renderer);
void cloudLayerRendererShutdown(CloudLayerRenderer **renderer);
bool cloudLayerRendererDraw(CloudLayerRenderer **renderer, const CloudLayerParams *params);

#endif // SDL

#endif // PSCAL_RUNTIME_CLOUD_LAYER_H
