#include "shell/parser.h"
#include "core/utils.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void shellParserAdvance(ShellParser *parser);
static bool shellParserCheck(const ShellParser *parser, ShellTokenType type);
static bool shellParserMatch(ShellParser *parser, ShellTokenType type);
static void shellParserConsume(ShellParser *parser, ShellTokenType type, const char *message);
static void shellParserSynchronize(ShellParser *parser);
static ShellCommand *parseCommand(ShellParser *parser);
static ShellCommand *parseAndOr(ShellParser *parser);
static ShellPipeline *parsePipeline(ShellParser *parser);
static ShellCommand *parsePrimary(ShellParser *parser);
static ShellCommand *parseSimpleCommand(ShellParser *parser);
static ShellCommand *parseIfCommand(ShellParser *parser);
static ShellCommand *parseLoopCommand(ShellParser *parser, bool is_until);
static ShellCommand *parseForCommand(ShellParser *parser);
static ShellCommand *parseCaseCommand(ShellParser *parser);
static ShellCommand *parseFunctionCommand(ShellParser *parser);
static ShellProgram *parseBlockUntil(ShellParser *parser, ShellTokenType terminator1,
                                     ShellTokenType terminator2, ShellTokenType terminator3);
static ShellProgram *parseBraceBody(ShellParser *parser);
static ShellWord *parseWordToken(ShellParser *parser, const char *context);
static void populateWordExpansions(ShellWord *word);
static bool parseDollarCommandSubstitution(const char *text, size_t start, size_t *out_span, char **out_command);
static bool parseBacktickCommandSubstitution(const char *text, size_t start, size_t *out_span, char **out_command);
static char *normalizeDollarCommand(const char *command, size_t len);
static char *normalizeBacktickCommand(const char *command, size_t len);
static void parserErrorAt(ShellParser *parser, const ShellToken *token, const char *message);

ShellProgram *shellParseString(const char *source, ShellParser *parser) {
    if (!parser) {
        return NULL;
    }
    memset(parser, 0, sizeof(*parser));
    shellInitLexer(&parser->lexer, source);
    parser->current.lexeme = NULL;
    parser->previous.lexeme = NULL;
    shellParserAdvance(parser);

    ShellProgram *program = shellCreateProgram();
    if (!program) {
        return NULL;
    }

    while (!parser->had_error && parser->current.type != SHELL_TOKEN_EOF) {
        if (parser->current.type == SHELL_TOKEN_NEWLINE) {
            shellParserAdvance(parser);
            continue;
        }
        ShellCommand *command = parseCommand(parser);
        if (command) {
            shellProgramAddCommand(program, command);
        }
        if (parser->current.type == SHELL_TOKEN_SEMICOLON || parser->current.type == SHELL_TOKEN_NEWLINE) {
            shellParserAdvance(parser);
            while (parser->current.type == SHELL_TOKEN_NEWLINE) {
                shellParserAdvance(parser);
            }
        } else if (parser->current.type != SHELL_TOKEN_EOF) {
            // Allow implicit separators before EOF or reserved words like fi/done
            if (parser->current.type == SHELL_TOKEN_FI || parser->current.type == SHELL_TOKEN_DONE ||
                parser->current.type == SHELL_TOKEN_ESAC || parser->current.type == SHELL_TOKEN_ELSE ||
                parser->current.type == SHELL_TOKEN_ELIF) {
                continue;
            }
            parserErrorAt(parser, &parser->current, "Expected command separator");
            shellParserSynchronize(parser);
        }
    }

    return program;
}

void shellParserFree(ShellParser *parser) {
    if (!parser) {
        return;
    }
    shellFreeToken(&parser->current);
    shellFreeToken(&parser->previous);
}

static void shellParserAdvance(ShellParser *parser) {
    shellFreeToken(&parser->previous);
    parser->previous = parser->current;
    // TODO: Incorporate lexer rule-mask metadata and reserved-word downgrades when
    // the parser begins consuming context-sensitive productions from Rules 1-9.
    parser->current = shellNextToken(&parser->lexer);
}

static bool shellParserCheck(const ShellParser *parser, ShellTokenType type) {
    return parser->current.type == type;
}

static bool shellParserMatch(ShellParser *parser, ShellTokenType type) {
    if (!shellParserCheck(parser, type)) {
        return false;
    }
    shellParserAdvance(parser);
    return true;
}

static void parserErrorAt(ShellParser *parser, const ShellToken *token, const char *message) {
    if (!parser || parser->had_error) {
        return;
    }
    int line = token ? token->line : parser->lexer.line;
    int column = token ? token->column : parser->lexer.column;
    fprintf(stderr, "shell parse error at %d:%d: %s\n", line, column, message ? message : "error");
    parser->had_error = true;
    parser->panic_mode = true;
}

static void shellParserConsume(ShellParser *parser, ShellTokenType type, const char *message) {
    if (parser->current.type == type) {
        shellParserAdvance(parser);
        return;
    }
    parserErrorAt(parser, &parser->current, message);
}

