#ifndef REA_LEXER_H
#define REA_LEXER_H

#include <stddef.h>

typedef enum {
    REA_TOKEN_EOF = 0,
    REA_TOKEN_UNKNOWN
} ReaTokenType;

typedef struct {
    ReaTokenType type;
    const char *start;
    size_t length;
    int line;
} ReaToken;

typedef struct {
    const char *source;
    size_t pos;
    int line;
} ReaLexer;

void reaInitLexer(ReaLexer *lexer, const char *source);
ReaToken reaNextToken(ReaLexer *lexer);

#endif

