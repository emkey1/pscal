#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "vm/vm.h"

#ifdef SDL
#include "backend_ast/graphics_3d_backend.h"
#include "backend_ast/pscal_sdl_runtime.h"
#include "core/sdl_headers.h"
#include PSCALI_SDL_OPENGL_HEADER
#endif

#include <limits.h>
#include <math.h>
#include <stdlib.h>

static Value* resolveArrayArg(VM* vm, Value* arg, const char* name, int* lower,
                              int* upper) {
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
    int l = (arrVal->dimensions > 0 && arrVal->lower_bounds) ? arrVal->lower_bounds[0]
                                                             : arrVal->lower_bound;
    int u = (arrVal->dimensions > 0 && arrVal->upper_bounds) ? arrVal->upper_bounds[0]
                                                             : arrVal->upper_bound;
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
    target->type = TYPE_DOUBLE;
    SET_REAL_VALUE(target, value);
}

static inline double clampd(double value, double minVal, double maxVal) {
    if (value < minVal) return minVal;
    if (value > maxVal) return maxVal;
    return value;
}

static inline double enforceSpeed(double value, double minSpeed, double maxSpeed) {
    double absVal = fabs(value);
    if (absVal < minSpeed) {
        value = (value < 0.0) ? -minSpeed : minSpeed;
    }
    if (value > maxSpeed) value = maxSpeed;
    if (value < -maxSpeed) value = -maxSpeed;
    return value;
}

typedef struct Balls3DParams {
    int ballCount;
    double deltaTime;
    double boxWidth;
    double boxHeight;
    double boxDepth;
    double wallElasticity;
    double minSpeed;
    double maxSpeed;
    double drag;
    double cameraDistance;
    double screenWidth;
    double screenHeight;
    double lightDir[3];
    bool hasLight;
    Value* posX;
    Value* posY;
    Value* posZ;
    Value* velX;
    Value* velY;
    Value* velZ;
    Value* radius;
    Value* screenX;
    Value* screenY;
    Value* screenRadius;
    Value* depthShade;
    Value* lightIntensity;
    Value* rimIntensity;
    Value* highlightX;
    Value* highlightY;
    Value* highlightRadius;
    Value* highlightStrength;
} Balls3DParams;

static Value* fetchZeroBasedArray(VM* vm, Value* arg, const char* name,
                                  int* arrayUpper) {
    int lower = 0;
    int upper = 0;
    Value* arr = resolveArrayArg(vm, arg, name, &lower, &upper);
    if (!arr) return NULL;
    if (lower != 0) {
        runtimeError(vm, "%s arrays must start at index 0.", name);
        return NULL;
    }
    if (upper < *arrayUpper) *arrayUpper = upper;
    return arr;
}

typedef struct Balls3DWorkBuffers {
    int capacity;
    double* posX;
    double* posY;
    double* posZ;
    double* velX;
    double* velY;
    double* velZ;
    double* radius;
} Balls3DWorkBuffers;

static Balls3DWorkBuffers balls3dWorkBuffers = {0};

static void freeBalls3DWorkBuffers(Balls3DWorkBuffers* buffers) {
    if (!buffers) return;
    free(buffers->posX);
    free(buffers->posY);
    free(buffers->posZ);
    free(buffers->velX);
    free(buffers->velY);
    free(buffers->velZ);
    free(buffers->radius);
    buffers->capacity = 0;
    buffers->posX = NULL;
    buffers->posY = NULL;
    buffers->posZ = NULL;
    buffers->velX = NULL;
    buffers->velY = NULL;
    buffers->velZ = NULL;
    buffers->radius = NULL;
}

static bool ensureBalls3DWorkCapacity(int count) {
    if (count <= 0) return true;
    if (balls3dWorkBuffers.capacity >= count) return true;

    int newCapacity = balls3dWorkBuffers.capacity > 0 ? balls3dWorkBuffers.capacity : 16;
    while (newCapacity < count) {
        if (newCapacity > INT_MAX / 2) {
            newCapacity = count;
            break;
        }
        newCapacity *= 2;
    }

    double* posX = (double*)malloc(sizeof(double) * newCapacity);
    double* posY = (double*)malloc(sizeof(double) * newCapacity);
    double* posZ = (double*)malloc(sizeof(double) * newCapacity);
    double* velX = (double*)malloc(sizeof(double) * newCapacity);
    double* velY = (double*)malloc(sizeof(double) * newCapacity);
    double* velZ = (double*)malloc(sizeof(double) * newCapacity);
    double* radius = (double*)malloc(sizeof(double) * newCapacity);

    if (!posX || !posY || !posZ || !velX || !velY || !velZ || !radius) {
        free(posX);
        free(posY);
        free(posZ);
        free(velX);
        free(velY);
        free(velZ);
        free(radius);
        return false;
    }

    freeBalls3DWorkBuffers(&balls3dWorkBuffers);

    balls3dWorkBuffers.capacity = newCapacity;
    balls3dWorkBuffers.posX = posX;
    balls3dWorkBuffers.posY = posY;
    balls3dWorkBuffers.posZ = posZ;
    balls3dWorkBuffers.velX = velX;
    balls3dWorkBuffers.velY = velY;
    balls3dWorkBuffers.velZ = velZ;
    balls3dWorkBuffers.radius = radius;
    return true;
}

typedef struct NumericVarRef {
    Value* slot;
    bool isInteger;
} NumericVarRef;

