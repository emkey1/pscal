#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "vm/vm.h"
#include "vm/string_sentinels.h"
#include "runtime/shaders/terrain/terrain_shader.h"

#if defined(SDL) && defined(PSCAL_TARGET_IOS)

#include "backend_ast/pscal_sdl_runtime.h"

static Value landscapeUnsupportedBuiltin(VM* vm, const char* name) {
    runtimeError(vm, "%s is not supported on iOS builds (OpenGL renderers unavailable).",
                 name);
    return makeVoid();
}

#define DEFINE_LANDSCAPE_STUB(fn, label)              \
    static Value fn(VM* vm, int arg_count, Value* args) { \
        (void)arg_count;                              \
        (void)args;                                   \
        return landscapeUnsupportedBuiltin(vm, label);\
    }

DEFINE_LANDSCAPE_STUB(vmBuiltinLandscapeConfigureProcedural, "LandscapeConfigureProcedural");
DEFINE_LANDSCAPE_STUB(vmBuiltinLandscapeDrawTerrain, "LandscapeDrawTerrain");
DEFINE_LANDSCAPE_STUB(vmBuiltinLandscapeDrawWater, "LandscapeDrawWater");
DEFINE_LANDSCAPE_STUB(vmBuiltinLandscapePrecomputeWorldCoords, "LandscapePrecomputeWorldCoords");
DEFINE_LANDSCAPE_STUB(vmBuiltinLandscapePrecomputeWaterOffsets,
                      "LandscapePrecomputeWaterOffsets");
DEFINE_LANDSCAPE_STUB(vmBuiltinLandscapeBuildHeightField, "LandscapeBuildHeightField");
DEFINE_LANDSCAPE_STUB(vmBuiltinLandscapeBakeVertexData, "LandscapeBakeVertexData");
DEFINE_LANDSCAPE_STUB(vmBuiltinLandscapeSetPalettePreset, "LandscapeSetPalettePreset");
DEFINE_LANDSCAPE_STUB(vmBuiltinLandscapeSetLightingPreset, "LandscapeSetLightingPreset");
DEFINE_LANDSCAPE_STUB(vmBuiltinLandscapeDrawSkyDome, "LandscapeDrawSkyDome");
DEFINE_LANDSCAPE_STUB(vmBuiltinLandscapeDrawCloudLayer, "LandscapeDrawCloudLayer");

#undef DEFINE_LANDSCAPE_STUB

