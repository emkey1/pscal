#ifndef CLIKE_CODEGEN_H
#define CLIKE_CODEGEN_H

#include "clike/ast.h"
#include "compiler/bytecode.h"

void clikeCompile(ASTNodeClike *program, BytecodeChunk *chunk);
void clikeResetCodegenState(void);

#endif
