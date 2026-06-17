#ifndef CLIKE_TRANSLATE_H
#define CLIKE_TRANSLATE_H

#include "clike/ast.h"
#include "ast/ast.h"

/*
 * Lower a CLike front-end AST (ASTNodeClike) into the shared PSCAL AST so it
 * can be fed to the common pipeline: annotateTypes() then
 * compileASTToBytecode(). This replaces the bespoke clikeCompile() codegen and
 * makes CLike a first-class shared-AST front end alongside Pascal and Rea.
 *
 * Returns a freshly allocated AST_PROGRAM root (caller owns it; free with
 * freeAST), or NULL on failure.
 */
AST *translateClikeToShared(ASTNodeClike *program);

#endif /* CLIKE_TRANSLATE_H */
