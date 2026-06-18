#ifndef PSCAL_REA_FRONTEND_HOOKS_H
#define PSCAL_REA_FRONTEND_HOOKS_H

#include "ast/ast.h"

/*
 * Engine-owned interface for the behaviours the shared Rea/Aether frontend
 * engine needs from whichever front end is driving it.
 *
 * Two kinds of members:
 *   - Mandatory entry points (parse, semantic analysis, module access, ...):
 *     every front end provides these. The engine accessors fall back to the
 *     built-in Rea implementations when a front end installs no override, so
 *     the plain `rea` build needs no registration at all.
 *   - Optional behaviours (diagnostic codes, source rewriting, verbose-compat):
 *     default to no-ops / NULL when unset.
 *
 * Front ends install overrides via reaSetFrontendHooks(). This frees the engine
 * of any compile- or link-time dependency on a specific front end (no -D name
 * dispatch, no front-end headers), which is what lets it be lifted into its own
 * repo while Aether layers on top by registering hooks at startup.
 */
typedef struct ReaFrontendHooks {
    /* Mandatory entry points (NULL => engine uses its built-in Rea behaviour). */
    AST *(*parseSource)(const char *source);
    void (*setStrictMode)(int enable);
    void (*resetSymbolState)(void);
    void (*invalidateGlobalState)(void);
    void (*semanticSetSourcePath)(const char *path);
    void (*performSemanticAnalysis)(AST *root);
    int (*getLoadedModuleCount)(void);
    AST *(*getModuleAST)(int index);
    const char *(*getModulePath)(int index);
    const char *(*getModuleName)(int index);
    char *(*resolveImportPath)(const char *path);

    /* Optional behaviours (NULL => no-op / NULL result). */
    const char *(*inferDiagnosticCode)(const char *kind, const char *detail);
    char *(*rewriteSource)(const char *source, const char *path);
    void (*setVerboseCompatibilityDiagnostics)(int enable);
    int (*getVerboseCompatibilityDiagnostics)(void);
} ReaFrontendHooks;

/* Install the active front end's hooks. Passing NULL clears them. */
void reaSetFrontendHooks(const ReaFrontendHooks *hooks);

/* Engine-side accessors. Each is safe with no hooks installed: entry points run
 * the built-in Rea behaviour, optional behaviours no-op. */
AST *reaFrontendParseSource(const char *source);
void reaFrontendSetStrictMode(int enable);
void reaFrontendResetSymbolState(void);
void reaFrontendInvalidateGlobalState(void);
void reaFrontendSemanticSetSourcePath(const char *path);
void reaFrontendPerformSemanticAnalysis(AST *root);
int reaFrontendGetLoadedModuleCount(void);
AST *reaFrontendGetModuleAST(int index);
const char *reaFrontendGetModulePath(int index);
const char *reaFrontendGetModuleName(int index);
char *reaFrontendResolveImportPath(const char *path);

const char *reaFrontendInferDiagnosticCode(const char *kind, const char *detail);
char *reaFrontendRewriteSource(const char *source, const char *path);
void reaFrontendSetVerboseCompatibilityDiagnostics(int enable);
int reaFrontendGetVerboseCompatibilityDiagnostics(void);

#endif /* PSCAL_REA_FRONTEND_HOOKS_H */
