#include "ext_builtins/registry.h"

void registerBalls3DBuiltins(void);

void registerThreeDBuiltins(void) {
    const char *category = "3d";
    extBuiltinRegisterCategory(category);
    extBuiltinRegisterGroup(category, "physics");
    extBuiltinRegisterGroup(category, "rendering");
    extBuiltinRegisterFunction(category, "physics", "BouncingBalls3DStep");
    extBuiltinRegisterFunction(category, "physics", "BouncingBalls3DStepUltra");
    extBuiltinRegisterFunction(category, "physics", "BouncingBalls3DStepAdvanced");
    extBuiltinRegisterFunction(category, "physics", "BouncingBalls3DStepUltraAdvanced");
    extBuiltinRegisterFunction(category, "physics", "BouncingBalls3DAccelerate");
    extBuiltinRegisterFunction(category, "rendering", "BouncingBalls3DDrawUnitSphereFast");

    registerBalls3DBuiltins();
}
