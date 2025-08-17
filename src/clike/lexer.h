#ifndef CLIKE_LEXER_H
#define CLIKE_LEXER_H

#include <stdbool.h>

typedef enum {
    CLIKE_TOKEN_INT,
    CLIKE_TOKEN_VOID,
    CLIKE_TOKEN_IF,
    CLIKE_TOKEN_ELSE,
    CLIKE_TOKEN_WHILE,
    CLIKE_TOKEN_RETURN,
    CLIKE_TOKEN_IDENTIFIER,
    CLIKE_TOKEN_NUMBER,
    CLIKE_TOKEN_PLUS,
    CLIKE_TOKEN_MINUS,
    CLIKE_TOKEN_STAR,
    CLIKE_TOKEN_SLASH,
    CLIKE_TOKEN_BANG,
    CLIKE_TOKEN_BANG_EQUAL,
    CLIKE_TOKEN_EQUAL,
    CLIKE_TOKEN_EQUAL_EQUAL,
    CLIKE_TOKEN_LESS,
    CLIKE_TOKEN_LESS_EQUAL,
    CLIKE_TOKEN_GREATER,
    CLIKE_TOKEN_GREATER_EQUAL,
    CLIKE_TOKEN_AND_AND,
    CLIKE_TOKEN_OR_OR,
    CLIKE_TOKEN_SEMICOLON,
    CLIKE_TOKEN_COMMA,
    CLIKE_TOKEN_LPAREN,
    CLIKE_TOKEN_RPAREN,
    CLIKE_TOKEN_LBRACE,
    CLIKE_TOKEN_RBRACE,
    CLIKE_TOKEN_LBRACKET,
    CLIKE_TOKEN_RBRACKET,
    CLIKE_TOKEN_EOF,
    CLIKE_TOKEN_UNKNOWN
} ClikeTokenType;

typedef struct {
    ClikeTokenType type;
    const char *lexeme;
    int length;
    int line;
    int int_val;
} ClikeToken;

typedef struct {
    const char *src;
    int pos;
    int line;
} ClikeLexer;

void clike_initLexer(ClikeLexer *lexer, const char *source);
ClikeToken clike_nextToken(ClikeLexer *lexer);
const char* clikeTokenTypeToString(ClikeTokenType type);

#endif
