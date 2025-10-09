#ifndef SHELL_SEMANTICS_H
#define SHELL_SEMANTICS_H

#include "shell/ast.h"
#include "symbol/symbol.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    HashTable *builtin_table;
    HashTable *variable_table;
    HashTable *function_table;
    int error_count;
    int warning_count;
} ShellSemanticContext;

typedef struct {
    int error_count;
    int warning_count;
} ShellSemanticResult;

void shellInitSemanticContext(ShellSemanticContext *ctx);
void shellFreeSemanticContext(ShellSemanticContext *ctx);
ShellSemanticResult shellAnalyzeProgram(ShellSemanticContext *ctx, ShellProgram *program);
void shellSemanticsSetWarningSuppressed(bool suppressed);
bool shellSemanticsWarningsSuppressed(void);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_SEMANTICS_H */
