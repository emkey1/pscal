#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"

void registerLandscapeBuiltins(void);
void registerBalls3DBuiltins(void);

void registerUserBuiltins(void) {
    extBuiltinRegisterCategory("user");
    extBuiltinRegisterFunction("user", "LandscapeDrawTerrain");
    extBuiltinRegisterFunction("user", "LandscapeDrawWater");
    extBuiltinRegisterFunction("user", "LandscapePrecomputeWorldCoords");
    extBuiltinRegisterFunction("user", "LandscapePrecomputeWaterOffsets");
    extBuiltinRegisterFunction("user", "BouncingBalls3DStep");
    extBuiltinRegisterFunction("user", "BouncingBalls3DStepAdvanced");

    registerLandscapeBuiltins();
    registerBalls3DBuiltins();
}
