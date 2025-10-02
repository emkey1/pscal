#ifndef SHELL_BUILTINS_H
#define SHELL_BUILTINS_H

#include "symbol/symbol.h"
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void shellRegisterBuiltins(HashTable *table);
int shellGetBuiltinId(const char *name);
const char *shellBuiltinCanonicalName(const char *name);
bool shellIsBuiltinName(const char *name);
void shellDumpBuiltins(FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_BUILTINS_H */