static bool fetchNumericVarRef(VM* vm, Value* arg, const char* name,
                               const char* paramDesc, NumericVarRef* out) {
    if (!out) return false;
    if (arg->type != TYPE_POINTER) {
        runtimeError(vm, "%s expects VAR parameter for %s.", name, paramDesc);
        return false;
    }

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

#ifdef SDL
#ifndef GL_TRIANGLE_STRIP
#define GL_TRIANGLE_STRIP 0x0005u
#endif
#ifndef GL_COMPILE
#define GL_COMPILE 0x1300u
#endif

typedef struct SphereDisplayListCache {
    unsigned int displayListId;
    int stacks;
    int slices;
    bool initialized;
} SphereDisplayListCache;

static SphereDisplayListCache gSphereDisplayListCache = {0, 0, 0, false};
static bool gSphereDisplayListSupported = true;

static bool ensureGlContext(VM* vm, const char* name) {
    if (!gSdlInitialized || !gSdlWindow ||
        (gSdlGLContext == NULL && gSdlRenderer == NULL)) {
        runtimeError(vm, "%s requires an active 3D graphics window. Call InitGraph3D first.",
                     name);
        return false;
    }
    return true;
}

static void destroySphereDisplayList(void) {
    if (gSphereDisplayListCache.initialized && gSphereDisplayListCache.displayListId != 0) {
        gfx3dDeleteLists(gSphereDisplayListCache.displayListId, 1);
    }
    gSphereDisplayListCache.displayListId = 0;
    gSphereDisplayListCache.initialized = false;
}

static void drawUnitSphereImmediate(int stacks, int slices) {
    const double pi = 3.14159265358979323846;
    for (int stack = 0; stack < stacks; ++stack) {
        double phi0 = -pi * 0.5 + pi * stack / (double)stacks;
        double phi1 = -pi * 0.5 + pi * (stack + 1) / (double)stacks;
        double cosPhi0 = cos(phi0);
        double sinPhi0 = sin(phi0);
        double cosPhi1 = cos(phi1);
        double sinPhi1 = sin(phi1);

        gfx3dBegin(GL_TRIANGLE_STRIP);
        for (int slice = 0; slice <= slices; ++slice) {
            double theta = 2.0 * pi * slice / (double)slices;
            double cosTheta = cos(theta);
            double sinTheta = sin(theta);

            float n1x = (float)(cosPhi1 * cosTheta);
            float n1y = (float)sinPhi1;
            float n1z = (float)(cosPhi1 * sinTheta);
            gfx3dNormal3f(n1x, n1y, n1z);
            gfx3dVertex3f(n1x, n1y, n1z);

            float n0x = (float)(cosPhi0 * cosTheta);
            float n0y = (float)sinPhi0;
            float n0z = (float)(cosPhi0 * sinTheta);
            gfx3dNormal3f(n0x, n0y, n0z);
            gfx3dVertex3f(n0x, n0y, n0z);
        }
        gfx3dEnd();
    }
}

static bool ensureSphereDisplayList(int stacks, int slices) {
    if (!gSphereDisplayListSupported) {
        return false;
    }

    if (gSphereDisplayListCache.initialized && gSphereDisplayListCache.stacks == stacks &&
        gSphereDisplayListCache.slices == slices && gSphereDisplayListCache.displayListId != 0) {
        return true;
    }

    unsigned int newList = gfx3dGenLists(1);
    if (newList == 0) {
        gSphereDisplayListSupported = false;
        destroySphereDisplayList();
        return false;
    }

    gfx3dNewList(newList, GL_COMPILE);
    drawUnitSphereImmediate(stacks, slices);
    gfx3dEndList();

    destroySphereDisplayList();
    gSphereDisplayListCache.displayListId = newList;
    gSphereDisplayListCache.stacks = stacks;
    gSphereDisplayListCache.slices = slices;
    gSphereDisplayListCache.initialized = true;
    return true;
}

static Value vmBuiltinBouncingBalls3DDrawUnitSphereFast(VM* vm, int arg_count,
                                                        Value* args) {
    const char* name = "BouncingBalls3DDrawUnitSphereFast";
    if (arg_count != 2) {
        runtimeError(vm, "%s expects 2 arguments.", name);
        return makeVoid();
    }
    if (!IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1])) {
        runtimeError(vm, "%s expects integer stack and slice counts.", name);
        return makeVoid();
    }

    int stacks = (int)asI64(args[0]);
    int slices = (int)asI64(args[1]);

    if (stacks < 3 || slices < 3) {
        runtimeError(vm, "%s received invalid tessellation parameters.", name);
        return makeVoid();
    }

    if (!ensureGlContext(vm, name)) {
        return makeVoid();
    }

    if (ensureSphereDisplayList(stacks, slices) && gSphereDisplayListCache.displayListId != 0) {
        gfx3dCallList(gSphereDisplayListCache.displayListId);
        return makeVoid();
    }

    drawUnitSphereImmediate(stacks, slices);

    return makeVoid();
}
#else
static Value vmBuiltinBouncingBalls3DDrawUnitSphereFast(VM* vm, int arg_count,
                                                        Value* args) {
    (void)arg_count;
    (void)args;
    runtimeError(vm,
                 "BouncingBalls3DDrawUnitSphereFast requires SDL/OpenGL support to be built.");
    return makeVoid();
}
#endif

static void assignNumericVar(const NumericVarRef* ref, long double value) {
    if (!ref || !ref->slot) return;
    if (ref->isInteger) {
        long double truncated = truncl(value);
        if (truncated < 0.0L) truncated = 0.0L;
        SET_INT_VALUE(ref->slot, (long long)truncated);
    } else {
        SET_REAL_VALUE(ref->slot, value);
    }
}

static void writeDefaultLightingOutputs(const Balls3DParams* params, int index) {
    if (params->lightIntensity) assignFloatValue(&params->lightIntensity[index], 0.0);
    if (params->rimIntensity) assignFloatValue(&params->rimIntensity[index], 0.0);
    if (params->highlightX) assignFloatValue(&params->highlightX[index], 0.0);
    if (params->highlightY) assignFloatValue(&params->highlightY[index], 0.0);
    if (params->highlightRadius) assignFloatValue(&params->highlightRadius[index], 1.0);
    if (params->highlightStrength)
        assignFloatValue(&params->highlightStrength[index], 0.0);
}

static void projectHighlight(const Balls3DParams* params, double centerX, double centerY,
                             double centerZ, double radius, double screenRadius,
                             double viewScaleX, double viewScaleY, double rim,
                             const double viewDir[3], const double halfVec[3], int index) {
    if (!params->highlightX || !params->highlightY || !params->highlightRadius ||
        !params->highlightStrength)
        return;

    double highlightScale = 0.6;
    double hx = centerX + halfVec[0] * radius * highlightScale;
    double hy = centerY + halfVec[1] * radius * highlightScale;
    double hz = centerZ + halfVec[2] * radius * highlightScale;

    double denom = params->cameraDistance - hz;
    double highlightSX = params->screenWidth * 0.5 + (centerX * viewScaleX);
    double highlightSY = params->screenHeight * 0.5 - (centerY * viewScaleY);
    if (denom > 1e-6) {
        double perspective = params->cameraDistance / denom;
        highlightSX = params->screenWidth * 0.5 + hx * perspective * viewScaleX;
        highlightSY = params->screenHeight * 0.5 - hy * perspective * viewScaleY;
    }

    double rimClamped = clampd(1.0 - rim, 0.0, 1.0);
    double highlightRadius = screenRadius * clampd(0.2 + 0.3 * rimClamped, 0.18, 0.45);
    double specDot = clampd(viewDir[0] * halfVec[0] + viewDir[1] * halfVec[1] +
                                viewDir[2] * halfVec[2],
                            0.0, 1.0);
    double highlightStrength = pow(specDot, 12.0);

    assignFloatValue(&params->highlightX[index], highlightSX);
    assignFloatValue(&params->highlightY[index], highlightSY);
    assignFloatValue(&params->highlightRadius[index], highlightRadius);
    assignFloatValue(&params->highlightStrength[index], highlightStrength);
}

static void writeLightingOutputs(const Balls3DParams* params, int index, double diffuse,
                                 double rim, double viewScaleX, double viewScaleY,
                                 double x, double y, double z, double radius,
                                 double screenRadius, const double viewDir[3],
                                 const double halfVec[3]) {
    if (params->lightIntensity)
        assignFloatValue(&params->lightIntensity[index], diffuse);
    if (params->rimIntensity)
        assignFloatValue(&params->rimIntensity[index], clampd(rim, 0.0, 1.0));
    projectHighlight(params, x, y, z, radius, screenRadius, viewScaleX, viewScaleY,
                     clampd(rim, 0.0, 1.0), viewDir, halfVec, index);
}

