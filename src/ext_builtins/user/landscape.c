#include "backend_ast/builtin.h"
#include "backend_ast/sdl.h"
#include "core/utils.h"
#include "vm/vm.h"

#include <math.h>

#ifdef SDL
#include <SDL2/SDL_opengl.h>
#endif

static Value* resolveArrayArg(VM* vm, Value* arg, const char* name, int* lower, int* upper) {
    Value* arrVal = arg;
    if (arg->type == TYPE_POINTER) {
        arrVal = (Value*)arg->ptr_val;
        if (!arrVal) {
            runtimeError(vm, "%s received a NIL pointer.", name);
            return NULL;
        }
    }
    if (!arrVal || arrVal->type != TYPE_ARRAY) {
        runtimeError(vm, "%s expects VAR array arguments.", name);
        return NULL;
    }
    if (arrVal->dimensions > 1) {
        runtimeError(vm, "%s arrays must be single dimensional.", name);
        return NULL;
    }
    int l = (arrVal->dimensions > 0 && arrVal->lower_bounds) ? arrVal->lower_bounds[0] : arrVal->lower_bound;
    int u = (arrVal->dimensions > 0 && arrVal->upper_bounds) ? arrVal->upper_bounds[0] : arrVal->upper_bound;
    if (lower) *lower = l;
    if (upper) *upper = u;
    if (!arrVal->array_val) {
        runtimeError(vm, "%s received an array with NIL storage.", name);
        return NULL;
    }
    return arrVal->array_val;
}

static inline void assignFloatValue(Value* target, double value) {
    if (!target) return;
    target->type = TYPE_FLOAT;
    SET_REAL_VALUE(target, value);
}

#ifdef SDL
static void computeTerrainNormal(Value* vertexHeights,
                                 Value* worldXCoords,
                                 Value* worldZCoords,
                                 int vertexStride,
                                 int terrainSize,
                                 int x,
                                 int z,
                                 float* nx,
                                 float* ny,
                                 float* nz) {
    int leftX = (x > 0) ? x - 1 : x;
    int rightX = (x < terrainSize) ? x + 1 : x;
    int downZ = (z > 0) ? z - 1 : z;
    int upZ = (z < terrainSize) ? z + 1 : z;

    float left = (float)asLd(vertexHeights[z * vertexStride + leftX]);
    float right = (float)asLd(vertexHeights[z * vertexStride + rightX]);
    float down = (float)asLd(vertexHeights[downZ * vertexStride + x]);
    float up = (float)asLd(vertexHeights[upZ * vertexStride + x]);

    float worldLeft = (float)asLd(worldXCoords[leftX]);
    float worldRight = (float)asLd(worldXCoords[rightX]);
    float worldDown = (float)asLd(worldZCoords[downZ]);
    float worldUp = (float)asLd(worldZCoords[upZ]);

    float dx = 0.0f;
    float spanX = worldRight - worldLeft;
    if (fabsf(spanX) > 1e-6f) {
        dx = (right - left) / spanX;
    }

    float dz = 0.0f;
    float spanZ = worldUp - worldDown;
    if (fabsf(spanZ) > 1e-6f) {
        dz = (up - down) / spanZ;
    }

    float nxTemp = -dx;
    float nyTemp = 1.0f;
    float nzTemp = -dz;
    float length = sqrtf(nxTemp * nxTemp + nyTemp * nyTemp + nzTemp * nzTemp);
    if (length <= 1e-6f) {
        length = 1.0f;
    }

    if (nx) *nx = nxTemp / length;
    if (ny) *ny = nyTemp / length;
    if (nz) *nz = nzTemp / length;
}
#endif

