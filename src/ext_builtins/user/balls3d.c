#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "vm/vm.h"

#include <math.h>

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

    for (int i = 1; i <= 9; ++i) {
        if (!IS_NUMERIC(args[i])) {
            runtimeError(vm, "%s expects numeric arguments in positions 2-10.", name);
            return makeVoid();
        }
    }
    for (int i = 10; i <= 11; ++i) {
        if (!IS_NUMERIC(args[i])) {
            runtimeError(vm, "%s expects numeric screen dimension arguments.", name);
            return makeVoid();
        }
    }

    double deltaTime = (double)asLd(args[1]);
    double boxWidth = (double)asLd(args[2]);
    double boxHeight = (double)asLd(args[3]);
    double boxDepth = (double)asLd(args[4]);
    double wallElasticity = (double)asLd(args[5]);
    double minSpeed = fabs((double)asLd(args[6]));
    double maxSpeed = fabs((double)asLd(args[7]));
    double drag = (double)asLd(args[8]);
    double cameraDistance = (double)asLd(args[9]);
    double screenWidth = (double)asLd(args[10]);
    double screenHeight = (double)asLd(args[11]);

    if (deltaTime <= 0.0) {
        runtimeError(vm, "%s requires positive delta time.", name);
        return makeVoid();
    }
    if (boxWidth <= 0.0 || boxHeight <= 0.0 || boxDepth <= 0.0) {
        runtimeError(vm, "%s requires positive box dimensions.", name);
        return makeVoid();
    }
    if (wallElasticity < 0.0) {
        runtimeError(vm, "%s requires non-negative wall elasticity.", name);
        return makeVoid();
    }
    if (maxSpeed < 1e-6) {
        runtimeError(vm, "%s requires a positive maximum speed.", name);
        return makeVoid();
    }
    if (minSpeed > maxSpeed) {
        runtimeError(vm, "%s minimum speed exceeds maximum speed.", name);
        return makeVoid();
    }
    if (drag <= 0.0 || drag > 1.0) {
        runtimeError(vm, "%s expects drag between 0 and 1.", name);
        return makeVoid();
    }
    if (cameraDistance <= 0.0) {
        runtimeError(vm, "%s requires positive camera distance.", name);
        return makeVoid();
    }
    if (screenWidth <= 0.0 || screenHeight <= 0.0) {
        runtimeError(vm, "%s requires positive screen dimensions.", name);
        return makeVoid();
    }

    int lower = 0, upper = 0;
    Value* posX = resolveArrayArg(vm, &args[12], name, &lower, &upper);
    if (!posX) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "%s position arrays must start at index 0.", name);
        return makeVoid();
    }
    int arrayUpper = upper;

    Value* posY = resolveArrayArg(vm, &args[13], name, &lower, &upper);
    if (!posY) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "%s position arrays must start at index 0.", name);
        return makeVoid();
    }
    if (upper < arrayUpper) arrayUpper = upper;

    Value* posZ = resolveArrayArg(vm, &args[14], name, &lower, &upper);
    if (!posZ) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "%s position arrays must start at index 0.", name);
        return makeVoid();
    }
    if (upper < arrayUpper) arrayUpper = upper;

    Value* velX = resolveArrayArg(vm, &args[15], name, &lower, &upper);
    if (!velX) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "%s velocity arrays must start at index 0.", name);
        return makeVoid();
    }
    if (upper < arrayUpper) arrayUpper = upper;

    Value* velY = resolveArrayArg(vm, &args[16], name, &lower, &upper);
    if (!velY) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "%s velocity arrays must start at index 0.", name);
        return makeVoid();
    }
    if (upper < arrayUpper) arrayUpper = upper;

    Value* velZ = resolveArrayArg(vm, &args[17], name, &lower, &upper);
    if (!velZ) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "%s velocity arrays must start at index 0.", name);
        return makeVoid();
    }
    if (upper < arrayUpper) arrayUpper = upper;

    Value* radiusArr = resolveArrayArg(vm, &args[18], name, &lower, &upper);
    if (!radiusArr) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "%s radius array must start at index 0.", name);
        return makeVoid();
    }
    if (upper < arrayUpper) arrayUpper = upper;

    Value* screenX = resolveArrayArg(vm, &args[19], name, &lower, &upper);
    if (!screenX) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "%s output arrays must start at index 0.", name);
        return makeVoid();
    }
    if (upper < arrayUpper) arrayUpper = upper;

    Value* screenY = resolveArrayArg(vm, &args[20], name, &lower, &upper);
    if (!screenY) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "%s output arrays must start at index 0.", name);
        return makeVoid();
    }
    if (upper < arrayUpper) arrayUpper = upper;

    Value* screenRadius = resolveArrayArg(vm, &args[21], name, &lower, &upper);
    if (!screenRadius) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "%s output arrays must start at index 0.", name);
        return makeVoid();
    }
    if (upper < arrayUpper) arrayUpper = upper;

    Value* shadeArr = resolveArrayArg(vm, &args[22], name, &lower, &upper);
    if (!shadeArr) return makeVoid();
    if (lower != 0) {
        runtimeError(vm, "%s output arrays must start at index 0.", name);
        return makeVoid();
    }
    if (upper < arrayUpper) arrayUpper = upper;

    if (arrayUpper < ballCount - 1) {
        runtimeError(vm, "%s arrays are smaller than the requested ball count.", name);
        return makeVoid();
    }

    double halfWidth = boxWidth * 0.5;
    double halfHeight = boxHeight * 0.5;
    double nearPlane = 0.0;
    double backPlane = -boxDepth;
    double viewScaleX = screenWidth / boxWidth;
    double viewScaleY = screenHeight / boxHeight;

    for (int i = 0; i < ballCount; ++i) {
        double x = (double)asLd(posX[i]);
        double y = (double)asLd(posY[i]);
        double z = (double)asLd(posZ[i]);
        double vx = (double)asLd(velX[i]);
        double vy = (double)asLd(velY[i]);
        double vz = (double)asLd(velZ[i]);
        double r = (double)asLd(radiusArr[i]);

        if (r <= 0.0) r = 1.0;
        double minX = -halfWidth + r;
        double maxX = halfWidth - r;
        double minY = -halfHeight + r;
        double maxY = halfHeight - r;
        double minZ = backPlane + r;
        double maxZ = nearPlane - r;

        vx *= drag;
        vy *= drag;
        vz *= drag;

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

        vx = enforceSpeed(vx, minSpeed, maxSpeed);
        vy = enforceSpeed(vy, minSpeed, maxSpeed);
        vz = enforceSpeed(vz, minSpeed, maxSpeed);

        assignFloatValue(&posX[i], x);
        assignFloatValue(&posY[i], y);
        assignFloatValue(&posZ[i], z);
        assignFloatValue(&velX[i], vx);
        assignFloatValue(&velY[i], vy);
        assignFloatValue(&velZ[i], vz);
    }

    for (int i = 0; i < ballCount; ++i) {
        double xi = (double)asLd(posX[i]);
        double yi = (double)asLd(posY[i]);
        double zi = (double)asLd(posZ[i]);
        double vxi = (double)asLd(velX[i]);
        double vyi = (double)asLd(velY[i]);
        double vzi = (double)asLd(velZ[i]);
        double ri = (double)asLd(radiusArr[i]);
        double mi = ri * ri * ri;
        if (mi <= 0.0) mi = 1.0;

        for (int j = i + 1; j < ballCount; ++j) {
            double xj = (double)asLd(posX[j]);
            double yj = (double)asLd(posY[j]);
            double zj = (double)asLd(posZ[j]);
            double vxj = (double)asLd(velX[j]);
            double vyj = (double)asLd(velY[j]);
            double vzj = (double)asLd(velZ[j]);
            double rj = (double)asLd(radiusArr[j]);
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

            assignFloatValue(&posX[i], xi);
            assignFloatValue(&posY[i], yi);
            assignFloatValue(&posZ[i], zi);
            assignFloatValue(&velX[i], vxi);
            assignFloatValue(&velY[i], vyi);
            assignFloatValue(&velZ[i], vzi);

            assignFloatValue(&posX[j], xj);
            assignFloatValue(&posY[j], yj);
            assignFloatValue(&posZ[j], zj);
            assignFloatValue(&velX[j], vxj);
            assignFloatValue(&velY[j], vyj);
            assignFloatValue(&velZ[j], vzj);
        }
    }

    for (int i = 0; i < ballCount; ++i) {
        double x = (double)asLd(posX[i]);
        double y = (double)asLd(posY[i]);
        double z = (double)asLd(posZ[i]);
        double r = (double)asLd(radiusArr[i]);

        if (z > nearPlane - r) {
            z = nearPlane - r;
            assignFloatValue(&posZ[i], z);
        }
        if (z < backPlane + r) {
            z = backPlane + r;
            assignFloatValue(&posZ[i], z);
        }

        double denom = cameraDistance - z;
        if (denom <= 1e-6) {
            assignFloatValue(&shadeArr[i], -1.0);
            continue;
        }
        double perspective = cameraDistance / denom;
        double sx = screenWidth * 0.5 + x * perspective * viewScaleX;
        double sy = screenHeight * 0.5 - y * perspective * viewScaleY;
        double sr = r * perspective * (viewScaleX + viewScaleY) * 0.5;
        if (sr < 1.0) sr = 1.0;

        double depthFactor = clampd(-z / boxDepth, 0.0, 1.0);
        double shade = 0.25 + 0.75 * depthFactor;

        assignFloatValue(&screenX[i], sx);
        assignFloatValue(&screenY[i], sy);
        assignFloatValue(&screenRadius[i], sr);
        assignFloatValue(&shadeArr[i], shade);
    }

    return makeVoid();
}

void registerBalls3DBuiltins(void) {
    registerVmBuiltin("bouncingballs3dstep", vmBuiltinBouncingBalls3DStep,
                      BUILTIN_TYPE_PROCEDURE, "BouncingBalls3DStep");
}