static void shellParserSynchronize(ShellParser *parser) {
    while (parser->current.type != SHELL_TOKEN_EOF) {
        if (parser->previous.type == SHELL_TOKEN_SEMICOLON || parser->previous.type == SHELL_TOKEN_NEWLINE) {
            parser->panic_mode = false;
            return;
        }
        switch (parser->current.type) {
            case SHELL_TOKEN_IF:
            case SHELL_TOKEN_THEN:
            case SHELL_TOKEN_ELIF:
            case SHELL_TOKEN_ELSE:
            case SHELL_TOKEN_FI:
            case SHELL_TOKEN_FOR:
            case SHELL_TOKEN_WHILE:
            case SHELL_TOKEN_UNTIL:
            case SHELL_TOKEN_DO:
            case SHELL_TOKEN_DONE:
                parser->panic_mode = false;
                return;
            default:
                break;
        }
        shellParserAdvance(parser);
    }
}

static ShellCommand *parseCommand(ShellParser *parser) {
    ShellCommand *command = parseAndOr(parser);
    if (!command) {
        return NULL;
    }
    if (shellParserMatch(parser, SHELL_TOKEN_AMPERSAND)) {
        command->exec.runs_in_background = true;
        command->exec.is_async_parent = true;
    }
    return command;
}

static ShellCommand *parseAndOr(ShellParser *parser) {
    ShellPipeline *first = parsePipeline(parser);
    if (!first) {
        return NULL;
    }

    ShellLogicalList *logical = NULL;
    while (true) {
        ShellLogicalConnector connector;
        if (shellParserMatch(parser, SHELL_TOKEN_AND_AND)) {
            connector = SHELL_LOGICAL_AND;
        } else if (shellParserMatch(parser, SHELL_TOKEN_OR_OR)) {
            connector = SHELL_LOGICAL_OR;
        } else {
            break;
        }
        ShellPipeline *next = parsePipeline(parser);
        if (!next) {
            break;
        }
        if (!logical) {
            logical = shellCreateLogicalList();
            shellLogicalListAdd(logical, first, SHELL_LOGICAL_AND);
        }
        shellLogicalListAdd(logical, next, connector);
    }

    if (logical) {
        ShellCommand *cmd = shellCreateLogicalCommand(logical);
        if (cmd) {
            cmd->line = first && first->command_count > 0 && first->commands[0]
                            ? first->commands[0]->line
                            : parser->current.line;
            cmd->column = first && first->command_count > 0 && first->commands[0]
                              ? first->commands[0]->column
                              : parser->current.column;
        }
        return cmd;
    }

    ShellCommand *cmd = shellCreatePipelineCommand(first);
    if (cmd) {
        if (first && first->command_count > 0 && first->commands[0]) {
            cmd->line = first->commands[0]->line;
            cmd->column = first->commands[0]->column;
        } else {
            cmd->line = parser->current.line;
            cmd->column = parser->current.column;
        }
        if (first && first->command_count > 0) {
            for (size_t i = 0; i < first->command_count; ++i) {
                first->commands[i]->exec.pipeline_index = (int)i;
                first->commands[i]->exec.is_pipeline_head = (i == 0);
                first->commands[i]->exec.is_pipeline_tail = (i + 1 == first->command_count);
            }
        }
    }
    return cmd;
}

static ShellPipeline *parsePipeline(ShellParser *parser) {
    bool negate = false;
    while (shellParserMatch(parser, SHELL_TOKEN_BANG)) {
        negate = !negate;
        while (parser->current.type == SHELL_TOKEN_NEWLINE) {
            shellParserAdvance(parser);
        }
    }

    ShellPipeline *pipeline = shellCreatePipeline();
    if (!pipeline) {
        return NULL;
    }

    pipeline->negated = negate;

    ShellCommand *command = parsePrimary(parser);
    if (!command) {
        return pipeline;
    }
    shellPipelineAddCommand(pipeline, command);

    while (true) {
        if (shellParserMatch(parser, SHELL_TOKEN_PIPE)) {
            ShellCommand *next = parsePrimary(parser);
            if (!next) {
                break;
            }
            shellPipelineAddCommand(pipeline, next);
            continue;
        }
        if (shellParserMatch(parser, SHELL_TOKEN_PIPE_AMP)) {
            pipeline->negated = true;
            ShellCommand *next = parsePrimary(parser);
            if (next) {
                shellPipelineAddCommand(pipeline, next);
            }
            break;
        }
        break;
    }

    for (size_t i = 0; i < pipeline->command_count; ++i) {
        pipeline->commands[i]->exec.pipeline_index = (int)i;
        pipeline->commands[i]->exec.is_pipeline_head = (i == 0);
        pipeline->commands[i]->exec.is_pipeline_tail = (i + 1 == pipeline->command_count);
    }
    return pipeline;
}

static ShellProgram *parseSubshellBody(ShellParser *parser) {
    ShellProgram *body = shellCreateProgram();
    while (!parser->had_error && parser->current.type != SHELL_TOKEN_RPAREN && parser->current.type != SHELL_TOKEN_EOF) {
        if (parser->current.type == SHELL_TOKEN_NEWLINE) {
            shellParserAdvance(parser);
            continue;
        }
        ShellCommand *command = parseCommand(parser);
        if (command) {
            shellProgramAddCommand(body, command);
        }
        if (parser->current.type == SHELL_TOKEN_SEMICOLON || parser->current.type == SHELL_TOKEN_NEWLINE) {
            shellParserAdvance(parser);
        }
    }
    shellParserConsume(parser, SHELL_TOKEN_RPAREN, "Expected ')' to close subshell");
    return body;
}

