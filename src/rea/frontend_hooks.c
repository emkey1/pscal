#include "rea/frontend_hooks.h"

#include <stddef.h>

#include "rea/parser.h"
#include "rea/semantic.h"
#include "rea/state.h"

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

/* --- Mandatory entry points: default to the built-in Rea implementation. --- */

AST *reaFrontendParseSource(const char *source) {
    return g_rea_frontend_hooks.parseSource ? g_rea_frontend_hooks.parseSource(source)
                                            : parseRea(source);
}

void reaFrontendSetStrictMode(int enable) {
    if (g_rea_frontend_hooks.setStrictMode) g_rea_frontend_hooks.setStrictMode(enable);
    else reaSetStrictMode(enable);
}

void reaFrontendResetSymbolState(void) {
    if (g_rea_frontend_hooks.resetSymbolState) g_rea_frontend_hooks.resetSymbolState();
    else reaResetSymbolState();
}

void reaFrontendInvalidateGlobalState(void) {
    if (g_rea_frontend_hooks.invalidateGlobalState) g_rea_frontend_hooks.invalidateGlobalState();
    else reaInvalidateGlobalState();
}

void reaFrontendSemanticSetSourcePath(const char *path) {
    if (g_rea_frontend_hooks.semanticSetSourcePath) g_rea_frontend_hooks.semanticSetSourcePath(path);
    else reaSemanticSetSourcePath(path);
}

void reaFrontendPerformSemanticAnalysis(AST *root) {
    if (g_rea_frontend_hooks.performSemanticAnalysis) g_rea_frontend_hooks.performSemanticAnalysis(root);
    else reaPerformSemanticAnalysis(root);
}

int reaFrontendGetLoadedModuleCount(void) {
    return g_rea_frontend_hooks.getLoadedModuleCount ? g_rea_frontend_hooks.getLoadedModuleCount()
                                                     : reaGetLoadedModuleCount();
}

AST *reaFrontendGetModuleAST(int index) {
    return g_rea_frontend_hooks.getModuleAST ? g_rea_frontend_hooks.getModuleAST(index)
                                             : reaGetModuleAST(index);
}

const char *reaFrontendGetModulePath(int index) {
    return g_rea_frontend_hooks.getModulePath ? g_rea_frontend_hooks.getModulePath(index)
                                              : reaGetModulePath(index);
}

const char *reaFrontendGetModuleName(int index) {
    return g_rea_frontend_hooks.getModuleName ? g_rea_frontend_hooks.getModuleName(index)
                                              : reaGetModuleName(index);
}

char *reaFrontendResolveImportPath(const char *path) {
    return g_rea_frontend_hooks.resolveImportPath ? g_rea_frontend_hooks.resolveImportPath(path)
                                                  : reaResolveImportPath(path);
}

/* --- Optional behaviours: default to no-op / NULL. --- */

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