void registerLandscapeBuiltins(void) {
    registerVmBuiltin("landscapeconfigureprocedural", vmBuiltinLandscapeConfigureProcedural,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeConfigureProcedural");
    registerVmBuiltin("landscapedrawterrain", vmBuiltinLandscapeDrawTerrain,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeDrawTerrain");
    registerVmBuiltin("landscapedrawwater", vmBuiltinLandscapeDrawWater,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeDrawWater");
    registerVmBuiltin("landscapeprecomputeworldcoords", vmBuiltinLandscapePrecomputeWorldCoords,
                      BUILTIN_TYPE_PROCEDURE, "LandscapePrecomputeWorldCoords");
    registerVmBuiltin("landscapeprecomputewateroffsets", vmBuiltinLandscapePrecomputeWaterOffsets,
                      BUILTIN_TYPE_PROCEDURE, "LandscapePrecomputeWaterOffsets");
    registerVmBuiltin("landscapebuildheightfield", vmBuiltinLandscapeBuildHeightField,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeBuildHeightField");
    registerVmBuiltin("landscapebakevertexdata", vmBuiltinLandscapeBakeVertexData,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeBakeVertexData");
    registerVmBuiltin("landscapesetpalettepreset", vmBuiltinLandscapeSetPalettePreset,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeSetPalettePreset");
    registerVmBuiltin("landscapesetlightingpreset", vmBuiltinLandscapeSetLightingPreset,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeSetLightingPreset");
    registerVmBuiltin("landscapedrawskydome", vmBuiltinLandscapeDrawSkyDome,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeDrawSkyDome");
    registerVmBuiltin("landscapedrawcloudlayer", vmBuiltinLandscapeDrawCloudLayer,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeDrawCloudLayer");
}

#else

#ifdef SDL
#include "backend_ast/pscal_sdl_runtime.h"
#if defined(PSCAL_TARGET_IOS)
#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>
#else
#include PSCALI_SDL_OPENGL_HEADER
#endif
#include "runtime/terrain/terrain_generator.h"
#include "runtime/shaders/sky/sky_dome.h"
#include "runtime/shaders/sky/cloud_layer.h"
#endif

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct NumericVarRef {
    Value* slot;
    bool isInteger;
} NumericVarRef;

typedef struct ArrayArg {
    Value* values;
    Value* owner;
    int lower;
    int upper;
} ArrayArg;

static const Value* landscapeResolveStringPointer(const Value* value) {
    const Value* current = value;
    int depth = 0;
    while (current && current->type == TYPE_POINTER &&
           current->base_type_node != STRING_CHAR_PTR_SENTINEL) {
        if (!current->ptr_val) {
            return NULL;
        }
        current = (const Value*)current->ptr_val;
        if (++depth > 16) {
            return NULL;
        }
    }
    return current;
}

static const char* landscapeValueToCString(const Value* value) {
    if (!value) return NULL;
    if (value->type == TYPE_STRING) {
        return value->s_val ? value->s_val : "";
    }
    if (value->type == TYPE_POINTER) {
        if (value->base_type_node == STRING_CHAR_PTR_SENTINEL) {
            return (const char*)value->ptr_val;
        }
        const Value* resolved = landscapeResolveStringPointer(value);
        if (!resolved) return NULL;
        if (resolved->type == TYPE_STRING) {
            return resolved->s_val ? resolved->s_val : "";
        }
        if (resolved->type == TYPE_POINTER && resolved->base_type_node == STRING_CHAR_PTR_SENTINEL) {
            return (const char*)resolved->ptr_val;
        }
    }
    return NULL;
}

static bool sanityCheckNumericArray(VM* vm, const ArrayArg* array, int count, const char* name,
                                    const char* field) {
    if (!array || !array->values) return false;
    for (int i = 0; i < count; ++i) {
        Value value = array->values[i];
        if (!IS_NUMERIC(value)) {
            runtimeError(vm, "%s detected non-numeric data in %s at index %d.", name, field, i);
            return false;
        }
        double numeric = (double)asLd(value);
        if (!isfinite(numeric)) {
            runtimeError(vm, "%s detected invalid numeric data in %s at index %d.", name, field, i);
            return false;
        }
    }
    return true;
}

static bool sanityCheckColorArray(VM* vm, const ArrayArg* array, int count, const char* name,
                                  const char* field) {
    if (!sanityCheckNumericArray(vm, array, count, name, field)) return false;
    const double epsilon = 0.01;
    for (int i = 0; i < count; ++i) {
        double sample = (double)asLd(array->values[i]);
        if (sample < -epsilon || sample > 1.0 + epsilon) {
            runtimeError(vm, "%s detected out-of-range color data in %s at index %d.", name, field, i);
            return false;
        }
    }
    return true;
}

static bool sanityCheckNormalArrays(VM* vm, const ArrayArg* nx, const ArrayArg* ny, const ArrayArg* nz,
                                    int count, const char* name) {
    if (!sanityCheckNumericArray(vm, nx, count, name, "vertex normals (X)") ||
        !sanityCheckNumericArray(vm, ny, count, name, "vertex normals (Y)") ||
        !sanityCheckNumericArray(vm, nz, count, name, "vertex normals (Z)")) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        double x = (double)asLd(nx->values[i]);
        double y = (double)asLd(ny->values[i]);
        double z = (double)asLd(nz->values[i]);
        double lengthSq = x * x + y * y + z * z;
        if (!isfinite(lengthSq) || lengthSq < 1e-4 || lengthSq > 4.0) {
            runtimeError(vm, "%s detected invalid normal vector magnitude at index %d.", name, i);
            return false;
        }
    }
    return true;
}

static bool sanityCheckMonotonic(VM* vm, const ArrayArg* array, int count, const char* name,
                                 const char* field) {
    if (!sanityCheckNumericArray(vm, array, count, name, field)) return false;
    double previous = (double)asLd(array->values[0]);
    for (int i = 1; i < count; ++i) {
        double current = (double)asLd(array->values[i]);
        if (current + 1e-6 < previous) {
            runtimeError(vm, "%s detected non-monotonic data in %s at index %d.", name, field, i);
            return false;
        }
        previous = current;
    }
    return true;
}

static ArrayArg resolveArrayArg(VM* vm, Value* arg, const char* name, int* lower, int* upper) {
    ArrayArg result = {0};
    Value* arrVal = NULL;
    if (arg->type == TYPE_POINTER) {
        arrVal = (Value*)arg->ptr_val;
        if (!arrVal) {
            runtimeError(vm, "%s received a NIL pointer.", name);
            return result;
        }
        result.owner = arrVal;
    } else {
        arrVal = arg;
    }

    if (!arrVal || arrVal->type != TYPE_ARRAY) {
        runtimeError(vm, "%s expects VAR array arguments.", name);
        return result;
    }
    if (arrVal->dimensions > 1) {
        runtimeError(vm, "%s arrays must be single dimensional.", name);
        return result;
    }

    int l = (arrVal->dimensions > 0 && arrVal->lower_bounds) ? arrVal->lower_bounds[0]
                                                            : arrVal->lower_bound;
    int u = (arrVal->dimensions > 0 && arrVal->upper_bounds) ? arrVal->upper_bounds[0]
                                                            : arrVal->upper_bound;
    result.lower = l;
    result.upper = u;
    if (lower) *lower = l;
    if (upper) *upper = u;

    if (!arrVal->array_val) {
        runtimeError(vm, "%s received an array with NIL storage.", name);
        return result;
    }
    result.values = arrVal->array_val;
    return result;
}

static inline void assignFloatValue(Value* target, double value) {
    if (!target) return;
    target->type = TYPE_DOUBLE;
    SET_REAL_VALUE(target, value);
}

static bool fetchNumericVarRef(VM* vm, Value* arg, const char* name, const char* paramDesc,
                               NumericVarRef* out) {
    if (!out) return false;
    if (arg->type == TYPE_POINTER) {
        Value* slot = (Value*)arg->ptr_val;
        if (!slot) {
            runtimeError(vm, "%s received NIL storage for %s.", name, paramDesc);
            return false;
        }

        if (!IS_NUMERIC(*slot)) {
            runtimeError(vm, "%s %s must be numeric.", name, paramDesc);
            return false;
        }

        out->slot = slot;
        out->isInteger = IS_INTLIKE(*slot);
        return true;
    }

    if (!IS_NUMERIC(*arg)) {
        runtimeError(vm, "%s expects numeric or VAR parameter for %s.", name, paramDesc);
        return false;
    }

    out->slot = NULL;
    out->isInteger = IS_INTLIKE(*arg);
    return true;
}

static void assignNumericVar(const NumericVarRef* ref, double value) {
    if (!ref || !ref->slot) return;
    if (ref->isInteger) {
        SET_INT_VALUE(ref->slot, (long long)value);
    } else {
        SET_REAL_VALUE(ref->slot, value);
    }
}

static float clampf(float v, float minVal, float maxVal) {
    if (v < minVal) return minVal;
    if (v > maxVal) return maxVal;
    return v;
}

#ifdef SDL
static float saturatef(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static bool valueToBool(Value v, bool* out) {
    if (out == NULL) return false;
    if (v.type == TYPE_BOOLEAN) {
        *out = AS_BOOLEAN(v);
        return true;
    }
    if (IS_INTLIKE(v)) {
        *out = AS_INTEGER(v) != 0;
        return true;
    }
    if (isRealType(v.type)) {
        *out = fabs((double)asLd(v)) > 1e-6;
        return true;
    }
    return false;
}

static bool valueToFloat32(Value v, float* out) {
    if (!out) return false;
    if (isRealType(v.type)) {
        *out = (float)asLd(v);
        return true;
    }
    if (IS_INTLIKE(v)) {
        *out = (float)AS_INTEGER(v);
        return true;
    }
    return false;
}

typedef struct ProceduralTerrainState {
    bool enabled;
    bool initialised;
    TerrainGenerator generator;
    TerrainGeneratorConfig config;
    int lastResolution;
} ProceduralTerrainState;

static ProceduralTerrainState gProceduralTerrain = {0};

typedef struct SkyDomeState {
    bool initialised;
    SkyDome dome;
} SkyDomeState;

static SkyDomeState gSkyDome = {0};

typedef struct CloudLayerState {
    CloudLayerRenderer *renderer;
} CloudLayerState;

static CloudLayerState gCloudLayer = {0};

static void ensureProceduralGenerator(void) {
    if (!gProceduralTerrain.initialised) {
        terrainGeneratorInit(&gProceduralTerrain.generator);
        gProceduralTerrain.initialised = true;
    }
}

static void ensureSkyDomeState(void) {
    if (!gSkyDome.initialised) {
        skyDomeInit(&gSkyDome.dome);
        gSkyDome.initialised = true;
    }
}

static void shutdownSkyDome(void) {
    if (gSkyDome.initialised) {
        skyDomeFree(&gSkyDome.dome);
        gSkyDome.initialised = false;
    }
}

static void shutdownCloudLayer(void) {
    if (gCloudLayer.renderer) {
        cloudLayerRendererShutdown(&gCloudLayer.renderer);
    }
}

static void disableProceduralGenerator(void) {
    if (gProceduralTerrain.initialised) {
        terrainGeneratorFree(&gProceduralTerrain.generator);
        gProceduralTerrain.initialised = false;
    }
    memset(&gProceduralTerrain.config, 0, sizeof(gProceduralTerrain.config));
    gProceduralTerrain.enabled = false;
    gProceduralTerrain.lastResolution = -1;
    shutdownSkyDome();
    shutdownCloudLayer();
}
#endif

#ifdef SDL
static bool ensureGlContext(VM* vm, const char* name) {
    if (!gSdlInitialized || !gSdlWindow || !gSdlGLContext) {
        runtimeError(vm,
                     "%s requires an active OpenGL window. Call InitGraph3D first.",
                     name);
        return false;
    }
    return true;
}

static void computeTerrainNormal(const ArrayArg* vertexHeights,
                                 const ArrayArg* worldXCoords,
                                 const ArrayArg* worldZCoords,
                                 int vertexStride,
                                 int terrainSize,
                                 int x,
                                 int z,
                                 float* nx,
                                 float* ny,
                                 float* nz) {
    int leftX = x > 0 ? x - 1 : x;
    int rightX = x < terrainSize ? x + 1 : x;
    int downZ = z > 0 ? z - 1 : z;
    int upZ = z < terrainSize ? z + 1 : z;

    int rowIndex = z * vertexStride;
    int leftIdx = rowIndex + leftX;
    int rightIdx = rowIndex + rightX;
    int downIdx = downZ * vertexStride + x;
    int upIdx = upZ * vertexStride + x;

    float leftHeight = (float)asLd(vertexHeights->values[leftIdx]);
    float rightHeight = (float)asLd(vertexHeights->values[rightIdx]);
    float downHeight = (float)asLd(vertexHeights->values[downIdx]);
    float upHeight = (float)asLd(vertexHeights->values[upIdx]);

    float leftCoord = (float)asLd(worldXCoords->values[leftX]);
    float rightCoord = (float)asLd(worldXCoords->values[rightX]);
    float downCoord = (float)asLd(worldZCoords->values[downZ]);
    float upCoord = (float)asLd(worldZCoords->values[upZ]);

    float dx = rightCoord - leftCoord;
    if (fabsf(dx) < 1e-6f) {
        dx = dx >= 0.0f ? 1.0f : -1.0f;
    }
    float dz = upCoord - downCoord;
    if (fabsf(dz) < 1e-6f) {
        dz = dz >= 0.0f ? 1.0f : -1.0f;
    }

    float slopeX = (rightHeight - leftHeight) / dx;
    float slopeZ = (upHeight - downHeight) / dz;

    float nxVal = -slopeX;
    float nyVal = 1.0f;
    float nzVal = -slopeZ;
    float lengthSq = nxVal * nxVal + nyVal * nyVal + nzVal * nzVal;
    if (!isfinite(lengthSq) || lengthSq < 1.0e-8f) {
        nxVal = 0.0f;
        nyVal = 1.0f;
        nzVal = 0.0f;
    } else {
        float invLen = 1.0f / sqrtf(lengthSq);
        nxVal *= invLen;
        nyVal *= invLen;
        nzVal *= invLen;
    }

    if (nx) *nx = nxVal;
    if (ny) *ny = nyVal;
    if (nz) *nz = nzVal;
}

static void emitWaterVertexGl(const ArrayArg* worldXCoords,
                              const ArrayArg* worldZCoords,
                              const ArrayArg* waterPhaseOffset,
                              const ArrayArg* waterSecondaryOffset,
                              const ArrayArg* waterSparkleOffset,
                              double waterHeight,
                              double basePhase,
                              double baseSecondary,
                              double baseSparkle,
                              int gridX,
                              int gridZ,
                              int idx,
                              float groundHeight) {
    float depth = (float)(waterHeight - groundHeight);
    if (depth < 0.0f) depth = 0.0f;
    if (depth > 6.0f) depth = 6.0f;
    float depthFactor = depth / 6.0f;
    float shallow = 1.0f - depthFactor;

    double phase = basePhase + (double)asLd(waterPhaseOffset->values[idx]);
    double secondary = baseSecondary + (double)asLd(waterSecondaryOffset->values[idx]);
    double sparklePhase = baseSparkle + (double)asLd(waterSparkleOffset->values[idx]);

    float ripple = (float)sin(phase) * (0.08f + 0.04f * depthFactor);
    float ripple2 = (float)cos(secondary) * (0.05f + 0.05f * depthFactor);
    float surfaceHeight = (float)(waterHeight + 0.05) + ripple + ripple2;

    float worldX = (float)asLd(worldXCoords->values[gridX]);
    float worldZ = (float)asLd(worldZCoords->values[gridZ]);

    float foam = saturatef(1.0f - depth * 0.45f);
    float sparkle = 0.02f + 0.06f * (float)sin(sparklePhase);

    float r = 0.05f + 0.08f * depthFactor + 0.18f * foam + sparkle * shallow * 0.4f;
    float g = 0.34f + 0.30f * depthFactor + 0.26f * foam + sparkle * shallow * 0.5f;
    float b = 0.55f + 0.32f * depthFactor + 0.22f * foam + sparkle * 0.6f;
    r = saturatef(r);
    g = saturatef(g);
    b = saturatef(b);

    float alpha = 0.35f + 0.30f * shallow + sparkle * 0.4f;
    if (alpha < 0.18f) alpha = 0.18f;
    if (alpha > 0.82f) alpha = 0.82f;

    glColor4f(r, g, b, alpha);
    glNormal3f(0.0f, 1.0f, 0.0f);
    glVertex3f(worldX, surfaceHeight, worldZ);
}
#endif

static double landscapeBaseNoise(int x, int z, int seed) {
    long long n = (long long)x * 374761393LL + (long long)z * 668265263LL +
                  (long long)seed * 362437LL;
    n %= 2147483647LL;
    if (n < 0) n += 2147483647LL;
    double value = (double)n / 2147483647.0;
    return value * 2.0 - 1.0;
}

static double landscapeFade(double t) {
    return t * t * (3.0 - 2.0 * t);
}

static double landscapeValueNoise(double x, double z, int seed) {
    double xiFloor = floor(x);
    double ziFloor = floor(z);
    int xi = (int)xiFloor;
    int zi = (int)ziFloor;
    double xf = x - xiFloor;
    double zf = z - ziFloor;

    double v00 = landscapeBaseNoise(xi, zi, seed);
    double v10 = landscapeBaseNoise(xi + 1, zi, seed);
    double v01 = landscapeBaseNoise(xi, zi + 1, seed);
    double v11 = landscapeBaseNoise(xi + 1, zi + 1, seed);

    double u = landscapeFade(xf);
    double v = landscapeFade(zf);
    double i1 = v00 + (v10 - v00) * u;
    double i2 = v01 + (v11 - v01) * u;
    return i1 + (i2 - i1) * v;
}

static double landscapeFbm(double x, double z, int octaves, int seed) {
    double amplitude = 1.0;
    double frequency = 1.0;
    double sum = 0.0;
    double total = 0.0;
    for (int octave = 0; octave < octaves; ++octave) {
        sum += landscapeValueNoise(x * frequency, z * frequency, seed) * amplitude;
        total += amplitude;
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    if (total == 0.0) return 0.0;
    return sum / total;
}

static Value vmBuiltinLandscapePrecomputeWorldCoords(VM* vm, int arg_count, Value* args) {
    if (arg_count != 5) {
        runtimeError(vm, "LandscapePrecomputeWorldCoords expects 5 arguments.");
        return makeVoid();
    }

    int lower = 0, upper = 0;
    ArrayArg worldX =
        resolveArrayArg(vm, &args[0], "LandscapePrecomputeWorldCoords", &lower, &upper);
    if (!worldX.values) return makeVoid();
    if (worldX.lower != 0) {
        runtimeError(vm,
                     "LandscapePrecomputeWorldCoords requires coordinate arrays starting at index 0.");
        return makeVoid();
    }
    int coordUpper = worldX.upper;

    ArrayArg worldZ =
        resolveArrayArg(vm, &args[1], "LandscapePrecomputeWorldCoords", &lower, &upper);
    if (!worldZ.values) return makeVoid();
    if (worldZ.lower != 0) {
        runtimeError(vm,
                     "LandscapePrecomputeWorldCoords requires coordinate arrays starting at index 0.");
        return makeVoid();
    }
    if (worldZ.upper < coordUpper) coordUpper = worldZ.upper;

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
        runtimeError(vm,
                     "LandscapePrecomputeWorldCoords arrays are smaller than the required vertex stride.");
        return makeVoid();
    }

    double half = terrainSize * 0.5;
    for (int i = 0; i < vertexStride; ++i) {
        double world = (i - half) * tileScale;
        assignFloatValue(&worldX.values[i], world);
        assignFloatValue(&worldZ.values[i], world);
    }

    if (worldX.owner && worldX.owner->array_val != worldX.values) {
        for (int i = 0; i <= coordUpper; ++i) {
            worldX.owner->array_val[i] = worldX.values[i];
        }
    }
    if (worldZ.owner && worldZ.owner->array_val != worldZ.values) {
        for (int i = 0; i <= coordUpper; ++i) {
            worldZ.owner->array_val[i] = worldZ.values[i];
        }
    }

    if (!sanityCheckMonotonic(vm, &worldX, vertexStride, "LandscapePrecomputeWorldCoords",
                              "world X coordinates") ||
        !sanityCheckMonotonic(vm, &worldZ, vertexStride, "LandscapePrecomputeWorldCoords",
                              "world Z coordinates")) {
        return makeVoid();
    }

    return makeVoid();
}

static Value vmBuiltinLandscapePrecomputeWaterOffsets(VM* vm, int arg_count, Value* args) {
    if (arg_count != 5) {
        runtimeError(vm, "LandscapePrecomputeWaterOffsets expects 5 arguments.");
        return makeVoid();
    }

    int lower = 0, upper = 0;
    ArrayArg waterPhase =
        resolveArrayArg(vm, &args[0], "LandscapePrecomputeWaterOffsets", &lower, &upper);
    if (!waterPhase.values) return makeVoid();
    if (waterPhase.lower != 0) {
        runtimeError(vm,
                     "LandscapePrecomputeWaterOffsets requires offset arrays starting at index 0.");
        return makeVoid();
    }
    int arrayUpper = waterPhase.upper;

    ArrayArg waterSecondary =
        resolveArrayArg(vm, &args[1], "LandscapePrecomputeWaterOffsets", &lower, &upper);
    if (!waterSecondary.values) return makeVoid();
    if (waterSecondary.lower != 0) {
        runtimeError(vm,
                     "LandscapePrecomputeWaterOffsets requires offset arrays starting at index 0.");
        return makeVoid();
    }
    if (waterSecondary.upper < arrayUpper) arrayUpper = waterSecondary.upper;

    ArrayArg waterSparkle =
        resolveArrayArg(vm, &args[2], "LandscapePrecomputeWaterOffsets", &lower, &upper);
    if (!waterSparkle.values) return makeVoid();
    if (waterSparkle.lower != 0) {
        runtimeError(vm,
                     "LandscapePrecomputeWaterOffsets requires offset arrays starting at index 0.");
        return makeVoid();
    }
    if (waterSparkle.upper < arrayUpper) arrayUpper = waterSparkle.upper;

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
        runtimeError(vm,
                     "LandscapePrecomputeWaterOffsets arrays are smaller than the required vertex count.");
        return makeVoid();
    }

    for (int z = 0; z <= terrainSize; ++z) {
        int rowIndex = z * vertexStride;
        double zPhase = z * 0.12;
        double zSecondary = z * 0.21;
        double zSparkle = z * 0.22;
        for (int x = 0; x <= terrainSize; ++x) {
            int idx = rowIndex + x;
            assignFloatValue(&waterPhase.values[idx], x * 0.18 + zPhase);
            assignFloatValue(&waterSecondary.values[idx], x * 0.05 + zSecondary);
            assignFloatValue(&waterSparkle.values[idx], x * 0.22 + zSparkle);
        }
    }

    if (waterPhase.owner && waterPhase.owner->array_val != waterPhase.values) {
        for (int i = 0; i < vertexCount; ++i) {
            waterPhase.owner->array_val[i] = waterPhase.values[i];
        }
    }
    if (waterSecondary.owner && waterSecondary.owner->array_val != waterSecondary.values) {
        for (int i = 0; i < vertexCount; ++i) {
            waterSecondary.owner->array_val[i] = waterSecondary.values[i];
        }
    }
    if (waterSparkle.owner && waterSparkle.owner->array_val != waterSparkle.values) {
        for (int i = 0; i < vertexCount; ++i) {
            waterSparkle.owner->array_val[i] = waterSparkle.values[i];
        }
    }

    if (!sanityCheckNumericArray(vm, &waterPhase, vertexCount, "LandscapePrecomputeWaterOffsets",
                                 "water phase offsets") ||
        !sanityCheckNumericArray(vm, &waterSecondary, vertexCount,
                                 "LandscapePrecomputeWaterOffsets", "water secondary offsets") ||
        !sanityCheckNumericArray(vm, &waterSparkle, vertexCount,
                                 "LandscapePrecomputeWaterOffsets", "water sparkle offsets")) {
        return makeVoid();
    }

    return makeVoid();
}

static float landscapeHeightAt(const Value* heights, int vertexStride, int terrainSize, int x, int z) {
    if (x < 0) x = 0;
    if (x > terrainSize) x = terrainSize;
    if (z < 0) z = 0;
    if (z > terrainSize) z = terrainSize;
    return (float)asLd(heights[z * vertexStride + x]);
}

static Value vmBuiltinLandscapeBuildHeightField(VM* vm, int arg_count, Value* args) {
    const char* name = "LandscapeBuildHeightField";
    if (arg_count != 9) {
        runtimeError(vm, "%s expects 9 arguments.", name);
        return makeVoid();
    }

    int lower = 0, upper = 0;
    ArrayArg heightArray = resolveArrayArg(vm, &args[0], name, &lower, &upper);
    if (!heightArray.values) return makeVoid();
    if (heightArray.lower != 0) {
        runtimeError(vm, "%s requires height arrays starting at index 0.", name);
        return makeVoid();
    }

    if (!IS_INTLIKE(args[1])) {
        runtimeError(vm, "%s expects integer seed argument.", name);
        return makeVoid();
    }
    if (!IS_INTLIKE(args[2]) || !IS_INTLIKE(args[3])) {
        runtimeError(vm, "%s expects integer terrain parameters.", name);
        return makeVoid();
    }
    if (!isRealType(args[4].type) && !IS_INTLIKE(args[4])) {
        runtimeError(vm, "%s expects numeric height scale.", name);
        return makeVoid();
    }
    if (!IS_INTLIKE(args[5])) {
        runtimeError(vm, "%s expects integer octave count.", name);
        return makeVoid();
    }

    NumericVarRef minHeightRef;
    NumericVarRef maxHeightRef;
    NumericVarRef normScaleRef;
    if (!fetchNumericVarRef(vm, &args[6], name, "min height", &minHeightRef)) return makeVoid();
    if (!fetchNumericVarRef(vm, &args[7], name, "max height", &maxHeightRef)) return makeVoid();
    if (!fetchNumericVarRef(vm, &args[8], name, "normalization scale", &normScaleRef))
        return makeVoid();

    int seed = (int)asI64(args[1]);
    int terrainSize = (int)asI64(args[2]);
    int vertexStride = (int)asI64(args[3]);
    double heightScale = (double)asLd(args[4]);
    int octaves = (int)asI64(args[5]);

    if (terrainSize < 1 || vertexStride < 2 || vertexStride != terrainSize + 1) {
        runtimeError(vm, "%s received inconsistent terrain parameters.", name);
        return makeVoid();
    }
    if (heightArray.upper < vertexStride * vertexStride - 1) {
        runtimeError(vm, "%s height array is smaller than required vertex count.", name);
        return makeVoid();
    }
    if (octaves < 0) octaves = 0;

    double minHeight = DBL_MAX;
    double maxHeight = -DBL_MAX;
    double baseFrequency = 0.035;
    double seedOffsetX = seed * 0.13;
    double seedOffsetZ = seed * 0.29;

    for (int z = 0; z <= terrainSize; ++z) {
        double sampleZ = (z + seedOffsetZ) * baseFrequency;
        int rowIndex = z * vertexStride;
        for (int x = 0; x <= terrainSize; ++x) {
            double sampleX = (x + seedOffsetX) * baseFrequency;
            double height = landscapeFbm(sampleX, sampleZ, octaves, seed) * heightScale;
            int idx = rowIndex + x;
            assignFloatValue(&heightArray.values[idx], height);
            if (height < minHeight) minHeight = height;
            if (height > maxHeight) maxHeight = height;
        }
    }

    int vertexCount = vertexStride * vertexStride;
    if (heightArray.owner && heightArray.owner->array_val != heightArray.values) {
        for (int i = 0; i < vertexCount; ++i) {
            heightArray.owner->array_val[i] = heightArray.values[i];
        }
    }

    if (!sanityCheckNumericArray(vm, &heightArray, vertexCount, name, "height field")) {
        return makeVoid();
    }

    if (!isfinite(minHeight)) minHeight = 0.0;
    if (!isfinite(maxHeight)) maxHeight = minHeight;

    double span = maxHeight - minHeight;
    if (span <= 0.0001) {
        maxHeight = minHeight + 0.001;
        span = maxHeight - minHeight;
    }
    double normalizationScale = (span <= 0.0001) ? 0.0 : 1.0 / span;

    assignNumericVar(&minHeightRef, minHeight);
    assignNumericVar(&maxHeightRef, maxHeight);
    assignNumericVar(&normScaleRef, normalizationScale);

    return makeVoid();
}

static Value vmBuiltinLandscapeConfigureProcedural(VM* vm, int arg_count, Value* args) {
    const char* name = "LandscapeConfigureProcedural";
#ifndef SDL
    (void)arg_count;
    (void)args;
    runtimeError(vm, "%s requires SDL support.", name);
    return makeVoid();
#else
    if (arg_count != 9) {
        runtimeError(vm, "%s expects 9 arguments (seed, amplitude, frequency, octaves, lacunarity, persistence, offsetX, offsetZ, useSimplex).",
                     name);
        return makeVoid();
    }
    if (!IS_INTLIKE(args[0]) ||
        (!isRealType(args[1].type) && !IS_INTLIKE(args[1])) ||
        (!isRealType(args[2].type) && !IS_INTLIKE(args[2])) ||
        !IS_INTLIKE(args[3]) ||
        (!isRealType(args[4].type) && !IS_INTLIKE(args[4])) ||
        (!isRealType(args[5].type) && !IS_INTLIKE(args[5])) ||
        (!isRealType(args[6].type) && !IS_INTLIKE(args[6])) ||
        (!isRealType(args[7].type) && !IS_INTLIKE(args[7]))) {
        runtimeError(vm, "%s received invalid parameter types.", name);
        return makeVoid();
    }

    bool useSimplex = false;
    if (!valueToBool(args[8], &useSimplex)) {
        runtimeError(vm, "%s useSimplex parameter must be boolean or numeric.", name);
        return makeVoid();
    }

    int octaves = (int)asI64(args[3]);
    float amplitude = (float)asLd(args[1]);
    if (octaves <= 0 || amplitude <= 0.0f) {
        disableProceduralGenerator();
        return makeVoid();
    }

    ensureProceduralGenerator();
    TerrainGeneratorConfig config = gProceduralTerrain.config;
    config.seed = (uint32_t)asI64(args[0]);
    config.amplitude = amplitude;
    config.frequency = (float)asLd(args[2]);
    config.octaves = octaves;
    config.lacunarity = (float)asLd(args[4]);
    config.persistence = (float)asLd(args[5]);
    config.offsetX = (float)asLd(args[6]);
    config.offsetZ = (float)asLd(args[7]);
    config.useSimplex = useSimplex;

    if (config.frequency <= 0.0f) config.frequency = 0.01f;
    if (config.lacunarity <= 0.0f) config.lacunarity = 2.0f;
    if (config.persistence <= 0.0f) config.persistence = 0.5f;

    gProceduralTerrain.config = config;
    gProceduralTerrain.generator.config = config;
    gProceduralTerrain.enabled = true;
    gProceduralTerrain.lastResolution = -1;
    gProceduralTerrain.generator.gpuDirty = true;
    return makeVoid();
#endif
}

static Value vmBuiltinLandscapeBakeVertexData(VM* vm, int arg_count, Value* args) {
    const char* name = "LandscapeBakeVertexData";
    if (arg_count != 16) {
        runtimeError(vm, "%s expects 16 arguments.", name);
        return makeVoid();
    }

    int lower = 0, upper = 0;
    ArrayArg sourceHeights = resolveArrayArg(vm, &args[0], name, &lower, &upper);
    if (!sourceHeights.values) return makeVoid();
    if (sourceHeights.lower != 0) {
        runtimeError(vm, "%s requires source arrays starting at index 0.", name);
        return makeVoid();
    }
    int heightsUpper = sourceHeights.upper;

    ArrayArg vertexHeights = resolveArrayArg(vm, &args[1], name, &lower, &upper);
    if (!vertexHeights.values) return makeVoid();
    if (vertexHeights.lower != 0) {
        runtimeError(vm, "%s requires vertex arrays starting at index 0.", name);
        return makeVoid();
    }
    int vertexUpper = vertexHeights.upper;

    ArrayArg vertexNormalX = resolveArrayArg(vm, &args[2], name, NULL, &upper);
    if (!vertexNormalX.values) return makeVoid();
    if (vertexNormalX.upper < vertexUpper) vertexUpper = vertexNormalX.upper;

    ArrayArg vertexNormalY = resolveArrayArg(vm, &args[3], name, NULL, &upper);
    if (!vertexNormalY.values) return makeVoid();
    if (vertexNormalY.upper < vertexUpper) vertexUpper = vertexNormalY.upper;

    ArrayArg vertexNormalZ = resolveArrayArg(vm, &args[4], name, NULL, &upper);
    if (!vertexNormalZ.values) return makeVoid();
    if (vertexNormalZ.upper < vertexUpper) vertexUpper = vertexNormalZ.upper;

    ArrayArg vertexColorR = resolveArrayArg(vm, &args[5], name, NULL, &upper);
    if (!vertexColorR.values) return makeVoid();
    if (vertexColorR.upper < vertexUpper) vertexUpper = vertexColorR.upper;

    ArrayArg vertexColorG = resolveArrayArg(vm, &args[6], name, NULL, &upper);
    if (!vertexColorG.values) return makeVoid();
    if (vertexColorG.upper < vertexUpper) vertexUpper = vertexColorG.upper;

    ArrayArg vertexColorB = resolveArrayArg(vm, &args[7], name, NULL, &upper);
    if (!vertexColorB.values) return makeVoid();
    if (vertexColorB.upper < vertexUpper) vertexUpper = vertexColorB.upper;

    NumericVarRef waterHeightRef;
    if (!fetchNumericVarRef(vm, &args[8], name, "water height", &waterHeightRef))
        return makeVoid();

    if ((!isRealType(args[9].type) && !IS_INTLIKE(args[9])) ||
        (!isRealType(args[10].type) && !IS_INTLIKE(args[10])) ||
        (!isRealType(args[11].type) && !IS_INTLIKE(args[11])) ||
        (!isRealType(args[12].type) && !IS_INTLIKE(args[12])) ||
        (!isRealType(args[13].type) && !IS_INTLIKE(args[13]))) {
        runtimeError(vm, "%s expects numeric parameters for height bounds, normalization, water level, and tile scale.",
                     name);
        return makeVoid();
    }
    if (!IS_INTLIKE(args[14]) || !IS_INTLIKE(args[15])) {
        runtimeError(vm, "%s expects integer terrain parameters.", name);
        return makeVoid();
    }

    double minHeight = (double)asLd(args[9]);
    double maxHeight = (double)asLd(args[10]);
    double normalizationScale = (double)asLd(args[11]);
    double waterLevel = (double)asLd(args[12]);
    double tileScale = (double)asLd(args[13]);
    int terrainSize = (int)asI64(args[14]);
    int vertexStride = (int)asI64(args[15]);

    if (terrainSize < 1 || vertexStride < 2 || vertexStride != terrainSize + 1) {
        runtimeError(vm, "%s received inconsistent terrain parameters.", name);
        return makeVoid();
    }

    int vertexCount = vertexStride * vertexStride;
    if (heightsUpper < vertexCount - 1 || vertexUpper < vertexCount - 1) {
        runtimeError(vm, "%s vertex arrays are smaller than the required vertex count.", name);
        return makeVoid();
    }

    if (!sanityCheckNumericArray(vm, &sourceHeights, vertexCount, name, "source heights")) {
        return makeVoid();
    }

    bool usedProcedural = false;
#ifdef SDL
    if (gProceduralTerrain.enabled) {
        ensureProceduralGenerator();
        TerrainGeneratorConfig config = gProceduralTerrain.config;
        if (config.octaves < 1) config.octaves = 1;
        if (terrainGeneratorGenerate(&gProceduralTerrain.generator,
                                     terrainSize,
                                     (float)minHeight,
                                     (float)maxHeight,
                                     (float)waterLevel,
                                     (float)tileScale,
                                     &config)) {
            const TerrainVertex* vertices = terrainGeneratorVertices(&gProceduralTerrain.generator);
            size_t count = terrainGeneratorVertexCount(&gProceduralTerrain.generator);
            if (vertices && count == (size_t)vertexCount) {
                for (size_t i = 0; i < count; ++i) {
                    const TerrainVertex* v = &vertices[i];
                    assignFloatValue(&sourceHeights.values[i], v->position[1]);
                    assignFloatValue(&vertexHeights.values[i], v->position[1]);
                    assignFloatValue(&vertexNormalX.values[i], v->normal[0]);
                    assignFloatValue(&vertexNormalY.values[i], v->normal[1]);
                    assignFloatValue(&vertexNormalZ.values[i], v->normal[2]);
                    assignFloatValue(&vertexColorR.values[i], v->color[0]);
                    assignFloatValue(&vertexColorG.values[i], v->color[1]);
                    assignFloatValue(&vertexColorB.values[i], v->color[2]);
                }
                usedProcedural = true;
                gProceduralTerrain.lastResolution = terrainSize;
            }
        }
    }
#endif

    double span = maxHeight - minHeight;
    if (span <= 0.0001) span = 1.0;
    double waterHeight = minHeight + span * waterLevel;
    assignNumericVar(&waterHeightRef, waterHeight);

    double safeNormScale = normalizationScale;
    if (safeNormScale <= 0.0) safeNormScale = 0.0;

    double twoTileScale = tileScale * 2.0;
    bool safeScale = fabs(twoTileScale) > 1e-6;

    if (!usedProcedural) {
        for (int z = 0; z <= terrainSize; ++z) {
            for (int x = 0; x <= terrainSize; ++x) {
                int idx = z * vertexStride + x;
                double height = asLd(sourceHeights.values[idx]);
                assignFloatValue(&vertexHeights.values[idx], height);

                float left = landscapeHeightAt(sourceHeights.values, vertexStride, terrainSize, x - 1, z);
                float right = landscapeHeightAt(sourceHeights.values, vertexStride, terrainSize, x + 1, z);
                float down = landscapeHeightAt(sourceHeights.values, vertexStride, terrainSize, x, z - 1);
                float up = landscapeHeightAt(sourceHeights.values, vertexStride, terrainSize, x, z + 1);

                float dx = 0.0f;
                float dz = 0.0f;
                if (safeScale) {
                    dx = (float)((right - left) / twoTileScale);
                    dz = (float)((up - down) / twoTileScale);
                }

                float nx = -dx;
                float ny = 1.0f;
                float nz = -dz;
                float length = sqrtf(nx * nx + ny * ny + nz * nz);
                if (length <= 0.0001f) length = 1.0f;
                nx /= length;
                ny /= length;
                nz /= length;

                assignFloatValue(&vertexNormalX.values[idx], nx);
                assignFloatValue(&vertexNormalY.values[idx], ny);
                assignFloatValue(&vertexNormalZ.values[idx], nz);

                double normalized = 0.0;
                if (safeNormScale > 0.0) {
                    normalized = (height - minHeight) * safeNormScale;
                    if (normalized < 0.0) normalized = 0.0;
                    if (normalized > 1.0) normalized = 1.0;
                }

                float t = (float)normalized;
                float slope = clampf(1.0f - ny, 0.0f, 1.0f);
                float paletteColor[3];
                terrainShaderSampleGradient(t, (float)waterLevel, slope, paletteColor);
                assignFloatValue(&vertexColorR.values[idx], paletteColor[0]);
                assignFloatValue(&vertexColorG.values[idx], paletteColor[1]);
                assignFloatValue(&vertexColorB.values[idx], paletteColor[2]);
            }
        }
    }

    if (sourceHeights.owner && sourceHeights.owner->array_val != sourceHeights.values) {
        for (int i = 0; i < vertexCount; ++i) {
            sourceHeights.owner->array_val[i] = sourceHeights.values[i];
        }
    }
    if (vertexHeights.owner && vertexHeights.owner->array_val != vertexHeights.values) {
        for (int i = 0; i < vertexCount; ++i) {
            vertexHeights.owner->array_val[i] = vertexHeights.values[i];
        }
    }
    if (vertexNormalX.owner && vertexNormalX.owner->array_val != vertexNormalX.values) {
        for (int i = 0; i < vertexCount; ++i) {
            vertexNormalX.owner->array_val[i] = vertexNormalX.values[i];
        }
    }
    if (vertexNormalY.owner && vertexNormalY.owner->array_val != vertexNormalY.values) {
        for (int i = 0; i < vertexCount; ++i) {
            vertexNormalY.owner->array_val[i] = vertexNormalY.values[i];
        }
    }
    if (vertexNormalZ.owner && vertexNormalZ.owner->array_val != vertexNormalZ.values) {
        for (int i = 0; i < vertexCount; ++i) {
            vertexNormalZ.owner->array_val[i] = vertexNormalZ.values[i];
        }
    }
    if (vertexColorR.owner && vertexColorR.owner->array_val != vertexColorR.values) {
        for (int i = 0; i < vertexCount; ++i) {
            vertexColorR.owner->array_val[i] = vertexColorR.values[i];
        }
    }
    if (vertexColorG.owner && vertexColorG.owner->array_val != vertexColorG.values) {
        for (int i = 0; i < vertexCount; ++i) {
            vertexColorG.owner->array_val[i] = vertexColorG.values[i];
        }
    }
    if (vertexColorB.owner && vertexColorB.owner->array_val != vertexColorB.values) {
        for (int i = 0; i < vertexCount; ++i) {
            vertexColorB.owner->array_val[i] = vertexColorB.values[i];
        }
    }

    if (!sanityCheckNumericArray(vm, &vertexHeights, vertexCount, name, "vertex heights") ||
        !sanityCheckNormalArrays(vm, &vertexNormalX, &vertexNormalY, &vertexNormalZ, vertexCount, name) ||
        !sanityCheckColorArray(vm, &vertexColorR, vertexCount, name, "vertex color R") ||
        !sanityCheckColorArray(vm, &vertexColorG, vertexCount, name, "vertex color G") ||
        !sanityCheckColorArray(vm, &vertexColorB, vertexCount, name, "vertex color B")) {
        return makeVoid();
    }

    return makeVoid();
}

static void buildPresetList(size_t count,
                            const char *(*labelFn)(size_t),
                            char *buffer,
                            size_t bufferSize) {
    if (!buffer || bufferSize == 0) return;
    buffer[0] = '\0';
    size_t written = 0;
    for (size_t i = 0; i < count; ++i) {
        const char *label = labelFn ? labelFn(i) : NULL;
        if (!label) continue;
        int n;
        if (written > 0) {
            n = snprintf(buffer + written, bufferSize - written, ", %s", label);
        } else {
            n = snprintf(buffer + written, bufferSize - written, "%s", label);
        }
        if (n < 0) {
            break;
        }
        if ((size_t)n >= bufferSize - written) {
            written = bufferSize - 1;
            buffer[written] = '\0';
            break;
        }
        written += (size_t)n;
    }
}

static bool parsePalettePresetValue(VM* vm,
                                    const Value* value,
                                    TerrainPalettePreset* preset,
                                    const char* name) {
    if (!value || !preset) return false;
    if (IS_INTLIKE(*value) || value->type == TYPE_BOOLEAN) {
        long long idx = asI64(*value);
        if (idx < 0 || idx >= (long long)terrainShaderPalettePresetCount()) {
            char options[128];
            buildPresetList(terrainShaderPalettePresetCount(), terrainShaderPalettePresetLabel, options, sizeof(options));
            runtimeError(vm,
                         "%s received palette index %lld out of range. Valid presets: %s.",
                         name,
                         idx,
                         options);
            return false;
        }
        *preset = (TerrainPalettePreset)idx;
        return true;
    }
    if (isRealType(value->type)) {
        long long idx = (long long)asLd(*value);
        if (idx < 0 || idx >= (long long)terrainShaderPalettePresetCount()) {
            char options[128];
            buildPresetList(terrainShaderPalettePresetCount(), terrainShaderPalettePresetLabel, options, sizeof(options));
            runtimeError(vm,
                         "%s received palette index %lld out of range. Valid presets: %s.",
                         name,
                         idx,
                         options);
            return false;
        }
        *preset = (TerrainPalettePreset)idx;
        return true;
    }

    const char* label = landscapeValueToCString(value);
    if (!label) {
        runtimeError(vm, "%s expects an integer preset index or preset name.", name);
        return false;
    }
    if (!terrainShaderPalettePresetFromName(label, preset)) {
        char options[128];
        buildPresetList(terrainShaderPalettePresetCount(), terrainShaderPalettePresetLabel, options, sizeof(options));
        runtimeError(vm,
                     "%s received unknown palette preset '%s'. Valid presets: %s.",
                     name,
                     label,
                     options);
        return false;
    }
    return true;
}

static bool parseLightingPresetValue(VM* vm,
                                     const Value* value,
                                     TerrainLightingPreset* preset,
                                     const char* name) {
    if (!value || !preset) return false;
    if (IS_INTLIKE(*value) || value->type == TYPE_BOOLEAN) {
        long long idx = asI64(*value);
        if (idx < 0 || idx >= (long long)terrainShaderLightingPresetCount()) {
            char options[128];
            buildPresetList(terrainShaderLightingPresetCount(), terrainShaderLightingPresetLabel, options, sizeof(options));
            runtimeError(vm,
                         "%s received lighting index %lld out of range. Valid presets: %s.",
                         name,
                         idx,
                         options);
            return false;
        }
        *preset = (TerrainLightingPreset)idx;
        return true;
    }
    if (isRealType(value->type)) {
        long long idx = (long long)asLd(*value);
        if (idx < 0 || idx >= (long long)terrainShaderLightingPresetCount()) {
            char options[128];
            buildPresetList(terrainShaderLightingPresetCount(), terrainShaderLightingPresetLabel, options, sizeof(options));
            runtimeError(vm,
                         "%s received lighting index %lld out of range. Valid presets: %s.",
                         name,
                         idx,
                         options);
            return false;
        }
        *preset = (TerrainLightingPreset)idx;
        return true;
    }

    const char* label = landscapeValueToCString(value);
    if (!label) {
        runtimeError(vm, "%s expects an integer preset index or preset name.", name);
        return false;
    }
    if (!terrainShaderLightingPresetFromName(label, preset)) {
        char options[128];
        buildPresetList(terrainShaderLightingPresetCount(), terrainShaderLightingPresetLabel, options, sizeof(options));
        runtimeError(vm,
                     "%s received unknown lighting preset '%s'. Valid presets: %s.",
                     name,
                     label,
                     options);
        return false;
    }
    return true;
}

static Value vmBuiltinLandscapeDrawTerrain(VM* vm, int arg_count, Value* args) {
    const char* name = "LandscapeDrawTerrain";
    if (arg_count != 8 && arg_count != 11) {
        runtimeError(vm, "%s expects 8 or 11 arguments.", name);
        return makeVoid();
    }

    int lower = 0, upper = 0;
    ArrayArg vertexHeights = resolveArrayArg(vm, &args[0], name, &lower, &upper);
    if (!vertexHeights.values) return makeVoid();
    if (vertexHeights.lower != 0) {
        runtimeError(vm, "%s requires vertex height arrays starting at index 0.", name);
        return makeVoid();
    }
    int heightsUpper = vertexHeights.upper;

    ArrayArg vertexColorR = resolveArrayArg(vm, &args[1], name, &lower, &upper);
    if (!vertexColorR.values) return makeVoid();
    if (vertexColorR.lower != 0) {
        runtimeError(vm, "%s requires vertex color arrays starting at index 0.", name);
        return makeVoid();
    }
    int colorUpper = vertexColorR.upper;

    ArrayArg vertexColorG = resolveArrayArg(vm, &args[2], name, NULL, &upper);
    if (!vertexColorG.values) return makeVoid();
    if (vertexColorG.upper < colorUpper) colorUpper = vertexColorG.upper;

    ArrayArg vertexColorB = resolveArrayArg(vm, &args[3], name, NULL, &upper);
    if (!vertexColorB.values) return makeVoid();
    if (vertexColorB.upper < colorUpper) colorUpper = vertexColorB.upper;

    int argIndex = 4;
    ArrayArg vertexNormalX = {0};
    ArrayArg vertexNormalY = {0};
    ArrayArg vertexNormalZ = {0};
    bool hasNormals = arg_count == 11;
    if (hasNormals) {
        vertexNormalX = resolveArrayArg(vm, &args[argIndex], name, NULL, &upper);
        if (!vertexNormalX.values) return makeVoid();
        vertexNormalY = resolveArrayArg(vm, &args[argIndex + 1], name, NULL, &upper);
        if (!vertexNormalY.values) return makeVoid();
        vertexNormalZ = resolveArrayArg(vm, &args[argIndex + 2], name, NULL, &upper);
        if (!vertexNormalZ.values) return makeVoid();
        argIndex += 3;
    }

    ArrayArg worldXCoords = resolveArrayArg(vm, &args[argIndex], name, &lower, &upper);
    if (!worldXCoords.values) return makeVoid();
    if (worldXCoords.lower != 0) {
        runtimeError(vm, "%s requires coordinate arrays starting at index 0.", name);
        return makeVoid();
    }
    int worldUpper = worldXCoords.upper;

    ArrayArg worldZCoords = resolveArrayArg(vm, &args[argIndex + 1], name, NULL, &upper);
    if (!worldZCoords.values) return makeVoid();
    if (worldZCoords.upper < worldUpper) worldUpper = worldZCoords.upper;

    int terrainArgs = argIndex + 2;
    if (!IS_INTLIKE(args[terrainArgs]) || !IS_INTLIKE(args[terrainArgs + 1])) {
        runtimeError(vm, "%s expects integer TerrainSize and VertexStride arguments.", name);
        return makeVoid();
    }
    int terrainSize = (int)asI64(args[terrainArgs]);
    int vertexStride = (int)asI64(args[terrainArgs + 1]);
    if (terrainSize < 1 || vertexStride < 2 || vertexStride != terrainSize + 1) {
        runtimeError(vm, "%s received inconsistent terrain parameters.", name);
        return makeVoid();
    }

    int vertexCount = vertexStride * vertexStride;
    if (heightsUpper < vertexCount - 1 || colorUpper < vertexCount - 1) {
        runtimeError(vm, "%s vertex arrays are smaller than the required vertex count.", name);
        return makeVoid();
    }
    if (hasNormals) {
        if (vertexNormalX.lower != 0 || vertexNormalY.lower != 0 || vertexNormalZ.lower != 0) {
            runtimeError(vm, "%s requires normal arrays starting at index 0.", name);
            return makeVoid();
        }
        if (vertexNormalX.upper < vertexCount - 1 || vertexNormalY.upper < vertexCount - 1 ||
            vertexNormalZ.upper < vertexCount - 1) {
            runtimeError(vm, "%s normal arrays are smaller than the required vertex count.", name);
            return makeVoid();
        }
    }
    if (worldUpper < vertexStride - 1) {
        runtimeError(vm, "%s coordinate arrays are smaller than the required vertex stride.", name);
        return makeVoid();
    }

    if (!sanityCheckNumericArray(vm, &vertexHeights, vertexCount, name, "vertex heights") ||
        !sanityCheckColorArray(vm, &vertexColorR, vertexCount, name, "vertex color R") ||
        !sanityCheckColorArray(vm, &vertexColorG, vertexCount, name, "vertex color G") ||
        !sanityCheckColorArray(vm, &vertexColorB, vertexCount, name, "vertex color B") ||
        !sanityCheckMonotonic(vm, &worldXCoords, vertexStride, name, "world X coordinates") ||
        !sanityCheckMonotonic(vm, &worldZCoords, vertexStride, name, "world Z coordinates")) {
        return makeVoid();
    }
    if (hasNormals &&
        !sanityCheckNormalArrays(vm, &vertexNormalX, &vertexNormalY, &vertexNormalZ, vertexCount, name)) {
        return makeVoid();
    }

#ifdef SDL
    if (!ensureGlContext(vm, name)) {
        return makeVoid();
    }

    if (gProceduralTerrain.enabled && gProceduralTerrain.initialised &&
        gProceduralTerrain.lastResolution == terrainSize &&
        terrainGeneratorVertexCount(&gProceduralTerrain.generator) == (size_t)vertexCount) {
        if (terrainGeneratorEnsureUploaded(&gProceduralTerrain.generator)) {
            terrainGeneratorDraw(&gProceduralTerrain.generator);
            return makeVoid();
        }
    }

    for (int z = 0; z < terrainSize; ++z) {
        int rowIndex = z * vertexStride;
        int nextRowIndex = (z + 1) * vertexStride;
        float worldZ0 = (float)asLd(worldZCoords.values[z]);
        float worldZ1 = (float)asLd(worldZCoords.values[z + 1]);

        glBegin(GL_TRIANGLE_STRIP);
        for (int x = 0; x <= terrainSize; ++x) {
            int idx0 = rowIndex + x;
            int idx1 = nextRowIndex + x;
            float worldX = (float)asLd(worldXCoords.values[x]);

            float nx0, ny0, nz0;
            float nx1, ny1, nz1;
            if (hasNormals) {
                nx0 = (float)asLd(vertexNormalX.values[idx0]);
                ny0 = (float)asLd(vertexNormalY.values[idx0]);
                nz0 = (float)asLd(vertexNormalZ.values[idx0]);
                nx1 = (float)asLd(vertexNormalX.values[idx1]);
                ny1 = (float)asLd(vertexNormalY.values[idx1]);
                nz1 = (float)asLd(vertexNormalZ.values[idx1]);
            } else {
                computeTerrainNormal(&vertexHeights, &worldXCoords, &worldZCoords,
                                     vertexStride, terrainSize, x, z, &nx0, &ny0, &nz0);
                computeTerrainNormal(&vertexHeights, &worldXCoords, &worldZCoords,
                                     vertexStride, terrainSize, x, z + 1, &nx1, &ny1, &nz1);
            }

            float r0 = (float)asLd(vertexColorR.values[idx0]);
            float g0 = (float)asLd(vertexColorG.values[idx0]);
            float b0 = (float)asLd(vertexColorB.values[idx0]);
            float h0 = (float)asLd(vertexHeights.values[idx0]);

            float r1 = (float)asLd(vertexColorR.values[idx1]);
            float g1 = (float)asLd(vertexColorG.values[idx1]);
            float b1 = (float)asLd(vertexColorB.values[idx1]);
            float h1 = (float)asLd(vertexHeights.values[idx1]);

            glNormal3f(nx0, ny0, nz0);
            glColor3f(r0, g0, b0);
            glVertex3f(worldX, h0, worldZ0);

            glNormal3f(nx1, ny1, nz1);
            glColor3f(r1, g1, b1);
            glVertex3f(worldX, h1, worldZ1);
        }
        glEnd();
    }
#else
    runtimeError(vm, "%s requires SDL/OpenGL support.", name);
    return makeVoid();
#endif

    return makeVoid();
}

static Value vmBuiltinLandscapeDrawWater(VM* vm, int arg_count, Value* args) {
    const char* name = "LandscapeDrawWater";
    if (arg_count != 10) {
        runtimeError(vm, "%s expects 10 arguments.", name);
        return makeVoid();
    }

    int lower = 0, upper = 0;
    ArrayArg vertexHeights = resolveArrayArg(vm, &args[0], name, &lower, &upper);
    if (!vertexHeights.values) return makeVoid();
    if (vertexHeights.lower != 0) {
        runtimeError(vm, "%s requires vertex arrays starting at index 0.", name);
        return makeVoid();
    }
    int heightsUpper = vertexHeights.upper;

    ArrayArg worldXCoords = resolveArrayArg(vm, &args[1], name, &lower, &upper);
    if (!worldXCoords.values) return makeVoid();
    if (worldXCoords.lower != 0) {
        runtimeError(vm, "%s requires coordinate arrays starting at index 0.", name);
        return makeVoid();
    }
    int coordUpper = worldXCoords.upper;

    ArrayArg worldZCoords = resolveArrayArg(vm, &args[2], name, NULL, &upper);
    if (!worldZCoords.values) return makeVoid();
    if (worldZCoords.upper < coordUpper) coordUpper = worldZCoords.upper;

    ArrayArg waterPhaseOffset = resolveArrayArg(vm, &args[3], name, NULL, &upper);
    if (!waterPhaseOffset.values) return makeVoid();
    int phaseUpper = waterPhaseOffset.upper;

    ArrayArg waterSecondaryOffset = resolveArrayArg(vm, &args[4], name, NULL, &upper);
    if (!waterSecondaryOffset.values) return makeVoid();
    if (waterSecondaryOffset.upper < phaseUpper) phaseUpper = waterSecondaryOffset.upper;

    ArrayArg waterSparkleOffset = resolveArrayArg(vm, &args[5], name, NULL, &upper);
    if (!waterSparkleOffset.values) return makeVoid();
    if (waterSparkleOffset.upper < phaseUpper) phaseUpper = waterSparkleOffset.upper;

    if (!isRealType(args[6].type) && !IS_INTLIKE(args[6])) {
        runtimeError(vm, "%s expects numeric water height.", name);
        return makeVoid();
    }
    if (!isRealType(args[7].type) && !IS_INTLIKE(args[7])) {
        runtimeError(vm, "%s expects numeric time parameter.", name);
        return makeVoid();
    }
    if (!IS_INTLIKE(args[8]) || !IS_INTLIKE(args[9])) {
        runtimeError(vm, "%s expects integer terrain parameters.", name);
        return makeVoid();
    }

    double waterHeight = (double)asLd(args[6]);
    double timeSeconds = (double)asLd(args[7]);
    int terrainSize = (int)asI64(args[8]);
    int vertexStride = (int)asI64(args[9]);

    if (!isfinite(waterHeight) || !isfinite(timeSeconds)) {
        runtimeError(vm, "%s received non-finite parameters.", name);
        return makeVoid();
    }

    if (terrainSize < 1 || vertexStride < 2 || vertexStride != terrainSize + 1) {
        runtimeError(vm, "%s received inconsistent terrain parameters.", name);
        return makeVoid();
    }

    int vertexCount = vertexStride * vertexStride;
    if (heightsUpper < vertexCount - 1 || phaseUpper < vertexCount - 1) {
        runtimeError(vm, "%s arrays are smaller than the required vertex count.", name);
        return makeVoid();
    }
    if (coordUpper < vertexStride - 1) {
        runtimeError(vm, "%s coordinate arrays are smaller than the required vertex stride.", name);
        return makeVoid();
    }

    if (!sanityCheckNumericArray(vm, &vertexHeights, vertexCount, name, "vertex heights") ||
        !sanityCheckMonotonic(vm, &worldXCoords, vertexStride, name, "world X coordinates") ||
        !sanityCheckMonotonic(vm, &worldZCoords, vertexStride, name, "world Z coordinates") ||
        !sanityCheckNumericArray(vm, &waterPhaseOffset, vertexCount, name, "water phase offsets") ||
        !sanityCheckNumericArray(vm, &waterSecondaryOffset, vertexCount, name,
                                 "water secondary offsets") ||
        !sanityCheckNumericArray(vm, &waterSparkleOffset, vertexCount, name,
                                 "water sparkle offsets")) {
        return makeVoid();
    }

#ifdef SDL
    if (!ensureGlContext(vm, name)) {
        return makeVoid();
    }

    float allowance = 0.18f;
    float maxWaterHeight = (float)(waterHeight + allowance);
    double basePhase = timeSeconds * 0.7;
    double baseSecondary = timeSeconds * 1.6;
    double baseSparkle = timeSeconds * 2.4;

    glBegin(GL_TRIANGLES);
    for (int z = 0; z < terrainSize; ++z) {
        int rowIndex = z * vertexStride;
        int nextRowIndex = (z + 1) * vertexStride;
        for (int x = 0; x < terrainSize; ++x) {
            int idx00 = rowIndex + x;
            int idx10 = rowIndex + x + 1;
            int idx01 = nextRowIndex + x;
            int idx11 = nextRowIndex + x + 1;

            float h00 = (float)asLd(vertexHeights.values[idx00]);
            float h10 = (float)asLd(vertexHeights.values[idx10]);
            float h01 = (float)asLd(vertexHeights.values[idx01]);
            float h11 = (float)asLd(vertexHeights.values[idx11]);

            if (h00 <= maxWaterHeight && h10 <= maxWaterHeight && h01 <= maxWaterHeight) {
                emitWaterVertexGl(&worldXCoords, &worldZCoords, &waterPhaseOffset,
                                  &waterSecondaryOffset, &waterSparkleOffset, waterHeight,
                                  basePhase, baseSecondary, baseSparkle, x, z, idx00, h00);
                emitWaterVertexGl(&worldXCoords, &worldZCoords, &waterPhaseOffset,
                                  &waterSecondaryOffset, &waterSparkleOffset, waterHeight,
                                  basePhase, baseSecondary, baseSparkle, x + 1, z, idx10, h10);
                emitWaterVertexGl(&worldXCoords, &worldZCoords, &waterPhaseOffset,
                                  &waterSecondaryOffset, &waterSparkleOffset, waterHeight,
                                  basePhase, baseSecondary, baseSparkle, x, z + 1, idx01, h01);
            }
            if (h10 <= maxWaterHeight && h11 <= maxWaterHeight && h01 <= maxWaterHeight) {
                emitWaterVertexGl(&worldXCoords, &worldZCoords, &waterPhaseOffset,
                                  &waterSecondaryOffset, &waterSparkleOffset, waterHeight,
                                  basePhase, baseSecondary, baseSparkle, x + 1, z, idx10, h10);
                emitWaterVertexGl(&worldXCoords, &worldZCoords, &waterPhaseOffset,
                                  &waterSecondaryOffset, &waterSparkleOffset, waterHeight,
                                  basePhase, baseSecondary, baseSparkle, x + 1, z + 1, idx11,
                                  h11);
                emitWaterVertexGl(&worldXCoords, &worldZCoords, &waterPhaseOffset,
                                  &waterSecondaryOffset, &waterSparkleOffset, waterHeight,
                                  basePhase, baseSecondary, baseSparkle, x, z + 1, idx01, h01);
            }
        }
    }
    glEnd();
#else
    runtimeError(vm, "%s requires SDL/OpenGL support.", name);
    return makeVoid();
#endif

    return makeVoid();
}

static Value vmBuiltinLandscapeSetPalettePreset(VM* vm, int arg_count, Value* args) {
    const char* name = "LandscapeSetPalettePreset";
    if (arg_count != 1) {
        runtimeError(vm, "%s expects 1 argument.", name);
        return makeVoid();
    }

    TerrainPalettePreset preset;
    if (!parsePalettePresetValue(vm, &args[0], &preset, name)) {
        return makeVoid();
    }

    terrainShaderSetPalettePreset(preset);
    return makeVoid();
}

static Value vmBuiltinLandscapeSetLightingPreset(VM* vm, int arg_count, Value* args) {
    const char* name = "LandscapeSetLightingPreset";
    if (arg_count != 1) {
        runtimeError(vm, "%s expects 1 argument.", name);
        return makeVoid();
    }

    TerrainLightingPreset preset;
    if (!parseLightingPresetValue(vm, &args[0], &preset, name)) {
        return makeVoid();
    }

    terrainShaderSetLightingPreset(preset);
    return makeVoid();
}

#ifdef SDL
static Value vmBuiltinLandscapeDrawCloudLayer(VM* vm, int arg_count, Value* args) {
    const char* name = "LandscapeDrawCloudLayer";
    if (arg_count != 10) {
        runtimeError(vm, "%s expects 10 arguments.", name);
        return makeVoid();
    }

    float values[10];
    for (int i = 0; i < 10; ++i) {
        if (!valueToFloat32(args[i], &values[i])) {
            runtimeError(vm, "%s argument %d must be numeric.", name, i + 1);
            return makeVoid();
        }
    }

    if (!ensureGlContext(vm, name)) return makeVoid();

    CloudLayerParams params;
    memset(&params, 0, sizeof(params));
    params.timeSeconds = values[0];
    params.cameraOffsetX = values[1];
    params.cameraOffsetZ = values[2];
    params.parallaxScale = values[3];
    params.coverage = values[4];
    params.softness = values[5];
    params.dayFactor = values[6];
    params.sunDirection[0] = values[7];
    params.sunDirection[1] = values[8];
    params.sunDirection[2] = values[9];

    if (!cloudLayerRendererDraw(&gCloudLayer.renderer, &params)) {
        runtimeError(vm, "%s failed to render the cloud layer.", name);
    }
    return makeVoid();
}

static Value vmBuiltinLandscapeDrawSkyDome(VM* vm, int arg_count, Value* args) {
    const char* name = "LandscapeDrawSkyDome";
    if (arg_count != 0 && arg_count != 1) {
        runtimeError(vm, "%s expects 0 or 1 argument.", name);
        return makeVoid();
    }

    if (!ensureGlContext(vm, name)) return makeVoid();

    ensureSkyDomeState();

    float radius = 500.0f;
    if (arg_count == 1) {
        if (!valueToFloat32(args[0], &radius)) {
            runtimeError(vm, "%s expects a numeric radius.", name);
            return makeVoid();
        }
    }

    if (!skyDomeEnsureUploaded(&gSkyDome.dome, 32, 16)) {
        runtimeError(vm, "%s could not initialise sky dome geometry.", name);
        return makeVoid();
    }

    const float* horizon = terrainShaderSkyHorizonColor();
    const float* zenith = terrainShaderSkyZenithColor();
    skyDomeDraw(&gSkyDome.dome, radius, horizon, zenith);
    return makeVoid();
}
#endif

void registerLandscapeBuiltins(void) {
    registerVmBuiltin("landscapeconfigureprocedural", vmBuiltinLandscapeConfigureProcedural,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeConfigureProcedural");
    registerVmBuiltin("landscapedrawterrain", vmBuiltinLandscapeDrawTerrain,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeDrawTerrain");
    registerVmBuiltin("landscapedrawwater", vmBuiltinLandscapeDrawWater,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeDrawWater");
    registerVmBuiltin("landscapeprecomputeworldcoords", vmBuiltinLandscapePrecomputeWorldCoords,
                      BUILTIN_TYPE_PROCEDURE, "LandscapePrecomputeWorldCoords");
    registerVmBuiltin("landscapeprecomputewateroffsets", vmBuiltinLandscapePrecomputeWaterOffsets,
                      BUILTIN_TYPE_PROCEDURE, "LandscapePrecomputeWaterOffsets");
    registerVmBuiltin("landscapebuildheightfield", vmBuiltinLandscapeBuildHeightField,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeBuildHeightField");
    registerVmBuiltin("landscapebakevertexdata", vmBuiltinLandscapeBakeVertexData,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeBakeVertexData");
    registerVmBuiltin("landscapesetpalettepreset", vmBuiltinLandscapeSetPalettePreset,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeSetPalettePreset");
    registerVmBuiltin("landscapesetlightingpreset", vmBuiltinLandscapeSetLightingPreset,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeSetLightingPreset");
#ifdef SDL
    registerVmBuiltin("landscapedrawskydome", vmBuiltinLandscapeDrawSkyDome,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeDrawSkyDome");
    registerVmBuiltin("landscapedrawcloudlayer", vmBuiltinLandscapeDrawCloudLayer,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeDrawCloudLayer");
#endif
}

#endif /* SDL && PSCAL_TARGET_IOS */
