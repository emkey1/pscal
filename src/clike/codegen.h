#ifndef CLIKE_CODEGEN_H
#define CLIKE_CODEGEN_H

#include "clike/ast.h"
#include "compiler/bytecode.h"

void clike_compile(ASTNodeClike *program, BytecodeChunk *chunk);

#endif
