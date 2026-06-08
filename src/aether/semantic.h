#ifndef PSCAL_AETHER_SEMANTIC_H
#define PSCAL_AETHER_SEMANTIC_H

#include "ast/ast.h"

void aetherPerformSemanticAnalysis(AST *root);
void aetherSemanticSetSourcePath(const char *path);
const char *aetherSemanticGetSourcePath(void);
int aetherGetLoadedModuleCount(void);
AST *aetherGetModuleAST(int index);
const char *aetherGetModulePath(int index);
const char *aetherGetModuleName(int index);
char *aetherResolveImportPath(const char *path);
void aetherSemanticResetState(void);

#endif
