#ifndef SHELL_LEXER_H
#define SHELL_LEXER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SHELL_TOKEN_WORD,
    SHELL_TOKEN_NAME,
    SHELL_TOKEN_ASSIGNMENT_WORD,
    SHELL_TOKEN_PARAMETER,
    SHELL_TOKEN_IO_NUMBER,
    SHELL_TOKEN_NEWLINE,
    SHELL_TOKEN_SEMICOLON,
    SHELL_TOKEN_AMPERSAND,
    SHELL_TOKEN_BANG,
    SHELL_TOKEN_PIPE,
    SHELL_TOKEN_PIPE_AMP,
    SHELL_TOKEN_AND_AND,
    SHELL_TOKEN_OR_OR,
    SHELL_TOKEN_LPAREN,
    SHELL_TOKEN_RPAREN,
    SHELL_TOKEN_LBRACE,
    SHELL_TOKEN_RBRACE,
    SHELL_TOKEN_FUNCTION,
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
    SHELL_TOKEN_DGREAT,
    SHELL_TOKEN_DLESS,
    SHELL_TOKEN_DLESSDASH,
    SHELL_TOKEN_LESSGREAT,
    SHELL_TOKEN_GREATAND,
    SHELL_TOKEN_LESSAND,
    SHELL_TOKEN_CLOBBER,
    SHELL_TOKEN_COMMENT,
    SHELL_TOKEN_EOF,
    SHELL_TOKEN_ERROR,
    SHELL_TOKEN_ASSIGNMENT = SHELL_TOKEN_ASSIGNMENT_WORD,
    SHELL_TOKEN_GT_GT = SHELL_TOKEN_DGREAT,
    SHELL_TOKEN_LT_LT = SHELL_TOKEN_DLESS,
    SHELL_TOKEN_LT_GT = SHELL_TOKEN_LESSGREAT,
    SHELL_TOKEN_GT_AND = SHELL_TOKEN_GREATAND,
    SHELL_TOKEN_LT_AND = SHELL_TOKEN_LESSAND
} ShellTokenType;

typedef enum {
    SHELL_LEXER_RULE_1 = 1u << 0,
    SHELL_LEXER_RULE_2 = 1u << 1,
    SHELL_LEXER_RULE_3 = 1u << 2,
    SHELL_LEXER_RULE_4 = 1u << 3,
    SHELL_LEXER_RULE_5 = 1u << 4,
    SHELL_LEXER_RULE_6 = 1u << 5,
    SHELL_LEXER_RULE_7 = 1u << 6,
    SHELL_LEXER_RULE_8 = 1u << 7,
    SHELL_LEXER_RULE_9 = 1u << 8
} ShellLexerRule;

typedef struct {
    ShellTokenType type;
    ShellTokenType base_type;
    ShellTokenType reserved_type;
    char *lexeme;
    size_t length;
    int line;
    int column;
    bool single_quoted;
    bool double_quoted;
    bool contains_parameter_expansion;
    bool contains_command_substitution;
    bool contains_arithmetic_expansion;
    bool reserved_candidate;
    bool assignment_candidate;
    bool name_candidate;
    bool command_starts;
    unsigned int rule_mask;
} ShellToken;

typedef struct {
    const char *src;
    size_t length;
    size_t pos;
    int line;
    int column;
    bool at_line_start;
    unsigned int rule_mask;
} ShellLexer;

void shellInitLexer(ShellLexer *lexer, const char *source);
void shellFreeToken(ShellToken *token);
ShellToken shellNextToken(ShellLexer *lexer);
const char *shellTokenTypeName(ShellTokenType type);
void shellLexerSetRuleMask(ShellLexer *lexer, unsigned int mask);
unsigned int shellLexerGetRuleMask(const ShellLexer *lexer);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_LEXER_H */
