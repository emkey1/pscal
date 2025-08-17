#ifndef TINYC_CODEGEN_H
#define TINYC_CODEGEN_H

#include "tinyc/ast.h"
#include "compiler/bytecode.h"

void tinyc_compile(ASTNodeTinyC *program, BytecodeChunk *chunk);

#endif
