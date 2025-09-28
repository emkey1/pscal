#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"

void registerLandscapeBuiltins(void);

void registerUserBuiltins(void) {
    const char *category = "user";
    extBuiltinRegisterCategory(category);
    extBuiltinRegisterGroup(category, "landscape");
    extBuiltinRegisterGroup(category, "landscape/rendering");
    extBuiltinRegisterGroup(category, "landscape/precompute");
    extBuiltinRegisterFunction(category, "landscape/rendering",
                               "LandscapeDrawTerrain");
    extBuiltinRegisterFunction(category, "landscape/rendering",
                               "LandscapeDrawWater");
    extBuiltinRegisterFunction(category, "landscape/precompute",
                               "LandscapePrecomputeWorldCoords");
    extBuiltinRegisterFunction(category, "landscape/precompute",
                               "LandscapePrecomputeWaterOffsets");

    registerLandscapeBuiltins();
}
