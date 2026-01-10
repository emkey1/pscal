#ifndef CLIKE_SEMANTICS_H
#define CLIKE_SEMANTICS_H

#include "clike/ast.h"

void analyzeSemanticsClike(ASTNodeClike *program, const char *current_path);
void clikeResetSemanticsState(void);

#endif
