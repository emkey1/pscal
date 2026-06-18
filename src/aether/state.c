#include "aether/state.h"

#include "aether/parser.h"
#include "aether/diagnostics.h"
#include "aether/translate.h"
#include "rea/state.h"
#include "rea/frontend_hooks.h"

static int g_aether_verbose_compatibility_diagnostics = 0;

void aetherResetSymbolState(void) {
    reaResetSymbolState();
}

void aetherInvalidateGlobalState(void) {
    aetherClearLastSource();
    reaInvalidateGlobalState();
}

void aetherSetVerboseCompatibilityDiagnostics(int enable) {
    g_aether_verbose_compatibility_diagnostics = enable ? 1 : 0;
}

int aetherGetVerboseCompatibilityDiagnostics(void) {
    return g_aether_verbose_compatibility_diagnostics;
}

/* Install the Aether implementations of the shared engine's frontend hooks.
 * Runs before main(); the plain Rea front end does not compile this translation
 * unit, so it falls back to the engine's no-op defaults. */
__attribute__((constructor))
static void aetherInstallFrontendHooks(void) {
    static const ReaFrontendHooks hooks = {
        .inferDiagnosticCode = aetherInferDiagnosticCode,
        .rewriteSource = aetherRewriteSource,
        .setVerboseCompatibilityDiagnostics = aetherSetVerboseCompatibilityDiagnostics,
        .getVerboseCompatibilityDiagnostics = aetherGetVerboseCompatibilityDiagnostics,
    };
    reaSetFrontendHooks(&hooks);
}