static Value vmBuiltinLandscapePrecomputeWorldCoords(VM* vm, int arg_count, Value* args) {
    if (arg_count != 5) {
        runtimeError(vm, "LandscapePrecomputeWorldCoords expects 5 arguments.");
        return makeVoid();
    }

    int lower = 0, upper = 0;
    Value* worldXCoords = resolveArrayArg(vm, &args[0], "LandscapePrecomputeWorldCoords", &lower, &upper);
    if (!worldXCoords) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "LandscapePrecomputeWorldCoords requires coordinate arrays starting at index 0.");
        return makeVoid();
    }
    int coordUpper = upper;

    Value* worldZCoords = resolveArrayArg(vm, &args[1], "LandscapePrecomputeWorldCoords", &lower, &upper);
    if (!worldZCoords) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "LandscapePrecomputeWorldCoords requires coordinate arrays starting at index 0.");
        return makeVoid();
    }
    if (upper < coordUpper) coordUpper = upper;

    if (!isRealType(args[2].type) && !IS_INTLIKE(args[2])) {
        runtimeError(vm, "LandscapePrecomputeWorldCoords expects numeric tile scale argument.");
        return makeVoid();
    }
    if (!IS_INTLIKE(args[3]) || !IS_INTLIKE(args[4])) {
        runtimeError(vm, "LandscapePrecomputeWorldCoords expects integer terrain parameters.");
        return makeVoid();
    }

    double tileScale = (double)asLd(args[2]);
    int terrainSize = (int)asI64(args[3]);
    int vertexStride = (int)asI64(args[4]);

    if (terrainSize < 1 || vertexStride < 2 || vertexStride != terrainSize + 1) {
        runtimeError(vm, "LandscapePrecomputeWorldCoords received inconsistent terrain parameters.");
        return makeVoid();
    }

    if (coordUpper < vertexStride - 1) {
        runtimeError(vm, "LandscapePrecomputeWorldCoords arrays are smaller than the required vertex stride.");
        return makeVoid();
    }

    double half = terrainSize * 0.5;
    for (int i = 0; i < vertexStride; ++i) {
        double world = (i - half) * tileScale;
        assignFloatValue(&worldXCoords[i], world);
        assignFloatValue(&worldZCoords[i], world);
    }

    return makeVoid();
}

static Value vmBuiltinLandscapePrecomputeWaterOffsets(VM* vm, int arg_count, Value* args) {
    if (arg_count != 5) {
        runtimeError(vm, "LandscapePrecomputeWaterOffsets expects 5 arguments.");
        return makeVoid();
    }

    int lower = 0, upper = 0;
    Value* waterPhaseOffset = resolveArrayArg(vm, &args[0], "LandscapePrecomputeWaterOffsets", &lower, &upper);
    if (!waterPhaseOffset) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "LandscapePrecomputeWaterOffsets requires offset arrays starting at index 0.");
        return makeVoid();
    }
    int arrayUpper = upper;

    Value* waterSecondaryOffset = resolveArrayArg(vm, &args[1], "LandscapePrecomputeWaterOffsets", &lower, &upper);
    if (!waterSecondaryOffset) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "LandscapePrecomputeWaterOffsets requires offset arrays starting at index 0.");
        return makeVoid();
    }
    if (upper < arrayUpper) arrayUpper = upper;

    Value* waterSparkleOffset = resolveArrayArg(vm, &args[2], "LandscapePrecomputeWaterOffsets", &lower, &upper);
    if (!waterSparkleOffset) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "LandscapePrecomputeWaterOffsets requires offset arrays starting at index 0.");
        return makeVoid();
    }
    if (upper < arrayUpper) arrayUpper = upper;

    if (!IS_INTLIKE(args[3]) || !IS_INTLIKE(args[4])) {
        runtimeError(vm, "LandscapePrecomputeWaterOffsets expects integer terrain parameters.");
        return makeVoid();
    }

    int terrainSize = (int)asI64(args[3]);
    int vertexStride = (int)asI64(args[4]);

    if (terrainSize < 1 || vertexStride < 2 || vertexStride != terrainSize + 1) {
        runtimeError(vm, "LandscapePrecomputeWaterOffsets received inconsistent terrain parameters.");
        return makeVoid();
    }

    int vertexCount = vertexStride * vertexStride;
    if (arrayUpper < vertexCount - 1) {
        runtimeError(vm, "LandscapePrecomputeWaterOffsets arrays are smaller than the required vertex count.");
        return makeVoid();
    }

    for (int z = 0; z <= terrainSize; ++z) {
        int rowIndex = z * vertexStride;
        double zPhase = z * 0.12;
        double zSecondary = z * 0.21;
        double zSparkle = z * 0.22;
        for (int x = 0; x <= terrainSize; ++x) {
            int idx = rowIndex + x;
            assignFloatValue(&waterPhaseOffset[idx], x * 0.18 + zPhase);
            assignFloatValue(&waterSecondaryOffset[idx], x * 0.05 + zSecondary);
            assignFloatValue(&waterSparkleOffset[idx], x * 0.22 + zSparkle);
        }
    }

    return makeVoid();
}

