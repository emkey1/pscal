#ifndef CLIKE_OPT_H
#define CLIKE_OPT_H

#include "clike/ast.h"

// Perform simple AST optimizations such as constant folding and
// dead-branch elimination. The function returns the potentially
// replaced node for convenience.
ASTNodeClike* optimizeClikeAST(ASTNodeClike* node);

#endif
