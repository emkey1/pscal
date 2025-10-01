#include "backend_ast/builtin.h"
#include "backend_ast/sdl.h"
#include "core/utils.h"
#include "vm/vm.h"

#include <float.h>
#include <math.h>

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

static float saturatef(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

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

    double span = maxHeight - minHeight;
    if (span <= 0.0001) span = 1.0;
    double waterHeight = minHeight + span * waterLevel;
    assignNumericVar(&waterHeightRef, waterHeight);

    double safeNormScale = normalizationScale;
    if (safeNormScale <= 0.0) safeNormScale = 0.0;

    double twoTileScale = tileScale * 2.0;
    bool safeScale = fabs(twoTileScale) > 1e-6;

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
            float r, g, b;
            bool underwater = t < (float)waterLevel;
            if (underwater) {
                float denom = (float)waterLevel;
                float depth = 0.0f;
                if (denom > 1e-6f) {
                    depth = (float)((waterLevel - normalized) / waterLevel);
                }
                depth = clampf(depth, 0.0f, 1.0f);
                float shore = 1.0f - depth;
                r = 0.05f + 0.08f * depth + 0.10f * shore;
                g = 0.32f + 0.36f * depth + 0.18f * shore;
                b = 0.52f + 0.40f * depth + 0.12f * shore;
            } else if (t < (float)(waterLevel + 0.06)) {
                float w = (t - (float)waterLevel) / 0.06f;
                r = 0.36f + 0.14f * w;
                g = 0.34f + 0.20f * w;
                b = 0.20f + 0.09f * w;
            } else if (t < 0.62f) {
                float w = (t - (float)(waterLevel + 0.06)) / 0.16f;
                r = 0.24f + 0.18f * w;
                g = 0.46f + 0.32f * w;
                b = 0.22f + 0.12f * w;
            } else if (t < 0.82f) {
                float w = (t - 0.62f) / 0.20f;
                r = 0.46f + 0.26f * w;
                g = 0.40f + 0.22f * w;
                b = 0.30f + 0.20f * w;
            } else {
                float w = (t - 0.82f) / 0.18f;
                w = clampf(w, 0.0f, 1.0f);
                float base = 0.84f + 0.14f * w;
                r = base;
                g = base;
                b = base;
                float frost = saturatef((t - 0.88f) / 0.12f);
                float sunSpark = 0.75f + 0.25f * frost;
                r = lerpf(r, sunSpark, frost * 0.4f);
                g = lerpf(g, sunSpark, frost * 0.4f);
                b = lerpf(b, sunSpark, frost * 0.6f);
            }

            if (!underwater) {
                float slope = 1.0f - ny;
                slope = clampf(slope, 0.0f, 1.0f);
                float cool = saturatef((0.58f - t) * 3.5f);
                g += cool * 0.04f;
                b += cool * 0.06f;
                float alpine = saturatef((t - 0.68f) * 2.2f);
                r = lerpf(r, r * 0.92f, alpine * 0.3f);
                g = lerpf(g, g * 0.90f, alpine * 0.26f);
                b = lerpf(b, b * 1.05f, alpine * 0.24f);
                float slopeTint = slope * 0.6f;
                r = lerpf(r, r * 0.78f, slopeTint);
                g = lerpf(g, g * 0.74f, slopeTint);
                b = lerpf(b, b * 0.86f, slopeTint);
            }

            r = saturatef(r);
            g = saturatef(g);
            b = saturatef(b);

            assignFloatValue(&vertexColorR.values[idx], r);
            assignFloatValue(&vertexColorG.values[idx], g);
            assignFloatValue(&vertexColorB.values[idx], b);
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

    return makeVoid();
}

#ifdef SDL
#include <SDL2/SDL_opengl.h>

static bool ensureGlContext(VM* vm, const char* name) {
    if (!gSdlInitialized || !gSdlWindow || !gSdlGLContext) {
        runtimeError(vm, "%s requires an active OpenGL window. Call InitGraph3D first.", name);
        return false;
    }
    return true;
}

