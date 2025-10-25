#ifndef SHELL_FUNCTION_H
#define SHELL_FUNCTION_H

#include <stdint.h>
#include "compiler/bytecode.h"

#define SHELL_COMPILED_FUNCTION_MAGIC 0x5343464eU /* 'SCFN' */

typedef struct ShellCompiledFunction {
    uint32_t magic;
    BytecodeChunk chunk;
} ShellCompiledFunction;

#endif /* SHELL_FUNCTION_H */
