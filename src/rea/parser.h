#ifndef REA_PARSER_H
#define REA_PARSER_H

#include "rea/lexer.h"
#include "ast/ast.h"

AST *parseRea(const char *source);

#endif

