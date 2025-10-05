#ifndef SHELL_CODEGEN_H
#define SHELL_CODEGEN_H

#include "shell/ast.h"
#include "compiler/bytecode.h"

#ifdef __cplusplus
extern "C" {
#endif

void shellCompile(const ShellProgram *program, BytecodeChunk *chunk);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_CODEGEN_H */
