#include "rea/lexer.h"

void reaInitLexer(ReaLexer *lexer, const char *source) {
    lexer->source = source;
    lexer->pos = 0;
    lexer->line = 1;
}

ReaToken reaNextToken(ReaLexer *lexer) {
    ReaToken t;
    t.type = REA_TOKEN_EOF;
    t.start = lexer->source + lexer->pos;
    t.length = 0;
    t.line = lexer->line;
    return t;
}

