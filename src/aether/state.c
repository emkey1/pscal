#include "aether/state.h"

#include "aether/parser.h"
#include "rea/state.h"

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
