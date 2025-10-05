#ifndef SHELL_PARSER_H
#define SHELL_PARSER_H

#include "shell/ast.h"
#include "shell/lexer.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ShellPendingHereDocArray ShellPendingHereDocArray;

typedef struct {
    ShellLexer lexer;
    ShellToken current;
    ShellToken previous;
    bool had_error;
    bool panic_mode;
    unsigned int next_rule_mask;
    ShellPendingHereDocArray *pending_here_docs;
} ShellParser;

ShellProgram *shellParseString(const char *source, ShellParser *parser);
void shellParserFree(ShellParser *parser);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_PARSER_H */
