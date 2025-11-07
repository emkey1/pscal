#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"

void registerLandscapeBuiltins(void);
void registerSierpinskiBuiltins(void);

void registerUserBuiltins(void) {
    const char *category = "user";
    extBuiltinRegisterCategory(category);
    extBuiltinRegisterGroup(category, "landscape");
    extBuiltinRegisterGroup(category, "landscape/configure");
    extBuiltinRegisterGroup(category, "landscape/rendering");
    extBuiltinRegisterGroup(category, "landscape/precompute");
    extBuiltinRegisterGroup(category, "demos");
    extBuiltinRegisterFunction(category, "landscape/configure",
                               "LandscapeConfigureProcedural");
    extBuiltinRegisterFunction(category, "landscape/rendering",
                               "LandscapeDrawTerrain");
    extBuiltinRegisterFunction(category, "landscape/rendering",
                               "LandscapeDrawWater");
    extBuiltinRegisterFunction(category, "landscape/precompute",
                               "LandscapePrecomputeWorldCoords");
    extBuiltinRegisterFunction(category, "landscape/precompute",
                               "LandscapePrecomputeWaterOffsets");
    extBuiltinRegisterFunction(category, "landscape/precompute",
                               "LandscapeBuildHeightField");
    extBuiltinRegisterFunction(category, "landscape/precompute",
                               "LandscapeBakeVertexData");
    extBuiltinRegisterFunction(category, "demos",
                               "SierpinskiSpawnWorker");
    extBuiltinRegisterFunction(category, "demos",
                               "SierpinskiReleaseWorkers");

    registerLandscapeBuiltins();
    registerSierpinskiBuiltins();
}
