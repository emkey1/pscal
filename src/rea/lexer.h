#ifndef REA_LEXER_H
#define REA_LEXER_H

#include <stddef.h>

// Token types for the Rea language. This is intentionally broad – the
// initial front end will recognise a wide variety of punctuation, operators
// and keywords even if the parser does not yet use them. Expanding the enum
// here allows the lexer to be exercised independently of later stages.
typedef enum {
    REA_TOKEN_EOF = 0,
    REA_TOKEN_UNKNOWN,

    // Literals and identifiers
    REA_TOKEN_IDENTIFIER,
    REA_TOKEN_NUMBER,
    REA_TOKEN_STRING,

    // Punctuation
    REA_TOKEN_LEFT_PAREN,
    REA_TOKEN_RIGHT_PAREN,
    REA_TOKEN_LEFT_BRACE,
    REA_TOKEN_RIGHT_BRACE,
    REA_TOKEN_LEFT_BRACKET,
    REA_TOKEN_RIGHT_BRACKET,
    REA_TOKEN_COMMA,
    REA_TOKEN_DOT,
    REA_TOKEN_SEMICOLON,
    REA_TOKEN_COLON,
    REA_TOKEN_QUESTION,
    REA_TOKEN_ARROW,

    // Operators
    REA_TOKEN_PLUS,
    REA_TOKEN_PLUS_PLUS,
    REA_TOKEN_PLUS_EQUAL,
    REA_TOKEN_MINUS,
    REA_TOKEN_MINUS_MINUS,
    REA_TOKEN_MINUS_EQUAL,
    REA_TOKEN_STAR,
    REA_TOKEN_SLASH,
    REA_TOKEN_PERCENT,
    REA_TOKEN_EQUAL,
    REA_TOKEN_EQUAL_EQUAL,
    REA_TOKEN_BANG,
    REA_TOKEN_BANG_EQUAL,
    REA_TOKEN_AND,
    REA_TOKEN_AND_AND,
    REA_TOKEN_OR,
    REA_TOKEN_OR_OR,
    REA_TOKEN_XOR,
    REA_TOKEN_GREATER,
    REA_TOKEN_GREATER_EQUAL,
    REA_TOKEN_LESS,
    REA_TOKEN_LESS_EQUAL,

    // Keywords
    REA_TOKEN_CLASS,
    REA_TOKEN_EXTENDS,
    REA_TOKEN_NEW,
    REA_TOKEN_MYSELF,
    REA_TOKEN_SUPER,
    REA_TOKEN_IF,
    REA_TOKEN_ELSE,
    REA_TOKEN_WHILE,
    REA_TOKEN_FOR,
    REA_TOKEN_DO,
    REA_TOKEN_SWITCH,
    REA_TOKEN_CASE,
    REA_TOKEN_DEFAULT,
    REA_TOKEN_TYPE,
    REA_TOKEN_ALIAS,
    REA_TOKEN_MATCH,
    REA_TOKEN_TRY,
    REA_TOKEN_CATCH,
    REA_TOKEN_THROW,
    REA_TOKEN_BREAK,
    REA_TOKEN_CONTINUE,
    REA_TOKEN_RETURN,
    REA_TOKEN_TRUE,
    REA_TOKEN_FALSE,
    REA_TOKEN_NIL,
    REA_TOKEN_CONST,
    REA_TOKEN_MODULE,
    REA_TOKEN_EXPORT,
    REA_TOKEN_IMPORT,
    REA_TOKEN_SPAWN,
    REA_TOKEN_JOIN,

    // Type keywords
    REA_TOKEN_INT,
    REA_TOKEN_INT64,
    REA_TOKEN_INT32,
    REA_TOKEN_INT16,
    REA_TOKEN_INT8,
    REA_TOKEN_FLOAT,
    REA_TOKEN_FLOAT32,
    REA_TOKEN_LONG_DOUBLE,
    REA_TOKEN_CHAR,
    REA_TOKEN_BYTE,
    REA_TOKEN_STR,
    REA_TOKEN_TEXT,
    REA_TOKEN_MSTREAM,
    REA_TOKEN_VOID,
    REA_TOKEN_BOOL
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
const char* reaTokenTypeToString(ReaTokenType type);

#endif