static ShellCommand *parsePrimary(ShellParser *parser) {
    switch (parser->current.type) {
        case SHELL_TOKEN_LPAREN: {
            int line = parser->current.line;
            int column = parser->current.column;
            shellParserAdvance(parser);
            ShellProgram *body = parseSubshellBody(parser);
            ShellCommand *cmd = shellCreateSubshellCommand(body);
            if (cmd) {
                cmd->line = line;
                cmd->column = column;
            }
            return cmd;
        }
        case SHELL_TOKEN_FUNCTION:
            return parseFunctionCommand(parser);
        case SHELL_TOKEN_IF:
            return parseIfCommand(parser);
        case SHELL_TOKEN_WHILE:
            return parseLoopCommand(parser, false);
        case SHELL_TOKEN_UNTIL:
            return parseLoopCommand(parser, true);
        case SHELL_TOKEN_FOR:
            return parseForCommand(parser);
        case SHELL_TOKEN_CASE:
            return parseCaseCommand(parser);
        default:
            return parseSimpleCommand(parser);
    }
}

static ShellCommand *parseSimpleCommand(ShellParser *parser) {
    ShellCommand *command = shellCreateSimpleCommand();
    if (!command) {
        return NULL;
    }
    command->line = parser->current.line;
    command->column = parser->current.column;

    bool saw_word = false;
    while (!parser->had_error) {
        if (!saw_word && parser->current.type == SHELL_TOKEN_WORD) {
            const char *lexeme = parser->current.lexeme ? parser->current.lexeme : "";
            bool single_quoted = parser->current.single_quoted;
            bool double_quoted = parser->current.double_quoted;
            bool has_param = parser->current.contains_parameter_expansion;
            bool has_arith = parser->current.contains_arithmetic_expansion;
            int word_line = parser->current.line;
            int word_column = parser->current.column;
            shellParserAdvance(parser);
            if (parser->current.type == SHELL_TOKEN_LPAREN) {
                char *name_copy = parser->previous.lexeme ? strdup(parser->previous.lexeme) : NULL;
                shellParserAdvance(parser);
                shellParserConsume(parser, SHELL_TOKEN_RPAREN, "Expected ')' after function name");
                while (parser->current.type == SHELL_TOKEN_NEWLINE) {
                    shellParserAdvance(parser);
                }
                shellParserConsume(parser, SHELL_TOKEN_LBRACE, "Expected '{' to start function body");
                ShellProgram *body = parseBraceBody(parser);
                shellParserConsume(parser, SHELL_TOKEN_RBRACE, "Expected '}' to close function");
                const char *function_name = name_copy ? name_copy : "";
                ShellFunction *function = shellCreateFunction(function_name, "", body);
                ShellCommand *func_cmd = shellCreateFunctionCommand(function);
                if (func_cmd) {
                    func_cmd->line = word_line;
                    func_cmd->column = word_column;
                }
                free(name_copy);
                return func_cmd;
            }
            ShellWord *word = shellCreateWord(lexeme, single_quoted, double_quoted,
                                             has_param, has_arith, word_line, word_column);
            populateWordExpansions(word);
            shellCommandAddWord(command, word);
            saw_word = true;
            continue;
        }
        switch (parser->current.type) {
            case SHELL_TOKEN_WORD:
            case SHELL_TOKEN_ASSIGNMENT:
            case SHELL_TOKEN_PARAMETER: {
                ShellTokenType type = parser->current.type;
                const char *lexeme = parser->current.lexeme ? parser->current.lexeme : "";
                bool single_quoted = parser->current.single_quoted;
                bool double_quoted = parser->current.double_quoted;
                bool has_param = parser->current.contains_parameter_expansion;
                bool has_arith = parser->current.contains_arithmetic_expansion;
                int line = parser->current.line;
                int column = parser->current.column;
                shellParserAdvance(parser);
                ShellWord *word = shellCreateWord(lexeme, single_quoted, double_quoted,
                                                  has_param, has_arith, line, column);
                if (type == SHELL_TOKEN_ASSIGNMENT && word) {
                    word->is_assignment = true;
                }
                if (type == SHELL_TOKEN_PARAMETER && lexeme && lexeme[0] == '$' && lexeme[1]) {
                    shellWordAddExpansion(word, lexeme + 1);
                }
                populateWordExpansions(word);
                shellCommandAddWord(command, word);
                saw_word = true;
                continue;
            }
            case SHELL_TOKEN_IO_NUMBER: {
                int line = parser->current.line;
                int column = parser->current.column;
                char *io_number = parser->current.lexeme ? strdup(parser->current.lexeme) : NULL;
                shellParserAdvance(parser);
                ShellTokenType redir_type = parser->current.type;
                shellParserAdvance(parser);
                ShellRedirectionType type;
                switch (redir_type) {
                    case SHELL_TOKEN_LT: type = SHELL_REDIRECT_INPUT; break;
                    case SHELL_TOKEN_GT: type = SHELL_REDIRECT_OUTPUT; break;
                    case SHELL_TOKEN_GT_GT: type = SHELL_REDIRECT_APPEND; break;
                    case SHELL_TOKEN_LT_LT:
                    case SHELL_TOKEN_DLESSDASH: type = SHELL_REDIRECT_HEREDOC; break;
                    case SHELL_TOKEN_LT_AND: type = SHELL_REDIRECT_DUP_INPUT; break;
                    case SHELL_TOKEN_GT_AND: type = SHELL_REDIRECT_DUP_OUTPUT; break;
                    case SHELL_TOKEN_CLOBBER: type = SHELL_REDIRECT_CLOBBER; break;
                    default:
                        parserErrorAt(parser, &parser->current, "Expected redirection operator");
                        free(io_number);
                        return command;
                }
                ShellWord *target = NULL;
                if (parser->current.type == SHELL_TOKEN_WORD || parser->current.type == SHELL_TOKEN_ASSIGNMENT ||
                    parser->current.type == SHELL_TOKEN_PARAMETER) {
                    const char *lexeme = parser->current.lexeme ? parser->current.lexeme : "";
                    bool single_quoted = parser->current.single_quoted;
                    bool double_quoted = parser->current.double_quoted;
                    bool has_param = parser->current.contains_parameter_expansion;
                    bool has_arith = parser->current.contains_arithmetic_expansion;
                    int target_line = parser->current.line;
                    int target_column = parser->current.column;
                    shellParserAdvance(parser);
                    target = shellCreateWord(lexeme, single_quoted, double_quoted,
                                             has_param, has_arith, target_line, target_column);
                    populateWordExpansions(target);
                } else {
                    parserErrorAt(parser, &parser->current, "Expected redirection target");
                }
                ShellRedirection *redir = shellCreateRedirection(type, io_number, target, line, column);
                shellCommandAddRedirection(command, redir);
                free(io_number);
                continue;
            }
            case SHELL_TOKEN_LT:
            case SHELL_TOKEN_GT:
            case SHELL_TOKEN_GT_GT:
            case SHELL_TOKEN_LT_LT:
            case SHELL_TOKEN_LT_AND:
            case SHELL_TOKEN_GT_AND:
            case SHELL_TOKEN_CLOBBER: {
                ShellTokenType redirTokType = parser->current.type;
                int redir_line = parser->current.line;
                int redir_column = parser->current.column;
                shellParserAdvance(parser);
                ShellRedirectionType type;
                switch (redirTokType) {
                    case SHELL_TOKEN_LT: type = SHELL_REDIRECT_INPUT; break;
                    case SHELL_TOKEN_GT: type = SHELL_REDIRECT_OUTPUT; break;
                    case SHELL_TOKEN_GT_GT: type = SHELL_REDIRECT_APPEND; break;
                    case SHELL_TOKEN_LT_LT:
                    case SHELL_TOKEN_DLESSDASH: type = SHELL_REDIRECT_HEREDOC; break;
                    case SHELL_TOKEN_LT_AND: type = SHELL_REDIRECT_DUP_INPUT; break;
                    case SHELL_TOKEN_GT_AND: type = SHELL_REDIRECT_DUP_OUTPUT; break;
                    case SHELL_TOKEN_CLOBBER: type = SHELL_REDIRECT_CLOBBER; break;
                    default: type = SHELL_REDIRECT_OUTPUT; break;
                }
                ShellWord *target = NULL;
                if (parser->current.type == SHELL_TOKEN_WORD || parser->current.type == SHELL_TOKEN_ASSIGNMENT ||
                    parser->current.type == SHELL_TOKEN_PARAMETER) {
                    const char *lexeme = parser->current.lexeme ? parser->current.lexeme : "";
                    bool single_quoted = parser->current.single_quoted;
                    bool double_quoted = parser->current.double_quoted;
                    bool has_param = parser->current.contains_parameter_expansion;
                    bool has_arith = parser->current.contains_arithmetic_expansion;
                    int target_line = parser->current.line;
                    int target_column = parser->current.column;
                    shellParserAdvance(parser);
                    target = shellCreateWord(lexeme, single_quoted, double_quoted,
                                             has_param, has_arith, target_line, target_column);
                    populateWordExpansions(target);
                } else {
                    parserErrorAt(parser, &parser->current, "Expected redirection target");
                }
                ShellRedirection *redir = shellCreateRedirection(type, NULL, target, redir_line, redir_column);
                shellCommandAddRedirection(command, redir);
                continue;
            }
            default:
                break;
        }
        break;
    }

    if (!saw_word && command->data.simple.words.count == 0 && command->data.simple.redirections.count == 0) {
        parserErrorAt(parser, &parser->current, "Expected command word");
    }
    return command;
}

