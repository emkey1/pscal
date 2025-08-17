#ifndef TINYC_PARSER_H
#define TINYC_PARSER_H

#include "tinyc/lexer.h"
#include "tinyc/ast.h"

typedef struct {
    TinyCLexer lexer;
    TinyCToken current;
    TinyCToken next;
} ParserTinyC;

void initParserTinyC(ParserTinyC *parser, const char *source);
ASTNodeTinyC* parseProgramTinyC(ParserTinyC *parser);

#endif
