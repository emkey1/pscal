#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"

void registerLandscapeBuiltins(void);

void registerUserBuiltins(void) {
    const char *category = "user";
    extBuiltinRegisterCategory(category);
    extBuiltinRegisterGroup(category, "landscape");
    extBuiltinRegisterFunction(category, "landscape", "LandscapeDrawTerrain");
    extBuiltinRegisterFunction(category, "landscape", "LandscapeDrawWater");
    extBuiltinRegisterFunction(category, "landscape", "LandscapePrecomputeWorldCoords");
    extBuiltinRegisterFunction(category, "landscape", "LandscapePrecomputeWaterOffsets");

    registerLandscapeBuiltins();
}