static ShellProgram *parseBlockUntil(ShellParser *parser, ShellTokenType terminator1,
                                     ShellTokenType terminator2, ShellTokenType terminator3) {
    ShellProgram *block = shellCreateProgram();
    while (!parser->had_error && parser->current.type != terminator1 &&
           parser->current.type != terminator2 && parser->current.type != terminator3 &&
           parser->current.type != SHELL_TOKEN_EOF) {
        if (parser->current.type == SHELL_TOKEN_NEWLINE) {
            shellParserAdvance(parser);
            continue;
        }
        ShellCommand *command = parseCommand(parser);
        if (command) {
            shellProgramAddCommand(block, command);
        }
        if (parser->current.type == SHELL_TOKEN_SEMICOLON || parser->current.type == SHELL_TOKEN_NEWLINE) {
            shellParserAdvance(parser);
        }
    }
    return block;
}

static ShellProgram *parseBraceBody(ShellParser *parser) {
    ShellProgram *body = shellCreateProgram();
    while (!parser->had_error && parser->current.type != SHELL_TOKEN_RBRACE &&
           parser->current.type != SHELL_TOKEN_EOF) {
        if (parser->current.type == SHELL_TOKEN_NEWLINE) {
            shellParserAdvance(parser);
            continue;
        }
        ShellCommand *command = parseCommand(parser);
        if (command) {
            shellProgramAddCommand(body, command);
        }
        if (parser->current.type == SHELL_TOKEN_SEMICOLON || parser->current.type == SHELL_TOKEN_NEWLINE) {
            shellParserAdvance(parser);
        }
    }
    return body;
}

