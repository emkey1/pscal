#ifndef SHELL_LEXER_H
#define SHELL_LEXER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SHELL_TOKEN_WORD,
    SHELL_TOKEN_PARAMETER,
    SHELL_TOKEN_ASSIGNMENT,
    SHELL_TOKEN_IO_NUMBER,
    SHELL_TOKEN_NEWLINE,
    SHELL_TOKEN_SEMICOLON,
    SHELL_TOKEN_AMPERSAND,
    SHELL_TOKEN_PIPE,
    SHELL_TOKEN_PIPE_AMP,
    SHELL_TOKEN_AND_AND,
    SHELL_TOKEN_OR_OR,
    SHELL_TOKEN_LPAREN,
    SHELL_TOKEN_RPAREN,
    SHELL_TOKEN_LBRACE,
    SHELL_TOKEN_RBRACE,
    SHELL_TOKEN_IF,
    SHELL_TOKEN_THEN,
    SHELL_TOKEN_ELIF,
    SHELL_TOKEN_ELSE,
    SHELL_TOKEN_FI,
    SHELL_TOKEN_FOR,
    SHELL_TOKEN_WHILE,
    SHELL_TOKEN_UNTIL,
    SHELL_TOKEN_DO,
    SHELL_TOKEN_DONE,
    SHELL_TOKEN_IN,
    SHELL_TOKEN_CASE,
    SHELL_TOKEN_ESAC,
    SHELL_TOKEN_DSEMI,
    SHELL_TOKEN_LT,
    SHELL_TOKEN_GT,
    SHELL_TOKEN_GT_GT,
    SHELL_TOKEN_LT_LT,
    SHELL_TOKEN_LT_GT,
    SHELL_TOKEN_GT_AND,
    SHELL_TOKEN_LT_AND,
    SHELL_TOKEN_CLOBBER,
    SHELL_TOKEN_COMMENT,
    SHELL_TOKEN_EOF,
    SHELL_TOKEN_ERROR
} ShellTokenType;

typedef struct {
    ShellTokenType type;
    char *lexeme;
    size_t length;
    int line;
    int column;
    bool single_quoted;
    bool double_quoted;
    bool contains_parameter_expansion;
} ShellToken;

typedef struct {
    const char *src;
    size_t length;
    size_t pos;
    int line;
    int column;
    bool at_line_start;
} ShellLexer;

void shellInitLexer(ShellLexer *lexer, const char *source);
void shellFreeToken(ShellToken *token);
ShellToken shellNextToken(ShellLexer *lexer);
const char *shellTokenTypeName(ShellTokenType type);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_LEXER_H */
