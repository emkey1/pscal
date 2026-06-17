#ifndef PSCAL_CORE_COMPILED_FUNCTION_H
#define PSCAL_CORE_COMPILED_FUNCTION_H

#include <stdint.h>
#include "compiler/bytecode.h"

/*
 * A compiled routine persisted as a tagged bytecode chunk. Used as a VM Value
 * payload and serialized by the bytecode cache (core). The shell front end
 * compiles its functions into this form, but the type itself is
 * front-end-agnostic (magic + BytecodeChunk). The historical name
 * ShellCompiledFunction is retained to avoid churn across its users.
 */
#define SHELL_COMPILED_FUNCTION_MAGIC 0x5343464eU /* 'SCFN' */

typedef struct ShellCompiledFunction {
    uint32_t magic;
    BytecodeChunk chunk;
} ShellCompiledFunction;

#endif /* PSCAL_CORE_COMPILED_FUNCTION_H */