#ifdef SDL
static bool ensureGlContext(VM* vm, const char* name) {
    if (!gSdlInitialized || !gSdlWindow || !gSdlGLContext) {
        runtimeError(vm, "%s requires an active OpenGL window. Call InitGraph3D first.", name);
        return false;
    }
    return true;
}

static float clampf(float v, float minVal, float maxVal) {
    if (v < minVal) return minVal;
    if (v > maxVal) return maxVal;
    return v;
}

static Value vmBuiltinLandscapeDrawTerrain(VM* vm, int arg_count, Value* args) {
    if (arg_count != 8) {
        runtimeError(vm, "LandscapeDrawTerrain expects 8 arguments.");
        return makeVoid();
    }

    int lower = 0, upper = 0;
    Value* vertexHeights = resolveArrayArg(vm, &args[0], "LandscapeDrawTerrain", &lower, &upper);
    if (!vertexHeights) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "LandscapeDrawTerrain requires vertex height arrays starting at index 0.");
        return makeVoid();
    }
    int heightsUpper = upper;

    Value* vertexColorR = resolveArrayArg(vm, &args[1], "LandscapeDrawTerrain", &lower, &upper);
    if (!vertexColorR) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "LandscapeDrawTerrain requires vertex color arrays starting at index 0.");
        return makeVoid();
    }
    int colorUpper = upper;

    Value* vertexColorG = resolveArrayArg(vm, &args[2], "LandscapeDrawTerrain", NULL, &upper);
    if (!vertexColorG) return makeVoid();
    if (upper < colorUpper) colorUpper = upper;

    Value* vertexColorB = resolveArrayArg(vm, &args[3], "LandscapeDrawTerrain", NULL, &upper);
    if (!vertexColorB) return makeVoid();
    if (upper < colorUpper) colorUpper = upper;

    Value* worldXCoords = resolveArrayArg(vm, &args[4], "LandscapeDrawTerrain", &lower, &upper);
    if (!worldXCoords) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "LandscapeDrawTerrain requires coordinate arrays starting at index 0.");
        return makeVoid();
    }
    int worldUpper = upper;

    Value* worldZCoords = resolveArrayArg(vm, &args[5], "LandscapeDrawTerrain", NULL, &upper);
    if (!worldZCoords) return makeVoid();
    if (upper < worldUpper) worldUpper = upper;

    if (!IS_INTLIKE(args[6]) || !IS_INTLIKE(args[7])) {
        runtimeError(vm, "LandscapeDrawTerrain expects integer TerrainSize and VertexStride arguments.");
        return makeVoid();
    }
    int terrainSize = (int)asI64(args[6]);
    int vertexStride = (int)asI64(args[7]);
    if (terrainSize < 1 || vertexStride < 2 || vertexStride != terrainSize + 1) {
        runtimeError(vm, "LandscapeDrawTerrain received inconsistent terrain parameters.");
        return makeVoid();
    }

    int vertexCount = vertexStride * vertexStride;
    if (heightsUpper < vertexCount - 1 || colorUpper < vertexCount - 1) {
        runtimeError(vm, "LandscapeDrawTerrain vertex arrays are smaller than the required vertex count.");
        return makeVoid();
    }
    if (worldUpper < vertexStride - 1) {
        runtimeError(vm, "LandscapeDrawTerrain coordinate arrays are smaller than the required vertex stride.");
        return makeVoid();
    }

    if (!ensureGlContext(vm, "LandscapeDrawTerrain")) {
        return makeVoid();
    }

    for (int z = 0; z < terrainSize; ++z) {
        glBegin(GL_TRIANGLE_STRIP);
        float worldZ0 = (float)asLd(worldZCoords[z]);
        float worldZ1 = (float)asLd(worldZCoords[z + 1]);
        int rowIndex = z * vertexStride;
        int nextRowIndex = (z + 1) * vertexStride;
        for (int x = 0; x <= terrainSize; ++x) {
            int idx0 = rowIndex + x;
            int idx1 = nextRowIndex + x;
            float worldX = (float)asLd(worldXCoords[x]);

            float r0 = clampf((float)asLd(vertexColorR[idx0]), 0.0f, 1.0f);
            float g0 = clampf((float)asLd(vertexColorG[idx0]), 0.0f, 1.0f);
            float b0 = clampf((float)asLd(vertexColorB[idx0]), 0.0f, 1.0f);
            float h0 = (float)asLd(vertexHeights[idx0]);
            float nx0, ny0, nz0;
            computeTerrainNormal(vertexHeights, worldXCoords, worldZCoords,
                                 vertexStride, terrainSize, x, z,
                                 &nx0, &ny0, &nz0);
            glNormal3f(nx0, ny0, nz0);
            glColor3f(r0, g0, b0);
            glVertex3f(worldX, h0, worldZ0);

            float r1 = clampf((float)asLd(vertexColorR[idx1]), 0.0f, 1.0f);
            float g1 = clampf((float)asLd(vertexColorG[idx1]), 0.0f, 1.0f);
            float b1 = clampf((float)asLd(vertexColorB[idx1]), 0.0f, 1.0f);
            float h1 = (float)asLd(vertexHeights[idx1]);
            float nx1, ny1, nz1;
            computeTerrainNormal(vertexHeights, worldXCoords, worldZCoords,
                                 vertexStride, terrainSize, x, z + 1,
                                 &nx1, &ny1, &nz1);
            glNormal3f(nx1, ny1, nz1);
            glColor3f(r1, g1, b1);
            glVertex3f(worldX, h1, worldZ1);
        }
        glEnd();
    }

    return makeVoid();
}

