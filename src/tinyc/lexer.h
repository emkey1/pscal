#ifndef TINYC_LEXER_H
#define TINYC_LEXER_H

#include <stdbool.h>

typedef enum {
    TINYCTOKEN_INT,
    TINYCTOKEN_VOID,
    TINYCTOKEN_IF,
    TINYCTOKEN_ELSE,
    TINYCTOKEN_WHILE,
    TINYCTOKEN_RETURN,
    TINYCTOKEN_IDENTIFIER,
    TINYCTOKEN_NUMBER,
    TINYCTOKEN_PLUS,
    TINYCTOKEN_MINUS,
    TINYCTOKEN_STAR,
    TINYCTOKEN_SLASH,
    TINYCTOKEN_BANG,
    TINYCTOKEN_BANG_EQUAL,
    TINYCTOKEN_EQUAL,
    TINYCTOKEN_EQUAL_EQUAL,
    TINYCTOKEN_LESS,
    TINYCTOKEN_LESS_EQUAL,
    TINYCTOKEN_GREATER,
    TINYCTOKEN_GREATER_EQUAL,
    TINYCTOKEN_AND_AND,
    TINYCTOKEN_OR_OR,
    TINYCTOKEN_SEMICOLON,
    TINYCTOKEN_COMMA,
    TINYCTOKEN_LPAREN,
    TINYCTOKEN_RPAREN,
    TINYCTOKEN_LBRACE,
    TINYCTOKEN_RBRACE,
    TINYCTOKEN_LBRACKET,
    TINYCTOKEN_RBRACKET,
    TINYCTOKEN_EOF,
    TINYCTOKEN_UNKNOWN
} TinyCTokenType;

typedef struct {
    TinyCTokenType type;
    const char *lexeme;
    int length;
    int line;
    int int_val;
} TinyCToken;

typedef struct {
    const char *src;
    int pos;
    int line;
} TinyCLexer;

void tinyc_initLexer(TinyCLexer *lexer, const char *source);
TinyCToken tinyc_nextToken(TinyCLexer *lexer);
const char* tinycTokenTypeToString(TinyCTokenType type);

#endif
