#ifndef LEXER_H
#define LEXER_H

#include "types.h"

typedef struct {
    const char *text;
    size_t pos;
    char current_char;
    int line;
    int column;
} Lexer;

/* Keyword mapping */
typedef struct {
    const char *keyword;
    TokenType token_type;
} Keyword;

void initLexer(Lexer *lexer, const char *text);
void advance(Lexer *lexer);
void skipWhitespace(Lexer *lexer);
Token *number(Lexer *lexer);
Token *identifier(Lexer *lexer);
Token *stringLiteral(Lexer *lexer);
Token *getNextToken(Lexer *lexer);
void lexer_error(Lexer *lexer, const char *msg);

#endif // LEXER_H