static ShellCommand *parseFunctionCommand(ShellParser *parser) {
    int line = parser->current.line;
    int column = parser->current.column;
    shellParserAdvance(parser); // consume 'function'
    if (parser->current.type != SHELL_TOKEN_WORD) {
        parserErrorAt(parser, &parser->current, "Expected function name after 'function'");
        return NULL;
    }
    char *name_copy = parser->current.lexeme ? strdup(parser->current.lexeme) : NULL;
    int name_line = parser->current.line;
    int name_column = parser->current.column;
    shellParserAdvance(parser);
    const char *param_meta = NULL;
    if (parser->current.type == SHELL_TOKEN_LPAREN) {
        shellParserAdvance(parser);
        shellParserConsume(parser, SHELL_TOKEN_RPAREN, "Expected ')' after function name");
        param_meta = "";
        while (parser->current.type == SHELL_TOKEN_NEWLINE) {
            shellParserAdvance(parser);
        }
    }
    while (parser->current.type == SHELL_TOKEN_NEWLINE) {
        shellParserAdvance(parser);
    }
    shellParserConsume(parser, SHELL_TOKEN_LBRACE, "Expected '{' to start function body");
    ShellProgram *body = parseBraceBody(parser);
    shellParserConsume(parser, SHELL_TOKEN_RBRACE, "Expected '}' to close function");
    const char *function_name = name_copy ? name_copy : "";
    ShellFunction *function = shellCreateFunction(function_name, param_meta, body);
    ShellCommand *cmd = shellCreateFunctionCommand(function);
    if (cmd) {
        cmd->line = line;
        cmd->column = column;
    }
    free(name_copy);
    (void)name_line;
    (void)name_column;
    return cmd;
}

static ShellCommand *parseIfTail(ShellParser *parser);

static ShellCommand *parseIfCommand(ShellParser *parser) {
    int line = parser->current.line;
    int column = parser->current.column;
    shellParserAdvance(parser); // consume 'if'
    ShellPipeline *condition = parsePipeline(parser);
    while (parser->current.type == SHELL_TOKEN_NEWLINE) {
        shellParserAdvance(parser);
    }
    if (parser->current.type == SHELL_TOKEN_SEMICOLON) {
        shellParserAdvance(parser);
    }
    shellParserConsume(parser, SHELL_TOKEN_THEN, "Expected 'then' after if condition");
    ShellProgram *then_block = parseBlockUntil(parser, SHELL_TOKEN_ELIF, SHELL_TOKEN_ELSE, SHELL_TOKEN_FI);
    ShellProgram *else_block = NULL;

    if (shellParserMatch(parser, SHELL_TOKEN_ELIF)) {
        ShellCommand *elif_cmd = parseIfTail(parser);
        else_block = shellCreateProgram();
        shellProgramAddCommand(else_block, elif_cmd);
    } else if (shellParserMatch(parser, SHELL_TOKEN_ELSE)) {
        else_block = parseBlockUntil(parser, SHELL_TOKEN_FI, SHELL_TOKEN_EOF, SHELL_TOKEN_EOF);
        shellParserConsume(parser, SHELL_TOKEN_FI, "Expected 'fi' to close if");
        goto done;
    } else {
        shellParserConsume(parser, SHELL_TOKEN_FI, "Expected 'fi' to close if");
    }

done:
    ShellConditional *conditional = shellCreateConditional(condition, then_block, else_block);
    ShellCommand *cmd = shellCreateConditionalCommand(conditional);
    if (cmd) {
        cmd->line = line;
        cmd->column = column;
    }
    return cmd;
}

static ShellCommand *parseIfTail(ShellParser *parser) {
    int line = parser->previous.line;
    int column = parser->previous.column;
    // parser currently after 'elif'
    ShellPipeline *condition = parsePipeline(parser);
    while (parser->current.type == SHELL_TOKEN_NEWLINE) {
        shellParserAdvance(parser);
    }
    if (parser->current.type == SHELL_TOKEN_SEMICOLON) {
        shellParserAdvance(parser);
    }
    shellParserConsume(parser, SHELL_TOKEN_THEN, "Expected 'then' after elif condition");
    ShellProgram *then_block = parseBlockUntil(parser, SHELL_TOKEN_ELIF, SHELL_TOKEN_ELSE, SHELL_TOKEN_FI);
    ShellProgram *else_block = NULL;
    if (shellParserMatch(parser, SHELL_TOKEN_ELIF)) {
        ShellCommand *elif_cmd = parseIfTail(parser);
        else_block = shellCreateProgram();
        shellProgramAddCommand(else_block, elif_cmd);
    } else if (shellParserMatch(parser, SHELL_TOKEN_ELSE)) {
        else_block = parseBlockUntil(parser, SHELL_TOKEN_FI, SHELL_TOKEN_EOF, SHELL_TOKEN_EOF);
        shellParserConsume(parser, SHELL_TOKEN_FI, "Expected 'fi' to close if");
    } else {
        shellParserConsume(parser, SHELL_TOKEN_FI, "Expected 'fi' to close if");
    }
    ShellConditional *conditional = shellCreateConditional(condition, then_block, else_block);
    ShellCommand *cmd = shellCreateConditionalCommand(conditional);
    if (cmd) {
        cmd->line = line;
        cmd->column = column;
    }
    return cmd;
}

