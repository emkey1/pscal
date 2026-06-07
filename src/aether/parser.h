#ifndef PSCAL_AETHER_PARSER_H
#define PSCAL_AETHER_PARSER_H

#include "ast/ast.h"

AST *parseAether(const char *source);
void aetherSetStrictMode(int enable);
const char *aetherGetLastSource(void);
void aetherClearLastSource(void);

#endif