static Value runBalls3DStep(VM* vm, const char* name, const Balls3DParams* params) {
    int ballCount = params->ballCount;
    double deltaTime = params->deltaTime;
    double halfWidth = params->boxWidth * 0.5;
    double halfHeight = params->boxHeight * 0.5;
    double nearPlane = 0.0;
    double backPlane = -params->boxDepth;
    double viewScaleX = params->screenWidth / params->boxWidth;
    double viewScaleY = params->screenHeight / params->boxHeight;
    bool computeLighting = params->hasLight &&
                           (params->lightIntensity || params->rimIntensity ||
                            params->highlightX || params->highlightY ||
                            params->highlightRadius || params->highlightStrength);
    double lightDirX = params->lightDir[0];
    double lightDirY = params->lightDir[1];
    double lightDirZ = params->lightDir[2];

    for (int i = 0; i < ballCount; ++i) {
        double x = (double)asLd(params->posX[i]);
        double y = (double)asLd(params->posY[i]);
        double z = (double)asLd(params->posZ[i]);
        double vx = (double)asLd(params->velX[i]);
        double vy = (double)asLd(params->velY[i]);
        double vz = (double)asLd(params->velZ[i]);
        double r = (double)asLd(params->radius[i]);

        if (r <= 0.0) r = 1.0;
        double minX = -halfWidth + r;
        double maxX = halfWidth - r;
        double minY = -halfHeight + r;
        double maxY = halfHeight - r;
        double minZ = backPlane + r;
        double maxZ = nearPlane - r;

        vx *= params->drag;
        vy *= params->drag;
        vz *= params->drag;

        x += vx * deltaTime;
        y += vy * deltaTime;
        z += vz * deltaTime;

        if (x < minX) {
            x = minX;
            vx = fabs(vx) * params->wallElasticity;
            if (vx < params->minSpeed) vx = params->minSpeed;
        } else if (x > maxX) {
            x = maxX;
            vx = -fabs(vx) * params->wallElasticity;
            if (-vx < params->minSpeed) vx = -params->minSpeed;
        }
        if (y < minY) {
            y = minY;
            vy = fabs(vy) * params->wallElasticity;
            if (vy < params->minSpeed) vy = params->minSpeed;
        } else if (y > maxY) {
            y = maxY;
            vy = -fabs(vy) * params->wallElasticity;
            if (-vy < params->minSpeed) vy = -params->minSpeed;
        }
        if (z < minZ) {
            z = minZ;
            vz = fabs(vz) * params->wallElasticity;
            if (vz < params->minSpeed) vz = params->minSpeed;
        } else if (z > maxZ) {
            z = maxZ;
            vz = -fabs(vz) * params->wallElasticity;
            if (-vz < params->minSpeed) vz = -params->minSpeed;
        }

        vx = enforceSpeed(vx, params->minSpeed, params->maxSpeed);
        vy = enforceSpeed(vy, params->minSpeed, params->maxSpeed);
        vz = enforceSpeed(vz, params->minSpeed, params->maxSpeed);

        assignFloatValue(&params->posX[i], x);
        assignFloatValue(&params->posY[i], y);
        assignFloatValue(&params->posZ[i], z);
        assignFloatValue(&params->velX[i], vx);
        assignFloatValue(&params->velY[i], vy);
        assignFloatValue(&params->velZ[i], vz);
    }

    for (int i = 0; i < ballCount; ++i) {
        double xi = (double)asLd(params->posX[i]);
        double yi = (double)asLd(params->posY[i]);
        double zi = (double)asLd(params->posZ[i]);
        double vxi = (double)asLd(params->velX[i]);
        double vyi = (double)asLd(params->velY[i]);
        double vzi = (double)asLd(params->velZ[i]);
        double ri = (double)asLd(params->radius[i]);
        double mi = ri * ri * ri;
        if (mi <= 0.0) mi = 1.0;

        for (int j = i + 1; j < ballCount; ++j) {
            double xj = (double)asLd(params->posX[j]);
            double yj = (double)asLd(params->posY[j]);
            double zj = (double)asLd(params->posZ[j]);
            double vxj = (double)asLd(params->velX[j]);
            double vyj = (double)asLd(params->velY[j]);
            double vzj = (double)asLd(params->velZ[j]);
            double rj = (double)asLd(params->radius[j]);
            double mj = rj * rj * rj;
            if (mj <= 0.0) mj = 1.0;

            double dx = xj - xi;
            double dy = yj - yi;
            double dz = zj - zi;
            double sumR = ri + rj;
            double distSq = dx * dx + dy * dy + dz * dz;
            if (distSq >= sumR * sumR) {
                continue;
            }

            double dist = sqrt(distSq);
            double nx, ny, nz;
            if (dist > 1e-6) {
                nx = dx / dist;
                ny = dy / dist;
                nz = dz / dist;
            } else {
                nx = 1.0;
                ny = 0.0;
                nz = 0.0;
                dist = sumR;
            }

            double viN = vxi * nx + vyi * ny + vzi * nz;
            double vjN = vxj * nx + vyj * ny + vzj * nz;

            double viT_x = vxi - viN * nx;
            double viT_y = vyi - viN * ny;
            double viT_z = vzi - viN * nz;

            double vjT_x = vxj - vjN * nx;
            double vjT_y = vyj - vjN * ny;
            double vjT_z = vzj - vjN * nz;

            double newViN = (viN * (mi - mj) + 2.0 * mj * vjN) / (mi + mj);
            double newVjN = (vjN * (mj - mi) + 2.0 * mi * viN) / (mi + mj);

            vxi = viT_x + newViN * nx;
            vyi = viT_y + newViN * ny;
            vzi = viT_z + newViN * nz;

            vxj = vjT_x + newVjN * nx;
            vyj = vjT_y + newVjN * ny;
            vzj = vjT_z + newVjN * nz;

            double overlap = sumR - dist;
            if (overlap > 0.0) {
                double correction = overlap * 0.5;
                xi -= correction * nx;
                yi -= correction * ny;
                zi -= correction * nz;
                xj += correction * nx;
                yj += correction * ny;
                zj += correction * nz;
            }

            vxi = enforceSpeed(vxi, params->minSpeed, params->maxSpeed);
            vyi = enforceSpeed(vyi, params->minSpeed, params->maxSpeed);
            vzi = enforceSpeed(vzi, params->minSpeed, params->maxSpeed);
            vxj = enforceSpeed(vxj, params->minSpeed, params->maxSpeed);
            vyj = enforceSpeed(vyj, params->minSpeed, params->maxSpeed);
            vzj = enforceSpeed(vzj, params->minSpeed, params->maxSpeed);

            assignFloatValue(&params->posX[i], xi);
            assignFloatValue(&params->posY[i], yi);
            assignFloatValue(&params->posZ[i], zi);
            assignFloatValue(&params->velX[i], vxi);
            assignFloatValue(&params->velY[i], vyi);
            assignFloatValue(&params->velZ[i], vzi);

            assignFloatValue(&params->posX[j], xj);
            assignFloatValue(&params->posY[j], yj);
            assignFloatValue(&params->posZ[j], zj);
            assignFloatValue(&params->velX[j], vxj);
            assignFloatValue(&params->velY[j], vyj);
            assignFloatValue(&params->velZ[j], vzj);
        }
    }

    for (int i = 0; i < ballCount; ++i) {
        double x = (double)asLd(params->posX[i]);
        double y = (double)asLd(params->posY[i]);
        double z = (double)asLd(params->posZ[i]);
        double r = (double)asLd(params->radius[i]);

        if (z > nearPlane - r) {
            z = nearPlane - r;
            assignFloatValue(&params->posZ[i], z);
        }
        if (z < backPlane + r) {
            z = backPlane + r;
            assignFloatValue(&params->posZ[i], z);
        }

        double denom = params->cameraDistance - z;
        if (denom <= 1e-6) {
            assignFloatValue(&params->depthShade[i], -1.0);
            writeDefaultLightingOutputs(params, i);
            continue;
        }

        double perspective = params->cameraDistance / denom;
        double sx = params->screenWidth * 0.5 + x * perspective * viewScaleX;
        double sy = params->screenHeight * 0.5 - y * perspective * viewScaleY;
        double sr = r * perspective * (viewScaleX + viewScaleY) * 0.5;
        if (sr < 1.0) sr = 1.0;

        double depthFactor = clampd(-z / params->boxDepth, 0.0, 1.0);
        double shade = 0.25 + 0.75 * depthFactor;

        assignFloatValue(&params->screenX[i], sx);
        assignFloatValue(&params->screenY[i], sy);
        assignFloatValue(&params->screenRadius[i], sr);
        assignFloatValue(&params->depthShade[i], shade);

        if (computeLighting) {
            double viewVecX = -x;
            double viewVecY = -y;
            double viewVecZ = params->cameraDistance - z;
            double viewLenSq = viewVecX * viewVecX + viewVecY * viewVecY +
                               viewVecZ * viewVecZ;
            double viewDir[3];
            if (viewLenSq < 1e-9) {
                viewDir[0] = 0.0;
                viewDir[1] = 0.0;
                viewDir[2] = 1.0;
            } else {
                double invLen = 1.0 / sqrt(viewLenSq);
                viewDir[0] = viewVecX * invLen;
                viewDir[1] = viewVecY * invLen;
                viewDir[2] = viewVecZ * invLen;
            }

            double diffuse = clampd(viewDir[0] * lightDirX + viewDir[1] * lightDirY +
                                        viewDir[2] * lightDirZ,
                                    0.0, 1.0);
            double rim = clampd(1.0 - viewDir[2], 0.0, 1.0);

            double halfVec[3] = {viewDir[0] + lightDirX, viewDir[1] + lightDirY,
                                 viewDir[2] + lightDirZ};
            double halfLenSq = halfVec[0] * halfVec[0] + halfVec[1] * halfVec[1] +
                               halfVec[2] * halfVec[2];
            if (halfLenSq < 1e-9) {
                halfVec[0] = viewDir[0];
                halfVec[1] = viewDir[1];
                halfVec[2] = viewDir[2];
            } else {
                double invLen = 1.0 / sqrt(halfLenSq);
                halfVec[0] *= invLen;
                halfVec[1] *= invLen;
                halfVec[2] *= invLen;
            }

            writeLightingOutputs(params, i, diffuse, rim, viewScaleX, viewScaleY, x,
                                 y, z, r, sr, viewDir, halfVec);
        } else {
            writeDefaultLightingOutputs(params, i);
        }
    }

    return makeVoid();
}

