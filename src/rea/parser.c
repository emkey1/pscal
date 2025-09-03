#include "rea/parser.h"

AST *parseRea(const char *source) {
    ReaLexer lexer;
    reaInitLexer(&lexer, source);
    ReaToken t = reaNextToken(&lexer);
    (void)t;
    return NULL;
}