static bool rowVisible(const float* mvp, float xMin, float xMax, float z0, float z1,
                       float yMin, float yMax) {
    bool allRight = true, allLeft = true, allTop = true, allBottom = true;
    bool allFar = true, allNear = true;
    const float corners[8][3] = {
        {xMin, yMin, z0}, {xMax, yMin, z0}, {xMin, yMax, z0}, {xMax, yMax, z0},
        {xMin, yMin, z1}, {xMax, yMin, z1}, {xMin, yMax, z1}, {xMax, yMax, z1},
    };

    for (int i = 0; i < 8; ++i) {
        double x = corners[i][0];
        double y = corners[i][1];
        double z = corners[i][2];
        double clipX = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
        double clipY = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
        double clipZ = mvp[2] * x + mvp[6] * y + mvp[10] * z + mvp[14];
        double clipW = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
        if (clipW == 0.0) clipW = 1e-6;

        if (clipX <= clipW) allRight = false;
        if (clipX >= -clipW) allLeft = false;
        if (clipY <= clipW) allTop = false;
        if (clipY >= -clipW) allBottom = false;
        if (clipZ <= clipW) allFar = false;
        if (clipZ >= -clipW) allNear = false;
    }

    if (allRight || allLeft || allTop || allBottom || allFar || allNear) {
        return false;
    }
    return true;
}

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
    if (arg_count == 11) {
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
    if (vertexNormalX.values) {
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

    if (!ensureGlContext(vm, name)) {
        return makeVoid();
    }

    float modelview[16];
    float projection[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, modelview);
    glGetFloatv(GL_PROJECTION_MATRIX, projection);
    float mvp[16];
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += projection[k * 4 + row] * modelview[col * 4 + k];
            }
            mvp[col * 4 + row] = sum;
        }
    }

    float worldXMin = (float)asLd(worldXCoords.values[0]);
    float worldXMax = (float)asLd(worldXCoords.values[terrainSize]);

    for (int z = 0; z < terrainSize; ++z) {
        glBegin(GL_TRIANGLE_STRIP);
        float worldZ0 = (float)asLd(worldZCoords.values[z]);
        float worldZ1 = (float)asLd(worldZCoords.values[z + 1]);
        int rowIndex = z * vertexStride;
        int nextRowIndex = (z + 1) * vertexStride;

        float rowMin = FLT_MAX;
        float rowMax = -FLT_MAX;
        for (int x = 0; x <= terrainSize; ++x) {
            float h0 = (float)asLd(vertexHeights.values[rowIndex + x]);
            float h1 = (float)asLd(vertexHeights.values[nextRowIndex + x]);
            if (h0 < rowMin) rowMin = h0;
            if (h0 > rowMax) rowMax = h0;
            if (h1 < rowMin) rowMin = h1;
            if (h1 > rowMax) rowMax = h1;
        }

        if (!rowVisible(mvp, worldXMin, worldXMax, worldZ0, worldZ1, rowMin, rowMax)) {
            glEnd();
            continue;
        }

        for (int x = 0; x <= terrainSize; ++x) {
            int idx0 = rowIndex + x;
            int idx1 = nextRowIndex + x;
            float worldX = (float)asLd(worldXCoords.values[x]);

            float r0 = clampf((float)asLd(vertexColorR.values[idx0]), 0.0f, 1.0f);
            float g0 = clampf((float)asLd(vertexColorG.values[idx0]), 0.0f, 1.0f);
            float b0 = clampf((float)asLd(vertexColorB.values[idx0]), 0.0f, 1.0f);
            float h0 = (float)asLd(vertexHeights.values[idx0]);
            if (vertexNormalX.values) {
                glNormal3f((float)asLd(vertexNormalX.values[idx0]),
                           (float)asLd(vertexNormalY.values[idx0]),
                           (float)asLd(vertexNormalZ.values[idx0]));
            } else {
                float nx0, ny0, nz0;
                computeTerrainNormal(vertexHeights.values, worldXCoords.values, worldZCoords.values,
                                     vertexStride, terrainSize, x, z,
                                     &nx0, &ny0, &nz0);
                glNormal3f(nx0, ny0, nz0);
            }
            glColor3f(r0, g0, b0);
            glVertex3f(worldX, h0, worldZ0);

            float r1 = clampf((float)asLd(vertexColorR.values[idx1]), 0.0f, 1.0f);
            float g1 = clampf((float)asLd(vertexColorG.values[idx1]), 0.0f, 1.0f);
            float b1 = clampf((float)asLd(vertexColorB.values[idx1]), 0.0f, 1.0f);
            float h1 = (float)asLd(vertexHeights.values[idx1]);
            if (vertexNormalX.values) {
                glNormal3f((float)asLd(vertexNormalX.values[idx1]),
                           (float)asLd(vertexNormalY.values[idx1]),
                           (float)asLd(vertexNormalZ.values[idx1]));
            } else {
                float nx1, ny1, nz1;
                computeTerrainNormal(vertexHeights.values, worldXCoords.values, worldZCoords.values,
                                     vertexStride, terrainSize, x, z + 1,
                                     &nx1, &ny1, &nz1);
                glNormal3f(nx1, ny1, nz1);
            }
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
    ArrayArg vertexHeights = resolveArrayArg(vm, &args[0], "LandscapeDrawWater", &lower, &upper);
    if (!vertexHeights.values) return makeVoid();
    if (vertexHeights.lower != 0) {
        runtimeError(vm, "LandscapeDrawWater requires vertex arrays starting at index 0.");
        return makeVoid();
    }
    int heightsUpper = vertexHeights.upper;

    ArrayArg worldXCoords = resolveArrayArg(vm, &args[1], "LandscapeDrawWater", &lower, &upper);
    if (!worldXCoords.values) return makeVoid();
    if (worldXCoords.lower != 0) {
        runtimeError(vm, "LandscapeDrawWater requires coordinate arrays starting at index 0.");
        return makeVoid();
    }
    int coordUpper = worldXCoords.upper;

    ArrayArg worldZCoords = resolveArrayArg(vm, &args[2], "LandscapeDrawWater", NULL, &upper);
    if (!worldZCoords.values) return makeVoid();
    if (worldZCoords.upper < coordUpper) coordUpper = worldZCoords.upper;

    ArrayArg waterPhaseOffset = resolveArrayArg(vm, &args[3], "LandscapeDrawWater", NULL, &upper);
    if (!waterPhaseOffset.values) return makeVoid();
    int phaseUpper = waterPhaseOffset.upper;

    ArrayArg waterSecondaryOffset = resolveArrayArg(vm, &args[4], "LandscapeDrawWater", NULL, &upper);
    if (!waterSecondaryOffset.values) return makeVoid();
    if (waterSecondaryOffset.upper < phaseUpper) phaseUpper = waterSecondaryOffset.upper;

    ArrayArg waterSparkleOffset = resolveArrayArg(vm, &args[5], "LandscapeDrawWater", NULL, &upper);
    if (!waterSparkleOffset.values) return makeVoid();
    if (waterSparkleOffset.upper < phaseUpper) phaseUpper = waterSparkleOffset.upper;

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
        float worldZ0 = (float)asLd(worldZCoords.values[z]);
        float worldZ1 = (float)asLd(worldZCoords.values[z + 1]);
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
                float worldX0 = (float)asLd(worldXCoords.values[x]);
                float worldX1 = (float)asLd(worldXCoords.values[x + 1]);
                emitWaterVertex(waterHeight, basePhase, baseSecondary, baseSparkle,
                                worldX0, worldZ0, h00,
                                (float)asLd(waterPhaseOffset.values[idx00]),
                                (float)asLd(waterSecondaryOffset.values[idx00]),
                                (float)asLd(waterSparkleOffset.values[idx00]));
                emitWaterVertex(waterHeight, basePhase, baseSecondary, baseSparkle,
                                worldX1, worldZ0, h10,
                                (float)asLd(waterPhaseOffset.values[idx10]),
                                (float)asLd(waterSecondaryOffset.values[idx10]),
                                (float)asLd(waterSparkleOffset.values[idx10]));
                emitWaterVertex(waterHeight, basePhase, baseSecondary, baseSparkle,
                                worldX0, worldZ1, h01,
                                (float)asLd(waterPhaseOffset.values[idx01]),
                                (float)asLd(waterSecondaryOffset.values[idx01]),
                                (float)asLd(waterSparkleOffset.values[idx01]));
            }
            if (h10 <= maxWaterHeight && h11 <= maxWaterHeight && h01 <= maxWaterHeight) {
                float worldX1 = (float)asLd(worldXCoords.values[x + 1]);
                float worldX0 = (float)asLd(worldXCoords.values[x]);
                emitWaterVertex(waterHeight, basePhase, baseSecondary, baseSparkle,
                                worldX1, worldZ0, h10,
                                (float)asLd(waterPhaseOffset.values[idx10]),
                                (float)asLd(waterSecondaryOffset.values[idx10]),
                                (float)asLd(waterSparkleOffset.values[idx10]));
                emitWaterVertex(waterHeight, basePhase, baseSecondary, baseSparkle,
                                worldX1, worldZ1, h11,
                                (float)asLd(waterPhaseOffset.values[idx11]),
                                (float)asLd(waterSecondaryOffset.values[idx11]),
                                (float)asLd(waterSparkleOffset.values[idx11]));
                emitWaterVertex(waterHeight, basePhase, baseSecondary, baseSparkle,
                                worldX0, worldZ1, h01,
                                (float)asLd(waterPhaseOffset.values[idx01]),
                                (float)asLd(waterSecondaryOffset.values[idx01]),
                                (float)asLd(waterSparkleOffset.values[idx01]));
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
    registerVmBuiltin("landscapebuildheightfield", vmBuiltinLandscapeBuildHeightField,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeBuildHeightField");
    registerVmBuiltin("landscapebakevertexdata", vmBuiltinLandscapeBakeVertexData,
                      BUILTIN_TYPE_PROCEDURE, "LandscapeBakeVertexData");
}
