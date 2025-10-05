#ifndef SHELL_FUNCTION_H
#define SHELL_FUNCTION_H

#include "compiler/bytecode.h"

typedef struct ShellCompiledFunction {
    BytecodeChunk chunk;
} ShellCompiledFunction;

#endif /* SHELL_FUNCTION_H */