static Value runBalls3DStepOptimized(VM* vm, const char* name,
                                     const Balls3DParams* params) {
    int ballCount = params->ballCount;
    if (ballCount <= 0) return makeVoid();

    if (!ensureBalls3DWorkCapacity(ballCount)) {
        runtimeError(vm, "%s failed to allocate work buffers for %d balls.", name,
                     ballCount);
        return makeVoid();
    }

    double* posX = balls3dWorkBuffers.posX;
    double* posY = balls3dWorkBuffers.posY;
    double* posZ = balls3dWorkBuffers.posZ;
    double* velX = balls3dWorkBuffers.velX;
    double* velY = balls3dWorkBuffers.velY;
    double* velZ = balls3dWorkBuffers.velZ;
    double* radius = balls3dWorkBuffers.radius;

    for (int i = 0; i < ballCount; ++i) {
        posX[i] = (double)asLd(params->posX[i]);
        posY[i] = (double)asLd(params->posY[i]);
        posZ[i] = (double)asLd(params->posZ[i]);
        velX[i] = (double)asLd(params->velX[i]);
        velY[i] = (double)asLd(params->velY[i]);
        velZ[i] = (double)asLd(params->velZ[i]);
        radius[i] = (double)asLd(params->radius[i]);
        if (radius[i] <= 0.0) radius[i] = 1.0;
    }

    double deltaTime = params->deltaTime;
    double halfWidth = params->boxWidth * 0.5;
    double halfHeight = params->boxHeight * 0.5;
    double nearPlane = 0.0;
    double backPlane = -params->boxDepth;
    double viewScaleX = params->screenWidth / params->boxWidth;
    double viewScaleY = params->screenHeight / params->boxHeight;
    double minSpeed = params->minSpeed;
    double maxSpeed = params->maxSpeed;
    double drag = params->drag;
    double wallElasticity = params->wallElasticity;

    for (int i = 0; i < ballCount; ++i) {
        double r = radius[i];
        double minX = -halfWidth + r;
        double maxX = halfWidth - r;
        double minY = -halfHeight + r;
        double maxY = halfHeight - r;
        double minZ = backPlane + r;
        double maxZ = nearPlane - r;

        double x = posX[i];
        double y = posY[i];
        double z = posZ[i];
        double vx = velX[i] * drag;
        double vy = velY[i] * drag;
        double vz = velZ[i] * drag;

        x += vx * deltaTime;
        y += vy * deltaTime;
        z += vz * deltaTime;

        if (x < minX) {
            x = minX;
            vx = fabs(vx) * wallElasticity;
            if (vx < minSpeed) vx = minSpeed;
        } else if (x > maxX) {
            x = maxX;
            vx = -fabs(vx) * wallElasticity;
            if (-vx < minSpeed) vx = -minSpeed;
        }

        if (y < minY) {
            y = minY;
            vy = fabs(vy) * wallElasticity;
            if (vy < minSpeed) vy = minSpeed;
        } else if (y > maxY) {
            y = maxY;
            vy = -fabs(vy) * wallElasticity;
            if (-vy < minSpeed) vy = -minSpeed;
        }

        if (z < minZ) {
            z = minZ;
            vz = fabs(vz) * wallElasticity;
            if (vz < minSpeed) vz = minSpeed;
        } else if (z > maxZ) {
            z = maxZ;
            vz = -fabs(vz) * wallElasticity;
            if (-vz < minSpeed) vz = -minSpeed;
        }

        velX[i] = enforceSpeed(vx, minSpeed, maxSpeed);
        velY[i] = enforceSpeed(vy, minSpeed, maxSpeed);
        velZ[i] = enforceSpeed(vz, minSpeed, maxSpeed);
        posX[i] = x;
        posY[i] = y;
        posZ[i] = z;
    }

    for (int i = 0; i < ballCount; ++i) {
        double xi = posX[i];
        double yi = posY[i];
        double zi = posZ[i];
        double vxi = velX[i];
        double vyi = velY[i];
        double vzi = velZ[i];
        double ri = radius[i];
        double mi = ri * ri * ri;
        if (mi <= 0.0) mi = 1.0;

        for (int j = i + 1; j < ballCount; ++j) {
            double xj = posX[j];
            double yj = posY[j];
            double zj = posZ[j];
            double vxj = velX[j];
            double vyj = velY[j];
            double vzj = velZ[j];
            double rj = radius[j];
            double mj = rj * rj * rj;
            if (mj <= 0.0) mj = 1.0;

            double dx = xj - xi;
            double dy = yj - yi;
            double dz = zj - zi;
            double sumR = ri + rj;
            double distSq = dx * dx + dy * dy + dz * dz;
            if (distSq >= sumR * sumR) {
                continue;
            }

            double dist = sqrt(distSq);
            double nx, ny, nz;
            if (dist > 1e-6) {
                nx = dx / dist;
                ny = dy / dist;
                nz = dz / dist;
            } else {
                nx = 1.0;
                ny = 0.0;
                nz = 0.0;
                dist = sumR;
            }

            double viN = vxi * nx + vyi * ny + vzi * nz;
            double vjN = vxj * nx + vyj * ny + vzj * nz;

            double viT_x = vxi - viN * nx;
            double viT_y = vyi - viN * ny;
            double viT_z = vzi - viN * nz;

            double vjT_x = vxj - vjN * nx;
            double vjT_y = vyj - vjN * ny;
            double vjT_z = vzj - vjN * nz;

            double newViN = (viN * (mi - mj) + 2.0 * mj * vjN) / (mi + mj);
            double newVjN = (vjN * (mj - mi) + 2.0 * mi * viN) / (mi + mj);

            vxi = viT_x + newViN * nx;
            vyi = viT_y + newViN * ny;
            vzi = viT_z + newViN * nz;

            vxj = vjT_x + newVjN * nx;
            vyj = vjT_y + newVjN * ny;
            vzj = vjT_z + newVjN * nz;

            double overlap = sumR - dist;
            if (overlap > 0.0) {
                double correction = overlap * 0.5;
                xi -= correction * nx;
                yi -= correction * ny;
                zi -= correction * nz;
                xj += correction * nx;
                yj += correction * ny;
                zj += correction * nz;
            }

            vxi = enforceSpeed(vxi, minSpeed, maxSpeed);
            vyi = enforceSpeed(vyi, minSpeed, maxSpeed);
            vzi = enforceSpeed(vzi, minSpeed, maxSpeed);
            vxj = enforceSpeed(vxj, minSpeed, maxSpeed);
            vyj = enforceSpeed(vyj, minSpeed, maxSpeed);
            vzj = enforceSpeed(vzj, minSpeed, maxSpeed);

            posX[j] = xj;
            posY[j] = yj;
            posZ[j] = zj;
            velX[j] = vxj;
            velY[j] = vyj;
            velZ[j] = vzj;
        }

        posX[i] = xi;
        posY[i] = yi;
        posZ[i] = zi;
        velX[i] = vxi;
        velY[i] = vyi;
        velZ[i] = vzi;
    }

    bool computeLighting = params->hasLight &&
                           (params->lightIntensity || params->rimIntensity ||
                            params->highlightX || params->highlightY ||
                            params->highlightRadius || params->highlightStrength);
    double lightDirX = params->lightDir[0];
    double lightDirY = params->lightDir[1];
    double lightDirZ = params->lightDir[2];

    for (int i = 0; i < ballCount; ++i) {
        double x = posX[i];
        double y = posY[i];
        double z = posZ[i];
        double r = radius[i];

        if (z > nearPlane - r) {
            z = nearPlane - r;
            posZ[i] = z;
        }
        if (z < backPlane + r) {
            z = backPlane + r;
            posZ[i] = z;
        }

        assignFloatValue(&params->posX[i], x);
        assignFloatValue(&params->posY[i], y);
        assignFloatValue(&params->posZ[i], z);
        assignFloatValue(&params->velX[i], velX[i]);
        assignFloatValue(&params->velY[i], velY[i]);
        assignFloatValue(&params->velZ[i], velZ[i]);

        double denom = params->cameraDistance - z;
        if (denom <= 1e-6) {
            assignFloatValue(&params->depthShade[i], -1.0);
            writeDefaultLightingOutputs(params, i);
            continue;
        }

        double perspective = params->cameraDistance / denom;
        double sx = params->screenWidth * 0.5 + x * perspective * viewScaleX;
        double sy = params->screenHeight * 0.5 - y * perspective * viewScaleY;
        double sr = r * perspective * (viewScaleX + viewScaleY) * 0.5;
        if (sr < 1.0) sr = 1.0;

        double depthFactor = clampd(-z / params->boxDepth, 0.0, 1.0);
        double shade = 0.25 + 0.75 * depthFactor;

        assignFloatValue(&params->screenX[i], sx);
        assignFloatValue(&params->screenY[i], sy);
        assignFloatValue(&params->screenRadius[i], sr);
        assignFloatValue(&params->depthShade[i], shade);

        if (computeLighting) {
            double viewVecX = -x;
            double viewVecY = -y;
            double viewVecZ = params->cameraDistance - z;
            double viewLenSq = viewVecX * viewVecX + viewVecY * viewVecY +
                               viewVecZ * viewVecZ;
            double viewDir[3];
            if (viewLenSq < 1e-9) {
                viewDir[0] = 0.0;
                viewDir[1] = 0.0;
                viewDir[2] = 1.0;
            } else {
                double invLen = 1.0 / sqrt(viewLenSq);
                viewDir[0] = viewVecX * invLen;
                viewDir[1] = viewVecY * invLen;
                viewDir[2] = viewVecZ * invLen;
            }

            double diffuse = clampd(viewDir[0] * lightDirX + viewDir[1] * lightDirY +
                                        viewDir[2] * lightDirZ,
                                    0.0, 1.0);
            double rim = clampd(1.0 - viewDir[2], 0.0, 1.0);

            double halfVec[3] = {viewDir[0] + lightDirX, viewDir[1] + lightDirY,
                                 viewDir[2] + lightDirZ};
            double halfLenSq = halfVec[0] * halfVec[0] + halfVec[1] * halfVec[1] +
                               halfVec[2] * halfVec[2];
            if (halfLenSq < 1e-9) {
                halfVec[0] = viewDir[0];
                halfVec[1] = viewDir[1];
                halfVec[2] = viewDir[2];
            } else {
                double invLen = 1.0 / sqrt(halfLenSq);
                halfVec[0] *= invLen;
                halfVec[1] *= invLen;
                halfVec[2] *= invLen;
            }

            writeLightingOutputs(params, i, diffuse, rim, viewScaleX, viewScaleY, x,
                                 y, z, r, sr, viewDir, halfVec);
        } else {
            writeDefaultLightingOutputs(params, i);
        }
    }

    return makeVoid();
}