static void emitWaterVertex(float waterHeight,
                            float basePhase,
                            float baseSecondary,
                            float baseSparkle,
                            float worldX,
                            float worldZ,
                            float groundHeight,
                            float phaseOffset,
                            float secondaryOffset,
                            float sparkleOffset) {
    float depth = waterHeight - groundHeight;
    if (depth < 0.0f) depth = 0.0f;
    if (depth > 6.0f) depth = 6.0f;
    float depthFactor = depth / 6.0f;
    float shallow = 1.0f - depthFactor;
    float ripple = sinf(basePhase + phaseOffset) * (0.08f + 0.04f * depthFactor);
    float ripple2 = cosf(baseSecondary + secondaryOffset) * (0.05f + 0.05f * depthFactor);
    float surfaceHeight = waterHeight + 0.05f + ripple + ripple2;
    float foam = clampf(1.0f - depth * 0.45f, 0.0f, 1.0f);
    float sparkle = 0.02f + 0.06f * sinf(baseSparkle + sparkleOffset);
    float r = 0.05f + 0.08f * depthFactor + 0.18f * foam + sparkle * shallow * 0.4f;
    float g = 0.34f + 0.30f * depthFactor + 0.26f * foam + sparkle * shallow * 0.5f;
    float b = 0.55f + 0.32f * depthFactor + 0.22f * foam + sparkle * 0.6f;
    r = clampf(r, 0.0f, 1.0f);
    g = clampf(g, 0.0f, 1.0f);
    b = clampf(b, 0.0f, 1.0f);
    float alpha = 0.35f + 0.30f * shallow + sparkle * 0.4f;
    if (alpha < 0.18f) alpha = 0.18f;
    if (alpha > 0.82f) alpha = 0.82f;
    glColor4f(r, g, b, alpha);
    glNormal3f(0.0f, 1.0f, 0.0f);
    glVertex3f(worldX, surfaceHeight, worldZ);
}

