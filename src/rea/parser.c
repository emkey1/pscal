#include "rea/parser.h"

AST *parseRea(const char *source) {
    ReaLexer lexer;
    reaInitLexer(&lexer, source);

    // For now simply exercise the lexer by consuming tokens until EOF. The
    // resulting tokens are ignored; a proper AST will be constructed in a
    // later phase of development.
    ReaToken t;
    do {
        t = reaNextToken(&lexer);
    } while (t.type != REA_TOKEN_EOF);

    return NULL;
}

