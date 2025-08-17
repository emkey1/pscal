#ifndef CLIKE_PARSER_H
#define CLIKE_PARSER_H

#include "clike/lexer.h"
#include "clike/ast.h"

typedef struct {
    ClikeLexer lexer;
    ClikeToken current;
    ClikeToken next;
} ParserClike;

void initParserClike(ParserClike *parser, const char *source);
ASTNodeClike* parseProgramClike(ParserClike *parser);

#endif