static ShellCommand *parseLoopCommand(ShellParser *parser, bool is_until) {
    int line = parser->current.line;
    int column = parser->current.column;
    shellParserAdvance(parser); // consume keyword
    ShellPipeline *condition = parsePipeline(parser);
    while (parser->current.type == SHELL_TOKEN_NEWLINE) {
        shellParserAdvance(parser);
    }
    if (parser->current.type == SHELL_TOKEN_SEMICOLON) {
        shellParserAdvance(parser);
        while (parser->current.type == SHELL_TOKEN_NEWLINE) {
            shellParserAdvance(parser);
        }
    }
    shellParserConsume(parser, SHELL_TOKEN_DO, "Expected 'do' after loop condition");
    ShellProgram *body = parseBlockUntil(parser, SHELL_TOKEN_DONE, SHELL_TOKEN_EOF, SHELL_TOKEN_EOF);
    shellParserConsume(parser, SHELL_TOKEN_DONE, "Expected 'done' to close loop");
    ShellLoop *loop = shellCreateLoop(is_until, condition, body);
    ShellCommand *cmd = shellCreateLoopCommand(loop);
    if (cmd) {
        cmd->line = line;
        cmd->column = column;
    }
    return cmd;
}

static ShellCommand *parseCaseCommand(ShellParser *parser) {
    int line = parser->current.line;
    int column = parser->current.column;
    shellParserAdvance(parser);
    ShellWord *subject = parseWordToken(parser, "Expected subject after 'case'");
    if (!subject) {
        return NULL;
    }
    shellParserConsume(parser, SHELL_TOKEN_IN, "Expected 'in' after case subject");
    while (parser->current.type == SHELL_TOKEN_NEWLINE || parser->current.type == SHELL_TOKEN_SEMICOLON) {
        shellParserAdvance(parser);
    }
    ShellCase *case_stmt = shellCreateCase(subject);
    if (!case_stmt) {
        shellFreeWord(subject);
        return NULL;
    }
    while (!parser->had_error && parser->current.type != SHELL_TOKEN_ESAC && parser->current.type != SHELL_TOKEN_EOF) {
        if (parser->current.type == SHELL_TOKEN_NEWLINE || parser->current.type == SHELL_TOKEN_SEMICOLON) {
            shellParserAdvance(parser);
            continue;
        }
        if (shellParserMatch(parser, SHELL_TOKEN_LPAREN)) {
            while (parser->current.type == SHELL_TOKEN_NEWLINE) {
                shellParserAdvance(parser);
            }
        }
        if (parser->current.type == SHELL_TOKEN_ESAC || parser->current.type == SHELL_TOKEN_EOF) {
            break;
        }
        int clause_line = parser->current.line;
        int clause_column = parser->current.column;
        ShellCaseClause *clause = shellCreateCaseClause(clause_line, clause_column);
        if (!clause) {
            break;
        }
        while (parser->current.type == SHELL_TOKEN_WORD || parser->current.type == SHELL_TOKEN_ASSIGNMENT ||
               parser->current.type == SHELL_TOKEN_PARAMETER) {
            ShellWord *pattern = parseWordToken(parser, "Expected pattern in case clause");
            if (pattern) {
                shellCaseClauseAddPattern(clause, pattern);
            }
            if (!shellParserMatch(parser, SHELL_TOKEN_PIPE)) {
                break;
            }
            while (parser->current.type == SHELL_TOKEN_NEWLINE) {
                shellParserAdvance(parser);
            }
        }
        while (parser->current.type == SHELL_TOKEN_NEWLINE) {
            shellParserAdvance(parser);
        }
        shellParserConsume(parser, SHELL_TOKEN_RPAREN, "Expected ')' after case pattern");
        ShellProgram *body = parseBlockUntil(parser, SHELL_TOKEN_DSEMI, SHELL_TOKEN_ESAC, SHELL_TOKEN_EOF);
        shellCaseClauseSetBody(clause, body);
        shellCaseAddClause(case_stmt, clause);
        if (parser->current.type == SHELL_TOKEN_DSEMI) {
            shellParserAdvance(parser);
        }
        while (parser->current.type == SHELL_TOKEN_NEWLINE || parser->current.type == SHELL_TOKEN_SEMICOLON) {
            shellParserAdvance(parser);
        }
        if (parser->current.type == SHELL_TOKEN_ESAC) {
            break;
        }
    }
    shellParserConsume(parser, SHELL_TOKEN_ESAC, "Expected 'esac' to close case");
    ShellCommand *cmd = shellCreateCaseCommand(case_stmt);
    if (!cmd) {
        shellFreeCase(case_stmt);
        return NULL;
    }
    cmd->line = line;
    cmd->column = column;
    return cmd;
}

