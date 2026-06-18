#ifndef PSCAL_REA_FRONTEND_HOOKS_H
#define PSCAL_REA_FRONTEND_HOOKS_H

/*
 * Engine-owned interface for the small set of behaviours the shared Rea/Aether
 * frontend engine needs from whichever front end is driving it.
 *
 * Front ends register their implementations via reaSetFrontendHooks(); the
 * engine only ever calls the reaFrontend*() accessors below, which fall back to
 * safe no-op defaults when no hook is installed (e.g. the plain Rea front end).
 * This keeps the shared engine free of any hard dependency on a specific front
 * end's headers or sources, which is what lets the engine be extracted on its
 * own while Aether layers on top of it.
 */
typedef struct ReaFrontendHooks {
    /* Map a diagnostic (kind, detail) to a front-end-specific code, or NULL. */
    const char *(*inferDiagnosticCode)(const char *kind, const char *detail);
    /* Lower/rewrite source before compilation; returns a malloc'd buffer or NULL. */
    char *(*rewriteSource)(const char *source, const char *path);
    /* Toggle / query verbose compatibility diagnostics. */
    void (*setVerboseCompatibilityDiagnostics)(int enable);
    int (*getVerboseCompatibilityDiagnostics)(void);
} ReaFrontendHooks;

/* Install the active front end's hooks. Passing NULL clears them. */
void reaSetFrontendHooks(const ReaFrontendHooks *hooks);

/* Engine-side accessors. Each is safe to call with no hooks installed. */
const char *reaFrontendInferDiagnosticCode(const char *kind, const char *detail);
char *reaFrontendRewriteSource(const char *source, const char *path);
void reaFrontendSetVerboseCompatibilityDiagnostics(int enable);
int reaFrontendGetVerboseCompatibilityDiagnostics(void);

#endif /* PSCAL_REA_FRONTEND_HOOKS_H */
