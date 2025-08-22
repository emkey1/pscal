#ifndef PASCAL_OPT_H
#define PASCAL_OPT_H

#include "Pascal/ast.h"

// Perform simple AST optimizations such as constant folding and
// dead-branch elimination. Returns the potentially replaced root node.
AST* optimizePascalAST(AST* node);

#endif
