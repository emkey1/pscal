#ifndef REA_SEMANTIC_H
#define REA_SEMANTIC_H

#include "ast/ast.h"

/* Perform semantic analysis on the given AST.  This pass validates
 * class declarations, inheritance hierarchies and usage of fields and
 * methods.  Any detected issues are reported via stderr and the global
 * pascal_semantic_error_count variable. */
void reaPerformSemanticAnalysis(AST *root);
void reaSemanticSetSourcePath(const char *path);
int reaGetLoadedModuleCount(void);
AST *reaGetModuleAST(int index);
const char *reaGetModulePath(int index);
const char *reaGetModuleName(int index);
char *reaResolveImportPath(const char *path);
void reaSemanticResetState(void);

#endif /* REA_SEMANTIC_H */