static Value vmBuiltinBouncingBalls3DStep(VM* vm, int arg_count, Value* args) {
    const char* name = "BouncingBalls3DStep";
    if (arg_count != 23) {
        runtimeError(vm, "%s expects 23 arguments.", name);
        return makeVoid();
    }

    if (!IS_INTLIKE(args[0])) {
        runtimeError(vm, "%s expects integer ball count.", name);
        return makeVoid();
    }
    int ballCount = (int)asI64(args[0]);
    if (ballCount <= 0) {
        runtimeError(vm, "%s requires positive ball count.", name);
        return makeVoid();
    }

    for (int i = 1; i <= 11; ++i) {
        if (!IS_NUMERIC(args[i])) {
            runtimeError(vm, "%s expects numeric arguments in positions 2-12.",
                         name);
            return makeVoid();
        }
    }

    Balls3DParams params;
    params.ballCount = ballCount;
    params.deltaTime = (double)asLd(args[1]);
    params.boxWidth = (double)asLd(args[2]);
    params.boxHeight = (double)asLd(args[3]);
    params.boxDepth = (double)asLd(args[4]);
    params.wallElasticity = (double)asLd(args[5]);
    params.minSpeed = fabs((double)asLd(args[6]));
    params.maxSpeed = fabs((double)asLd(args[7]));
    params.drag = (double)asLd(args[8]);
    params.cameraDistance = (double)asLd(args[9]);
    params.screenWidth = (double)asLd(args[10]);
    params.screenHeight = (double)asLd(args[11]);
    params.lightDir[0] = 0.0;
    params.lightDir[1] = 0.0;
    params.lightDir[2] = 1.0;
    params.hasLight = false;

    if (params.deltaTime <= 0.0) {
        runtimeError(vm, "%s requires positive delta time.", name);
        return makeVoid();
    }
    if (params.boxWidth <= 0.0 || params.boxHeight <= 0.0 || params.boxDepth <= 0.0) {
        runtimeError(vm, "%s requires positive box dimensions.", name);
        return makeVoid();
    }
    if (params.wallElasticity < 0.0) {
        runtimeError(vm, "%s requires non-negative wall elasticity.", name);
        return makeVoid();
    }
    if (params.maxSpeed < 1e-6) {
        runtimeError(vm, "%s requires a positive maximum speed.", name);
        return makeVoid();
    }
    if (params.minSpeed > params.maxSpeed) {
        runtimeError(vm, "%s minimum speed exceeds maximum speed.", name);
        return makeVoid();
    }
    if (params.drag <= 0.0 || params.drag > 1.0) {
        runtimeError(vm, "%s expects drag between 0 and 1.", name);
        return makeVoid();
    }
    if (params.cameraDistance <= 0.0) {
        runtimeError(vm, "%s requires positive camera distance.", name);
        return makeVoid();
    }
    if (params.screenWidth <= 0.0 || params.screenHeight <= 0.0) {
        runtimeError(vm, "%s requires positive screen dimensions.", name);
        return makeVoid();
    }

    int arrayUpper = INT_MAX;
    params.posX = fetchZeroBasedArray(vm, &args[12], name, &arrayUpper);
    if (!params.posX) return makeVoid();
    params.posY = fetchZeroBasedArray(vm, &args[13], name, &arrayUpper);
    if (!params.posY) return makeVoid();
    params.posZ = fetchZeroBasedArray(vm, &args[14], name, &arrayUpper);
    if (!params.posZ) return makeVoid();
    params.velX = fetchZeroBasedArray(vm, &args[15], name, &arrayUpper);
    if (!params.velX) return makeVoid();
    params.velY = fetchZeroBasedArray(vm, &args[16], name, &arrayUpper);
    if (!params.velY) return makeVoid();
    params.velZ = fetchZeroBasedArray(vm, &args[17], name, &arrayUpper);
    if (!params.velZ) return makeVoid();
    params.radius = fetchZeroBasedArray(vm, &args[18], name, &arrayUpper);
    if (!params.radius) return makeVoid();
    params.screenX = fetchZeroBasedArray(vm, &args[19], name, &arrayUpper);
    if (!params.screenX) return makeVoid();
    params.screenY = fetchZeroBasedArray(vm, &args[20], name, &arrayUpper);
    if (!params.screenY) return makeVoid();
    params.screenRadius = fetchZeroBasedArray(vm, &args[21], name, &arrayUpper);
    if (!params.screenRadius) return makeVoid();
    params.depthShade = fetchZeroBasedArray(vm, &args[22], name, &arrayUpper);
    if (!params.depthShade) return makeVoid();

    params.lightIntensity = NULL;
    params.rimIntensity = NULL;
    params.highlightX = NULL;
    params.highlightY = NULL;
    params.highlightRadius = NULL;
    params.highlightStrength = NULL;

    if (arrayUpper < ballCount - 1) {
        runtimeError(vm, "%s arrays are smaller than the requested ball count.",
                     name);
        return makeVoid();
    }

    return runBalls3DStep(vm, name, &params);
}