static ShellCommand *parseForCommand(ShellParser *parser) {
    int line = parser->current.line;
    int column = parser->current.column;
    shellParserAdvance(parser); // consume 'for'
    if (parser->current.type != SHELL_TOKEN_WORD && parser->current.type != SHELL_TOKEN_ASSIGNMENT) {
        parserErrorAt(parser, &parser->current, "Expected identifier after for");
    }
    const char *var_lexeme = parser->current.lexeme ? parser->current.lexeme : "";
    int var_line = parser->current.line;
    int var_column = parser->current.column;
    shellParserAdvance(parser);
    ShellCommand *command = shellCreateSimpleCommand();
    ShellWord *var_word = shellCreateWord(var_lexeme, false, false, false, false, var_line, var_column);
    shellCommandAddWord(command, var_word);

    ShellProgram *body = NULL;
    if (shellParserMatch(parser, SHELL_TOKEN_IN)) {
        while (parser->current.type == SHELL_TOKEN_WORD || parser->current.type == SHELL_TOKEN_ASSIGNMENT ||
               parser->current.type == SHELL_TOKEN_PARAMETER) {
            const char *lexeme = parser->current.lexeme ? parser->current.lexeme : "";
            bool single_quoted = parser->current.single_quoted;
            bool double_quoted = parser->current.double_quoted;
            bool has_param = parser->current.contains_parameter_expansion;
            bool has_arith = parser->current.contains_arithmetic_expansion;
            int word_line = parser->current.line;
            int word_column = parser->current.column;
            ShellTokenType token_type = parser->current.type;
            shellParserAdvance(parser);
            ShellWord *word = shellCreateWord(lexeme, single_quoted, double_quoted,
                                              has_param, has_arith, word_line, word_column);
            if (token_type == SHELL_TOKEN_ASSIGNMENT && word) {
                word->is_assignment = true;
            }
            populateWordExpansions(word);
            shellCommandAddWord(command, word);
            if (parser->current.type == SHELL_TOKEN_SEMICOLON || parser->current.type == SHELL_TOKEN_NEWLINE) {
                break;
            }
        }
    }

    if (parser->current.type == SHELL_TOKEN_SEMICOLON || parser->current.type == SHELL_TOKEN_NEWLINE) {
        shellParserAdvance(parser);
    }
    shellParserConsume(parser, SHELL_TOKEN_DO, "Expected 'do' after for list");
    body = parseBlockUntil(parser, SHELL_TOKEN_DONE, SHELL_TOKEN_EOF, SHELL_TOKEN_EOF);
    shellParserConsume(parser, SHELL_TOKEN_DONE, "Expected 'done' to close for loop");

    ShellPipeline *condition = shellCreatePipeline();
    if (condition) {
        shellPipelineAddCommand(condition, command);
    }
    ShellLoop *loop = shellCreateLoop(false, condition, body);
    ShellCommand *cmd = shellCreateLoopCommand(loop);
    if (cmd) {
        cmd->line = line;
        cmd->column = column;
    }
    return cmd;
}

static ShellWord *parseWordToken(ShellParser *parser, const char *context) {
    if (parser->current.type != SHELL_TOKEN_WORD &&
        parser->current.type != SHELL_TOKEN_ASSIGNMENT &&
        parser->current.type != SHELL_TOKEN_PARAMETER) {
        if (context) {
            parserErrorAt(parser, &parser->current, context);
        } else {
            parserErrorAt(parser, &parser->current, "Expected word");
        }
        return NULL;
    }
    ShellTokenType type = parser->current.type;
    const char *lexeme = parser->current.lexeme ? parser->current.lexeme : "";
    bool single_quoted = parser->current.single_quoted;
    bool double_quoted = parser->current.double_quoted;
    bool has_param = parser->current.contains_parameter_expansion;
    bool has_arith = parser->current.contains_arithmetic_expansion;
    bool has_command = parser->current.contains_command_substitution;
    int line = parser->current.line;
    int column = parser->current.column;
    shellParserAdvance(parser);
    ShellWord *word = shellCreateWord(lexeme, single_quoted, double_quoted, has_param, has_arith, line, column);
    if (word && has_command) {
        word->has_command_substitution = true;
    }
    if (type == SHELL_TOKEN_ASSIGNMENT && word) {
        word->is_assignment = true;
    }
    if (type == SHELL_TOKEN_PARAMETER && lexeme && lexeme[0] == '$' && lexeme[1]) {
        if (lexeme[1] != '(') {
            shellWordAddExpansion(word, lexeme + 1);
        }
    }
    populateWordExpansions(word);
    return word;
}

static bool parseDollarCommandSubstitution(const char *text, size_t start, size_t *out_span, char **out_command) {
    if (out_span) *out_span = 0;
    if (out_command) *out_command = NULL;
    if (!text || text[start] != '$' || text[start + 1] != '(') {
        return false;
    }
    size_t i = start + 2;
    int depth = 1;
    bool in_single = false;
    bool in_double = false;
    while (text[i]) {
        char c = text[i];
        if (c == '\\') {
            if (text[i + 1]) {
                i += 2;
                continue;
            }
            break;
        }
        if (!in_double && c == 39) {
            in_single = !in_single;
            i++;
            continue;
        }
        if (!in_single && c == '"') {
            in_double = !in_double;
            i++;
            continue;
        }
        if (!in_single && !in_double) {
            if (c == '(') {
                depth++;
            } else if (c == ')') {
                depth--;
                if (depth == 0) {
                    size_t end = i;
                    size_t span = end - start + 1;
                    size_t command_start = start + 2;
                    size_t command_len = end - command_start;
                    char *raw = (char *)malloc(command_len + 1);
                    if (!raw) {
                        return false;
                    }
                    memcpy(raw, text + command_start, command_len);
                    raw[command_len] = '\0';
                    char *normalized = normalizeDollarCommand(raw, command_len);
                    free(raw);
                    if (!normalized) {
                        return false;
                    }
                    if (out_span) *out_span = span;
                    if (out_command) {
                        *out_command = normalized;
                    } else {
                        free(normalized);
                    }
                    return true;
                }
            }
        }
        i++;
    }
    return false;
}

