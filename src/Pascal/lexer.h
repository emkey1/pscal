#ifndef LEXER_H
#define LEXER_H

#include "core/types.h"
#include <stdbool.h>

typedef struct {
    const char *text;
    size_t pos;
    char current_char;
    int line;
    int column;
    size_t text_len;
    bool has_pending_builtin_override;
    char *pending_builtin_override_names;
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
void lexerError(Lexer *lexer, const char *msg);
bool lexerConsumeOverrideBuiltinDirective(Lexer *lexer, const char *procedure_name);

#endif // LEXER_H