static Value vmBuiltinBouncingBalls3DStepUltra(VM* vm, int arg_count,
                                               Value* args) {
    const char* name = "BouncingBalls3DStepUltra";
    if (arg_count != 23) {
        runtimeError(vm, "%s expects 23 arguments.", name);
        return makeVoid();
    }

    if (!IS_INTLIKE(args[0])) {
        runtimeError(vm, "%s expects integer ball count.", name);
        return makeVoid();
    }
    int ballCount = (int)asI64(args[0]);
    if (ballCount <= 0) {
        runtimeError(vm, "%s requires positive ball count.", name);
        return makeVoid();
    }

    for (int i = 1; i <= 11; ++i) {
        if (!IS_NUMERIC(args[i])) {
            runtimeError(vm, "%s expects numeric arguments in positions 2-12.",
                         name);
            return makeVoid();
        }
    }

    Balls3DParams params;
    params.ballCount = ballCount;
    params.deltaTime = (double)asLd(args[1]);
    params.boxWidth = (double)asLd(args[2]);
    params.boxHeight = (double)asLd(args[3]);
    params.boxDepth = (double)asLd(args[4]);
    params.wallElasticity = (double)asLd(args[5]);
    params.minSpeed = fabs((double)asLd(args[6]));
    params.maxSpeed = fabs((double)asLd(args[7]));
    params.drag = (double)asLd(args[8]);
    params.cameraDistance = (double)asLd(args[9]);
    params.screenWidth = (double)asLd(args[10]);
    params.screenHeight = (double)asLd(args[11]);
    params.lightDir[0] = 0.0;
    params.lightDir[1] = 0.0;
    params.lightDir[2] = 1.0;
    params.hasLight = false;

    if (params.deltaTime <= 0.0) {
        runtimeError(vm, "%s requires positive delta time.", name);
        return makeVoid();
    }
    if (params.boxWidth <= 0.0 || params.boxHeight <= 0.0 ||
        params.boxDepth <= 0.0) {
        runtimeError(vm, "%s requires positive box dimensions.", name);
        return makeVoid();
    }
    if (params.wallElasticity < 0.0) {
        runtimeError(vm, "%s requires non-negative wall elasticity.", name);
        return makeVoid();
    }
    if (params.maxSpeed < 1e-6) {
        runtimeError(vm, "%s requires a positive maximum speed.", name);
        return makeVoid();
    }
    if (params.minSpeed > params.maxSpeed) {
        runtimeError(vm, "%s minimum speed exceeds maximum speed.", name);
        return makeVoid();
    }
    if (params.drag <= 0.0 || params.drag > 1.0) {
        runtimeError(vm, "%s expects drag between 0 and 1.", name);
        return makeVoid();
    }
    if (params.cameraDistance <= 0.0) {
        runtimeError(vm, "%s requires positive camera distance.", name);
        return makeVoid();
    }
    if (params.screenWidth <= 0.0 || params.screenHeight <= 0.0) {
        runtimeError(vm, "%s requires positive screen dimensions.", name);
        return makeVoid();
    }

    int arrayUpper = INT_MAX;
    params.posX = fetchZeroBasedArray(vm, &args[12], name, &arrayUpper);
    if (!params.posX) return makeVoid();
    params.posY = fetchZeroBasedArray(vm, &args[13], name, &arrayUpper);
    if (!params.posY) return makeVoid();
    params.posZ = fetchZeroBasedArray(vm, &args[14], name, &arrayUpper);
    if (!params.posZ) return makeVoid();
    params.velX = fetchZeroBasedArray(vm, &args[15], name, &arrayUpper);
    if (!params.velX) return makeVoid();
    params.velY = fetchZeroBasedArray(vm, &args[16], name, &arrayUpper);
    if (!params.velY) return makeVoid();
    params.velZ = fetchZeroBasedArray(vm, &args[17], name, &arrayUpper);
    if (!params.velZ) return makeVoid();
    params.radius = fetchZeroBasedArray(vm, &args[18], name, &arrayUpper);
    if (!params.radius) return makeVoid();
    params.screenX = fetchZeroBasedArray(vm, &args[19], name, &arrayUpper);
    if (!params.screenX) return makeVoid();
    params.screenY = fetchZeroBasedArray(vm, &args[20], name, &arrayUpper);
    if (!params.screenY) return makeVoid();
    params.screenRadius = fetchZeroBasedArray(vm, &args[21], name, &arrayUpper);
    if (!params.screenRadius) return makeVoid();
    params.depthShade = fetchZeroBasedArray(vm, &args[22], name, &arrayUpper);
    if (!params.depthShade) return makeVoid();

    params.lightIntensity = NULL;
    params.rimIntensity = NULL;
    params.highlightX = NULL;
    params.highlightY = NULL;
    params.highlightRadius = NULL;
    params.highlightStrength = NULL;

    if (arrayUpper < ballCount - 1) {
        runtimeError(vm, "%s arrays are smaller than the requested ball count.",
                     name);
        return makeVoid();
    }

    return runBalls3DStepOptimized(vm, name, &params);
}

