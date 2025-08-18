#ifndef CLIKE_PARSER_H
#define CLIKE_PARSER_H

#include "clike/lexer.h"
#include "clike/ast.h"

typedef struct {
    ClikeLexer lexer;
    ClikeToken current;
    ClikeToken next;
    char **imports;
    int import_count;
    int import_capacity;
} ParserClike;

void initParserClike(ParserClike *parser, const char *source);
ASTNodeClike* parseProgramClike(ParserClike *parser);

extern char **clike_imports;
extern int clike_import_count;

#endif