static bool parseBacktickCommandSubstitution(const char *text, size_t start, size_t *out_span, char **out_command) {
    if (out_span) *out_span = 0;
    if (out_command) *out_command = NULL;
    if (!text || text[start] != '`') {
        return false;
    }
    size_t i = start + 1;
    while (text[i]) {
        char c = text[i];
        if (c == '\\') {
            if (text[i + 1]) {
                i += 2;
                continue;
            }
            break;
        }
        if (c == '`') {
            size_t span = (i - start) + 1;
            size_t command_start = start + 1;
            size_t command_len = i - command_start;
            char *raw = (char *)malloc(command_len + 1);
            if (!raw) {
                return false;
            }
            memcpy(raw, text + command_start, command_len);
            raw[command_len] = '\0';
            char *normalized = normalizeBacktickCommand(raw, command_len);
            free(raw);
            if (!normalized) {
                return false;
            }
            if (out_span) *out_span = span;
            if (out_command) {
                *out_command = normalized;
            } else {
                free(normalized);
            }
            return true;
        }
        i++;
    }
    return false;
}

static char *normalizeDollarCommand(const char *command, size_t len) {
    if (!command) {
        return NULL;
    }
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    size_t j = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = command[i];
        if (c == '\\' && i + 1 < len) {
            char next = command[i + 1];
            if (next == '\n') {
                i++;
                continue;
            }
        }
        out[j++] = c;
    }
    out[j] = '\0';
    char *shrunk = (char *)realloc(out, j + 1);
    return shrunk ? shrunk : out;
}

static char *normalizeBacktickCommand(const char *command, size_t len) {
    if (!command) {
        return NULL;
    }
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    size_t j = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = command[i];
        if (c == '\\' && i + 1 < len) {
            char next = command[i + 1];
            if (next == '\n') {
                i++;
                continue;
            }
            if (next == '\\' || next == '`' || next == '$') {
                out[j++] = next;
                i++;
                continue;
            }
        }
        out[j++] = c;
    }
    out[j] = '\0';
    char *shrunk = (char *)realloc(out, j + 1);
    return shrunk ? shrunk : out;
}

static void populateWordExpansions(ShellWord *word) {
    if (!word || !word->text) {
        return;
    }
    const char *text = word->text;
    size_t len = strlen(text);
    size_t i = 0;
    while (i < len) {
        char c = text[i];
        if (c == '$') {
            size_t span = 0;
            char *command = NULL;
            if (i + 1 < len && text[i + 1] == '(') {
                if (i + 2 < len && text[i + 2] == '(') {
                    /* Treat $(( as arithmetic expansion; do not parse as command substitution. */
                } else if (parseDollarCommandSubstitution(text, i, &span, &command)) {
                    if (command) {
                        shellWordAddCommandSubstitution(word, SHELL_COMMAND_SUBSTITUTION_DOLLAR, command, span);
                        free(command);
                    }
                    i += span;
                    continue;
                }
            }
            size_t j = i + 1;
            if (j < len && text[j] == '{') {
                j++;
                const char *start = text + j;
                while (j < len && text[j] && text[j] != '}' &&
                       (isalnum((unsigned char)text[j]) || text[j] == '_' || text[j] == '#')) {
                    j++;
                }
                size_t name_len = (size_t)((text + j) - start);
                if (name_len > 0) {
                    char *name = (char *)malloc(name_len + 1);
                    if (name) {
                        memcpy(name, start, name_len);
                        name[name_len] = '\0';
                        shellWordAddExpansion(word, name);
                        free(name);
                    }
                }
                while (j < len && text[j] && text[j] != '}') {
                    j++;
                }
                if (j < len && text[j] == '}') {
                    j++;
                }
                i = j;
                continue;
            } else {
                const char *start = text + j;
                while (j < len && (isalnum((unsigned char)text[j]) || text[j] == '_' || text[j] == '#')) {
                    j++;
                }
                size_t name_len = (size_t)((text + j) - start);
                if (name_len > 0) {
                    char *name = (char *)malloc(name_len + 1);
                    if (name) {
                        memcpy(name, start, name_len);
                        name[name_len] = '\0';
                        shellWordAddExpansion(word, name);
                        free(name);
                    }
                }
                i = j;
                continue;
            }
        } else if (c == '`') {
            size_t span = 0;
            char *command = NULL;
            if (parseBacktickCommandSubstitution(text, i, &span, &command)) {
                if (command) {
                    shellWordAddCommandSubstitution(word, SHELL_COMMAND_SUBSTITUTION_BACKTICK, command, span);
                    free(command);
                }
                i += span;
                continue;
            }
        }
        i++;
    }
}