static Value vmBuiltinBouncingBalls3DStepAdvanced(VM* vm, int arg_count,
                                                  Value* args) {
    const char* name = "BouncingBalls3DStepAdvanced";
    if (arg_count != 32) {
        runtimeError(vm, "%s expects 32 arguments.", name);
        return makeVoid();
    }

    if (!IS_INTLIKE(args[0])) {
        runtimeError(vm, "%s expects integer ball count.", name);
        return makeVoid();
    }
    int ballCount = (int)asI64(args[0]);
    if (ballCount <= 0) {
        runtimeError(vm, "%s requires positive ball count.", name);
        return makeVoid();
    }

    for (int i = 1; i <= 14; ++i) {
        if (!IS_NUMERIC(args[i])) {
            runtimeError(vm, "%s expects numeric arguments in positions 2-15.",
                         name);
            return makeVoid();
        }
    }

    Balls3DParams params;
    params.ballCount = ballCount;
    params.deltaTime = (double)asLd(args[1]);
    params.boxWidth = (double)asLd(args[2]);
    params.boxHeight = (double)asLd(args[3]);
    params.boxDepth = (double)asLd(args[4]);
    params.wallElasticity = (double)asLd(args[5]);
    params.minSpeed = fabs((double)asLd(args[6]));
    params.maxSpeed = fabs((double)asLd(args[7]));
    params.drag = (double)asLd(args[8]);
    params.cameraDistance = (double)asLd(args[9]);
    params.screenWidth = (double)asLd(args[10]);
    params.screenHeight = (double)asLd(args[11]);
    double lx = (double)asLd(args[12]);
    double ly = (double)asLd(args[13]);
    double lz = (double)asLd(args[14]);

    if (params.deltaTime <= 0.0) {
        runtimeError(vm, "%s requires positive delta time.", name);
        return makeVoid();
    }
    if (params.boxWidth <= 0.0 || params.boxHeight <= 0.0 || params.boxDepth <= 0.0) {
        runtimeError(vm, "%s requires positive box dimensions.", name);
        return makeVoid();
    }
    if (params.wallElasticity < 0.0) {
        runtimeError(vm, "%s requires non-negative wall elasticity.", name);
        return makeVoid();
    }
    if (params.maxSpeed < 1e-6) {
        runtimeError(vm, "%s requires a positive maximum speed.", name);
        return makeVoid();
    }
    if (params.minSpeed > params.maxSpeed) {
        runtimeError(vm, "%s minimum speed exceeds maximum speed.", name);
        return makeVoid();
    }
    if (params.drag <= 0.0 || params.drag > 1.0) {
        runtimeError(vm, "%s expects drag between 0 and 1.", name);
        return makeVoid();
    }
    if (params.cameraDistance <= 0.0) {
        runtimeError(vm, "%s requires positive camera distance.", name);
        return makeVoid();
    }
    if (params.screenWidth <= 0.0 || params.screenHeight <= 0.0) {
        runtimeError(vm, "%s requires positive screen dimensions.", name);
        return makeVoid();
    }

    double lightLenSq = lx * lx + ly * ly + lz * lz;
    if (lightLenSq < 1e-9) {
        runtimeError(vm, "%s requires a non-zero light direction.", name);
        return makeVoid();
    }
    double invLightLen = 1.0 / sqrt(lightLenSq);
    params.lightDir[0] = lx * invLightLen;
    params.lightDir[1] = ly * invLightLen;
    params.lightDir[2] = lz * invLightLen;
    params.hasLight = true;

    int arrayUpper = INT_MAX;
    params.posX = fetchZeroBasedArray(vm, &args[15], name, &arrayUpper);
    if (!params.posX) return makeVoid();
    params.posY = fetchZeroBasedArray(vm, &args[16], name, &arrayUpper);
    if (!params.posY) return makeVoid();
    params.posZ = fetchZeroBasedArray(vm, &args[17], name, &arrayUpper);
    if (!params.posZ) return makeVoid();
    params.velX = fetchZeroBasedArray(vm, &args[18], name, &arrayUpper);
    if (!params.velX) return makeVoid();
    params.velY = fetchZeroBasedArray(vm, &args[19], name, &arrayUpper);
    if (!params.velY) return makeVoid();
    params.velZ = fetchZeroBasedArray(vm, &args[20], name, &arrayUpper);
    if (!params.velZ) return makeVoid();
    params.radius = fetchZeroBasedArray(vm, &args[21], name, &arrayUpper);
    if (!params.radius) return makeVoid();
    params.screenX = fetchZeroBasedArray(vm, &args[22], name, &arrayUpper);
    if (!params.screenX) return makeVoid();
    params.screenY = fetchZeroBasedArray(vm, &args[23], name, &arrayUpper);
    if (!params.screenY) return makeVoid();
    params.screenRadius = fetchZeroBasedArray(vm, &args[24], name, &arrayUpper);
    if (!params.screenRadius) return makeVoid();
    params.depthShade = fetchZeroBasedArray(vm, &args[25], name, &arrayUpper);
    if (!params.depthShade) return makeVoid();
    params.lightIntensity = fetchZeroBasedArray(vm, &args[26], name, &arrayUpper);
    if (!params.lightIntensity) return makeVoid();
    params.rimIntensity = fetchZeroBasedArray(vm, &args[27], name, &arrayUpper);
    if (!params.rimIntensity) return makeVoid();
    params.highlightX = fetchZeroBasedArray(vm, &args[28], name, &arrayUpper);
    if (!params.highlightX) return makeVoid();
    params.highlightY = fetchZeroBasedArray(vm, &args[29], name, &arrayUpper);
    if (!params.highlightY) return makeVoid();
    params.highlightRadius = fetchZeroBasedArray(vm, &args[30], name, &arrayUpper);
    if (!params.highlightRadius) return makeVoid();
    params.highlightStrength = fetchZeroBasedArray(vm, &args[31], name, &arrayUpper);
    if (!params.highlightStrength) return makeVoid();

    if (arrayUpper < ballCount - 1) {
        runtimeError(vm, "%s arrays are smaller than the requested ball count.",
                     name);
        return makeVoid();
    }

    return runBalls3DStep(vm, name, &params);
}

static Value vmBuiltinBouncingBalls3DStepUltraAdvanced(VM* vm, int arg_count,
                                                       Value* args) {
    const char* name = "BouncingBalls3DStepUltraAdvanced";
    if (arg_count != 32) {
        runtimeError(vm, "%s expects 32 arguments.", name);
        return makeVoid();
    }

    if (!IS_INTLIKE(args[0])) {
        runtimeError(vm, "%s expects integer ball count.", name);
        return makeVoid();
    }
    int ballCount = (int)asI64(args[0]);
    if (ballCount <= 0) {
        runtimeError(vm, "%s requires positive ball count.", name);
        return makeVoid();
    }

    for (int i = 1; i <= 14; ++i) {
        if (!IS_NUMERIC(args[i])) {
            runtimeError(vm, "%s expects numeric arguments in positions 2-15.",
                         name);
            return makeVoid();
        }
    }

    Balls3DParams params;
    params.ballCount = ballCount;
    params.deltaTime = (double)asLd(args[1]);
    params.boxWidth = (double)asLd(args[2]);
    params.boxHeight = (double)asLd(args[3]);
    params.boxDepth = (double)asLd(args[4]);
    params.wallElasticity = (double)asLd(args[5]);
    params.minSpeed = fabs((double)asLd(args[6]));
    params.maxSpeed = fabs((double)asLd(args[7]));
    params.drag = (double)asLd(args[8]);
    params.cameraDistance = (double)asLd(args[9]);
    params.screenWidth = (double)asLd(args[10]);
    params.screenHeight = (double)asLd(args[11]);
    double lx = (double)asLd(args[12]);
    double ly = (double)asLd(args[13]);
    double lz = (double)asLd(args[14]);

    if (params.deltaTime <= 0.0) {
        runtimeError(vm, "%s requires positive delta time.", name);
        return makeVoid();
    }
    if (params.boxWidth <= 0.0 || params.boxHeight <= 0.0 ||
        params.boxDepth <= 0.0) {
        runtimeError(vm, "%s requires positive box dimensions.", name);
        return makeVoid();
    }
    if (params.wallElasticity < 0.0) {
        runtimeError(vm, "%s requires non-negative wall elasticity.", name);
        return makeVoid();
    }
    if (params.maxSpeed < 1e-6) {
        runtimeError(vm, "%s requires a positive maximum speed.", name);
        return makeVoid();
    }
    if (params.minSpeed > params.maxSpeed) {
        runtimeError(vm, "%s minimum speed exceeds maximum speed.", name);
        return makeVoid();
    }
    if (params.drag <= 0.0 || params.drag > 1.0) {
        runtimeError(vm, "%s expects drag between 0 and 1.", name);
        return makeVoid();
    }
    if (params.cameraDistance <= 0.0) {
        runtimeError(vm, "%s requires positive camera distance.", name);
        return makeVoid();
    }
    if (params.screenWidth <= 0.0 || params.screenHeight <= 0.0) {
        runtimeError(vm, "%s requires positive screen dimensions.", name);
        return makeVoid();
    }

    double lightLenSq = lx * lx + ly * ly + lz * lz;
    if (lightLenSq < 1e-9) {
        runtimeError(vm, "%s requires a non-zero light direction.", name);
        return makeVoid();
    }
    double invLightLen = 1.0 / sqrt(lightLenSq);
    params.lightDir[0] = lx * invLightLen;
    params.lightDir[1] = ly * invLightLen;
    params.lightDir[2] = lz * invLightLen;
    params.hasLight = true;

    int arrayUpper = INT_MAX;
    params.posX = fetchZeroBasedArray(vm, &args[15], name, &arrayUpper);
    if (!params.posX) return makeVoid();
    params.posY = fetchZeroBasedArray(vm, &args[16], name, &arrayUpper);
    if (!params.posY) return makeVoid();
    params.posZ = fetchZeroBasedArray(vm, &args[17], name, &arrayUpper);
    if (!params.posZ) return makeVoid();
    params.velX = fetchZeroBasedArray(vm, &args[18], name, &arrayUpper);
    if (!params.velX) return makeVoid();
    params.velY = fetchZeroBasedArray(vm, &args[19], name, &arrayUpper);
    if (!params.velY) return makeVoid();
    params.velZ = fetchZeroBasedArray(vm, &args[20], name, &arrayUpper);
    if (!params.velZ) return makeVoid();
    params.radius = fetchZeroBasedArray(vm, &args[21], name, &arrayUpper);
    if (!params.radius) return makeVoid();
    params.screenX = fetchZeroBasedArray(vm, &args[22], name, &arrayUpper);
    if (!params.screenX) return makeVoid();
    params.screenY = fetchZeroBasedArray(vm, &args[23], name, &arrayUpper);
    if (!params.screenY) return makeVoid();
    params.screenRadius = fetchZeroBasedArray(vm, &args[24], name, &arrayUpper);
    if (!params.screenRadius) return makeVoid();
    params.depthShade = fetchZeroBasedArray(vm, &args[25], name, &arrayUpper);
    if (!params.depthShade) return makeVoid();
    params.lightIntensity = fetchZeroBasedArray(vm, &args[26], name, &arrayUpper);
    if (!params.lightIntensity) return makeVoid();
    params.rimIntensity = fetchZeroBasedArray(vm, &args[27], name, &arrayUpper);
    if (!params.rimIntensity) return makeVoid();
    params.highlightX = fetchZeroBasedArray(vm, &args[28], name, &arrayUpper);
    if (!params.highlightX) return makeVoid();
    params.highlightY = fetchZeroBasedArray(vm, &args[29], name, &arrayUpper);
    if (!params.highlightY) return makeVoid();
    params.highlightRadius = fetchZeroBasedArray(vm, &args[30], name, &arrayUpper);
    if (!params.highlightRadius) return makeVoid();
    params.highlightStrength = fetchZeroBasedArray(vm, &args[31], name, &arrayUpper);
    if (!params.highlightStrength) return makeVoid();

    if (arrayUpper < ballCount - 1) {
        runtimeError(vm, "%s arrays are smaller than the requested ball count.",
                     name);
        return makeVoid();
    }

    return runBalls3DStepOptimized(vm, name, &params);
}

