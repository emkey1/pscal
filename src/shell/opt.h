#ifndef SHELL_OPT_H
#define SHELL_OPT_H

#include "shell/ast.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enable_constant_folding;
} ShellOptConfig;

void shellRunOptimizations(ShellProgram *program, const ShellOptConfig *config);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_OPT_H */