static Value vmBuiltinLandscapeDrawWater(VM* vm, int arg_count, Value* args) {
    if (arg_count != 10) {
        runtimeError(vm, "LandscapeDrawWater expects 10 arguments.");
        return makeVoid();
    }

    int lower = 0, upper = 0;
    Value* vertexHeights = resolveArrayArg(vm, &args[0], "LandscapeDrawWater", &lower, &upper);
    if (!vertexHeights) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "LandscapeDrawWater requires vertex arrays starting at index 0.");
        return makeVoid();
    }
    int heightsUpper = upper;

    Value* worldXCoords = resolveArrayArg(vm, &args[1], "LandscapeDrawWater", &lower, &upper);
    if (!worldXCoords) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "LandscapeDrawWater requires coordinate arrays starting at index 0.");
        return makeVoid();
    }
    int coordUpper = upper;

    Value* worldZCoords = resolveArrayArg(vm, &args[2], "LandscapeDrawWater", NULL, &upper);
    if (!worldZCoords) return makeVoid();
    if (upper < coordUpper) coordUpper = upper;

    Value* waterPhaseOffset = resolveArrayArg(vm, &args[3], "LandscapeDrawWater", NULL, &upper);
    if (!waterPhaseOffset) return makeVoid();
    int phaseUpper = upper;

    Value* waterSecondaryOffset = resolveArrayArg(vm, &args[4], "LandscapeDrawWater", NULL, &upper);
    if (!waterSecondaryOffset) return makeVoid();
    if (upper < phaseUpper) phaseUpper = upper;

    Value* waterSparkleOffset = resolveArrayArg(vm, &args[5], "LandscapeDrawWater", NULL, &upper);
    if (!waterSparkleOffset) return makeVoid();
    if (upper < phaseUpper) phaseUpper = upper;

    if (!isRealType(args[6].type) && !IS_INTLIKE(args[6])) {
        runtimeError(vm, "LandscapeDrawWater expects numeric water height.");
        return makeVoid();
    }
    if (!isRealType(args[7].type) && !IS_INTLIKE(args[7])) {
        runtimeError(vm, "LandscapeDrawWater expects numeric time parameter.");
        return makeVoid();
    }
    if (!IS_INTLIKE(args[8]) || !IS_INTLIKE(args[9])) {
        runtimeError(vm, "LandscapeDrawWater expects integer terrain parameters.");
        return makeVoid();
    }

    float waterHeight = (float)asLd(args[6]);
    float timeSeconds = (float)asLd(args[7]);
    int terrainSize = (int)asI64(args[8]);
    int vertexStride = (int)asI64(args[9]);

    if (terrainSize < 1 || vertexStride < 2 || vertexStride != terrainSize + 1) {
        runtimeError(vm, "LandscapeDrawWater received inconsistent terrain parameters.");
        return makeVoid();
    }

    int vertexCount = vertexStride * vertexStride;
    if (heightsUpper < vertexCount - 1 || phaseUpper < vertexCount - 1) {
        runtimeError(vm, "LandscapeDrawWater arrays are smaller than the required vertex count.");
        return makeVoid();
    }
    if (coordUpper < vertexStride - 1) {
        runtimeError(vm, "LandscapeDrawWater coordinate arrays are smaller than the required vertex stride.");
        return makeVoid();
    }

    if (!ensureGlContext(vm, "LandscapeDrawWater")) {
        return makeVoid();
    }

    float allowance = 0.18f;
    float maxWaterHeight = waterHeight + allowance;
    float basePhase = timeSeconds * 0.7f;
    float baseSecondary = timeSeconds * 1.6f;
    float baseSparkle = timeSeconds * 2.4f;

    glBegin(GL_TRIANGLES);
    for (int z = 0; z < terrainSize; ++z) {
        int rowIndex = z * vertexStride;
        int nextRowIndex = (z + 1) * vertexStride;
        float worldZ0 = (float)asLd(worldZCoords[z]);
        float worldZ1 = (float)asLd(worldZCoords[z + 1]);
        for (int x = 0; x < terrainSize; ++x) {
            int idx00 = rowIndex + x;
            int idx10 = rowIndex + x + 1;
            int idx01 = nextRowIndex + x;
            int idx11 = nextRowIndex + x + 1;
            float h00 = (float)asLd(vertexHeights[idx00]);
            float h10 = (float)asLd(vertexHeights[idx10]);
            float h01 = (float)asLd(vertexHeights[idx01]);
            float h11 = (float)asLd(vertexHeights[idx11]);
            if (h00 <= maxWaterHeight && h10 <= maxWaterHeight && h01 <= maxWaterHeight) {
                float worldX0 = (float)asLd(worldXCoords[x]);
                float worldX1 = (float)asLd(worldXCoords[x + 1]);
                emitWaterVertex(waterHeight, basePhase, baseSecondary, baseSparkle,
                                worldX0, worldZ0, h00,
                                (float)asLd(waterPhaseOffset[idx00]),
                                (float)asLd(waterSecondaryOffset[idx00]),
                                (float)asLd(waterSparkleOffset[idx00]));
                emitWaterVertex(waterHeight, basePhase, baseSecondary, baseSparkle,
                                worldX1, worldZ0, h10,
                                (float)asLd(waterPhaseOffset[idx10]),
                                (float)asLd(waterSecondaryOffset[idx10]),
                                (float)asLd(waterSparkleOffset[idx10]));
                emitWaterVertex(waterHeight, basePhase, baseSecondary, baseSparkle,
                                worldX0, worldZ1, h01,
                                (float)asLd(waterPhaseOffset[idx01]),
                                (float)asLd(waterSecondaryOffset[idx01]),
                                (float)asLd(waterSparkleOffset[idx01]));
            }
            if (h10 <= maxWaterHeight && h11 <= maxWaterHeight && h01 <= maxWaterHeight) {
                float worldX1 = (float)asLd(worldXCoords[x + 1]);
                float worldX0 = (float)asLd(worldXCoords[x]);
                emitWaterVertex(waterHeight, basePhase, baseSecondary, baseSparkle,
                                worldX1, worldZ0, h10,
                                (float)asLd(waterPhaseOffset[idx10]),
                                (float)asLd(waterSecondaryOffset[idx10]),
                                (float)asLd(waterSparkleOffset[idx10]));
                emitWaterVertex(waterHeight, basePhase, baseSecondary, baseSparkle,
                                worldX1, worldZ1, h11,
                                (float)asLd(waterPhaseOffset[idx11]),
                                (float)asLd(waterSecondaryOffset[idx11]),
                                (float)asLd(waterSparkleOffset[idx11]));
                emitWaterVertex(waterHeight, basePhase, baseSecondary, baseSparkle,
                                worldX0, worldZ1, h01,
                                (float)asLd(waterPhaseOffset[idx01]),
                                (float)asLd(waterSecondaryOffset[idx01]),
                                (float)asLd(waterSparkleOffset[idx01]));
            }
        }
    }
    glEnd();

    return makeVoid();
}
#else
static Value vmBuiltinLandscapeDrawTerrain(VM* vm, int arg_count, Value* args) {
    runtimeError(vm, "LandscapeDrawTerrain requires SDL support.");
    return makeVoid();
}

static Value vmBuiltinLandscapeDrawWater(VM* vm, int arg_count, Value* args) {
    runtimeError(vm, "LandscapeDrawWater requires SDL support.");
    return makeVoid();
}
#endif

void registerLandscapeBuiltins(void) {
    registerVmBuiltin("landscapedrawterrain", vmBuiltinLandscapeDrawTerrain,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeDrawTerrain");
    registerVmBuiltin("landscapedrawwater", vmBuiltinLandscapeDrawWater,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeDrawWater");
    registerVmBuiltin("landscapeprecomputeworldcoords", vmBuiltinLandscapePrecomputeWorldCoords,
                      BUILTIN_TYPE_PROCEDURE, "LandscapePrecomputeWorldCoords");
    registerVmBuiltin("landscapeprecomputewateroffsets", vmBuiltinLandscapePrecomputeWaterOffsets,
                      BUILTIN_TYPE_PROCEDURE, "LandscapePrecomputeWaterOffsets");
}