static Value vmBuiltinBouncingBalls3DAccelerate(VM* vm, int arg_count,
                                                Value* args) {
    const char* name = "BouncingBalls3DAccelerate";
    if (arg_count != 9) {
        runtimeError(vm, "%s expects 9 arguments.", name);
        return makeVoid();
    }

    NumericVarRef targetFps;
    NumericVarRef frameDelay;
    NumericVarRef deltaTime;
    NumericVarRef minSpeed;
    NumericVarRef maxSpeed;
    NumericVarRef cameraDistance;
    if (!fetchNumericVarRef(vm, &args[0], name, "target FPS", &targetFps) ||
        !fetchNumericVarRef(vm, &args[1], name, "frame delay", &frameDelay) ||
        !fetchNumericVarRef(vm, &args[2], name, "delta time", &deltaTime) ||
        !fetchNumericVarRef(vm, &args[3], name, "minimum speed", &minSpeed) ||
        !fetchNumericVarRef(vm, &args[4], name, "maximum speed", &maxSpeed) ||
        !fetchNumericVarRef(vm, &args[5], name, "camera distance",
                            &cameraDistance)) {
        return makeVoid();
    }

    for (int i = 6; i < 9; ++i) {
        if (!IS_NUMERIC(args[i])) {
            runtimeError(vm, "%s expects numeric scaling factors.", name);
            return makeVoid();
        }
    }

    double fpsMultiplier = (double)asLd(args[6]);
    double speedMultiplier = (double)asLd(args[7]);
    double cameraScale = (double)asLd(args[8]);
    if (!(fpsMultiplier > 0.0)) {
        runtimeError(vm, "%s requires a positive FPS multiplier.", name);
        return makeVoid();
    }
    if (!(speedMultiplier > 0.0)) {
        runtimeError(vm, "%s requires a positive speed multiplier.", name);
        return makeVoid();
    }
    if (!(cameraScale > 0.0)) {
        runtimeError(vm, "%s requires a positive camera scale.", name);
        return makeVoid();
    }

    long double baseTargetFps = asLd(*targetFps.slot);
    if (baseTargetFps < 1.0L) baseTargetFps = 60.0L;
    long double boostedFps = baseTargetFps * fpsMultiplier;
    if (boostedFps < 30.0L) boostedFps = 30.0L;
    if (boostedFps > 480.0L) boostedFps = 480.0L;

    long double boostedDeltaTime = 1.0L / boostedFps;
    long double boostedFrameDelay = 1000.0L / boostedFps;

    long double baseMinSpeed = fabsl(asLd(*minSpeed.slot));
    long double baseMaxSpeed = fabsl(asLd(*maxSpeed.slot));
    if (baseMinSpeed < 1.0L) baseMinSpeed = 1.0L;
    if (baseMaxSpeed < baseMinSpeed) baseMaxSpeed = baseMinSpeed;
    long double boostedMinSpeed = baseMinSpeed * speedMultiplier;
    long double boostedMaxSpeed = baseMaxSpeed * speedMultiplier;
    if (boostedMaxSpeed < boostedMinSpeed) boostedMaxSpeed = boostedMinSpeed;

    long double baseCamera = fabsl(asLd(*cameraDistance.slot));
    if (baseCamera < 120.0L) baseCamera = 120.0L;
    long double boostedCamera = baseCamera * cameraScale;
    if (boostedCamera < 120.0L) boostedCamera = 120.0L;

    assignNumericVar(&targetFps, boostedFps);
    assignNumericVar(&deltaTime, boostedDeltaTime);
    assignNumericVar(&frameDelay, boostedFrameDelay);
    assignNumericVar(&minSpeed, boostedMinSpeed);
    assignNumericVar(&maxSpeed, boostedMaxSpeed);
    assignNumericVar(&cameraDistance, boostedCamera);

    return makeVoid();
}

void cleanupBalls3DRenderingResources(void) {
#ifdef SDL
    gfx3dReleaseResources();
    if (gSphereDisplayListCache.initialized) {
        if (gSdlGLContext) {
            destroySphereDisplayList();
        } else {
            gSphereDisplayListCache.displayListId = 0;
            gSphereDisplayListCache.initialized = false;
        }
    }
    gSphereDisplayListSupported = true;
#endif
    freeBalls3DWorkBuffers(&balls3dWorkBuffers);
}

void registerBalls3DBuiltins(void) {
    registerVmBuiltin("bouncingballs3dstep", vmBuiltinBouncingBalls3DStep,
                      BUILTIN_TYPE_PROCEDURE, "BouncingBalls3DStep");
    registerVmBuiltin("bouncingballs3dstepultra", vmBuiltinBouncingBalls3DStepUltra,
                      BUILTIN_TYPE_PROCEDURE, "BouncingBalls3DStepUltra");
    registerVmBuiltin("bouncingballs3dstepadvanced",
                      vmBuiltinBouncingBalls3DStepAdvanced, BUILTIN_TYPE_PROCEDURE,
                      "BouncingBalls3DStepAdvanced");
    registerVmBuiltin("bouncingballs3dstepultraadvanced",
                      vmBuiltinBouncingBalls3DStepUltraAdvanced,
                      BUILTIN_TYPE_PROCEDURE, "BouncingBalls3DStepUltraAdvanced");
    registerVmBuiltin("bouncingballs3daccelerate", vmBuiltinBouncingBalls3DAccelerate,
                      BUILTIN_TYPE_PROCEDURE, "BouncingBalls3DAccelerate");
    registerVmBuiltin("bouncingballs3ddrawunitspherefast",
                      vmBuiltinBouncingBalls3DDrawUnitSphereFast,
                      BUILTIN_TYPE_PROCEDURE, "BouncingBalls3DDrawUnitSphereFast");
}
