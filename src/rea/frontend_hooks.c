#include "rea/frontend_hooks.h"

#include <stddef.h>

/* Zero-initialised => every hook NULL => engine uses its built-in defaults. */
static ReaFrontendHooks g_rea_frontend_hooks;

void reaSetFrontendHooks(const ReaFrontendHooks *hooks) {
    if (hooks) {
        g_rea_frontend_hooks = *hooks;
    } else {
        ReaFrontendHooks cleared = {0};
        g_rea_frontend_hooks = cleared;
    }
}

const char *reaFrontendInferDiagnosticCode(const char *kind, const char *detail) {
    if (g_rea_frontend_hooks.inferDiagnosticCode) {
        return g_rea_frontend_hooks.inferDiagnosticCode(kind, detail);
    }
    return NULL;
}

char *reaFrontendRewriteSource(const char *source, const char *path) {
    if (g_rea_frontend_hooks.rewriteSource) {
        return g_rea_frontend_hooks.rewriteSource(source, path);
    }
    return NULL;
}

void reaFrontendSetVerboseCompatibilityDiagnostics(int enable) {
    if (g_rea_frontend_hooks.setVerboseCompatibilityDiagnostics) {
        g_rea_frontend_hooks.setVerboseCompatibilityDiagnostics(enable);
    }
}

int reaFrontendGetVerboseCompatibilityDiagnostics(void) {
    if (g_rea_frontend_hooks.getVerboseCompatibilityDiagnostics) {
        return g_rea_frontend_hooks.getVerboseCompatibilityDiagnostics();
    }
    return 0;
}
