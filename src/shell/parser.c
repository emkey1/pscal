#include "shell/parser.h"
#include "core/utils.h"
#include "shell/quote_markers.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RULE_MASK_COMMAND_START (SHELL_LEXER_RULE_1 | SHELL_LEXER_RULE_7)
#define RULE_MASK_COMMAND_CONTINUATION (SHELL_LEXER_RULE_7)
#define RULE_MASK_REDIRECT_TARGET (SHELL_LEXER_RULE_2)
#define RULE_MASK_HEREDOC_DELIMITER (SHELL_LEXER_RULE_3)
#define RULE_MASK_CASE_PATTERN (SHELL_LEXER_RULE_4)
#define RULE_MASK_FOR_NAME (SHELL_LEXER_RULE_5)
#define RULE_MASK_FOR_LIST (SHELL_LEXER_RULE_6 | SHELL_LEXER_RULE_1)
#define RULE_MASK_FUNCTION_NAME (SHELL_LEXER_RULE_8 | SHELL_LEXER_RULE_1)
#define RULE_MASK_FUNCTION_BODY (SHELL_LEXER_RULE_9)

#define STRUCTURAL_CLOSER_RPAREN (1u << 0)
#define STRUCTURAL_CLOSER_RBRACE (1u << 1)

typedef struct {
    ShellRedirection *redir;
    char *delimiter;
    bool strip_tabs;
    bool quoted;
} ShellPendingHereDoc;

typedef struct ShellPendingHereDocArray {
    ShellPendingHereDoc *items;
    size_t count;
    size_t capacity;
} ShellPendingHereDocArray;

static void pendingHereDocArrayInit(ShellPendingHereDocArray *array);
static void pendingHereDocArrayFree(ShellPendingHereDocArray *array);
static void pendingHereDocArrayPush(ShellPendingHereDocArray *array, ShellRedirection *redir,
                                    const char *delimiter, bool strip_tabs, bool quoted);

static void parserWordArrayInit(ShellWordArray *array);
static bool parserWordArrayAppend(ShellWordArray *array, ShellWord *word);
static void parserWordArrayFree(ShellWordArray *array);

static void parserScheduleRuleMask(ShellParser *parser, unsigned int mask);
static unsigned int parserStructuralCloserBit(ShellTokenType type);
static void shellParserAdvance(ShellParser *parser);
static void shellParserConsume(ShellParser *parser, ShellTokenType type, const char *message);
static bool shellParserCheck(const ShellParser *parser, ShellTokenType type);
static bool shellParserMatch(ShellParser *parser, ShellTokenType type);
static void shellParserSynchronize(ShellParser *parser);
static void parserErrorAt(ShellParser *parser, const ShellToken *token, const char *message);
static void parserReclassifyCurrentToken(ShellParser *parser, unsigned int mask);

static bool parserConsumePendingHereDocs(ShellParser *parser);
static char *parserCopyWordWithoutMarkers(const ShellWord *word);
static char *parserCopyTrimmedRange(const char *src, size_t start, size_t end);
static bool parserExtractCStyleForSegments(ShellParser *parser, size_t start_pos, char **init_out, char **cond_out,
                                          char **update_out);
static bool parserExtractArithmeticCommandExpression(ShellParser *parser, size_t start_pos, char **expr_out);

static bool parseCompleteCommands(ShellParser *parser, ShellProgram *program);
static bool parseCompleteCommand(ShellParser *parser, ShellProgram *program);
static bool parseList(ShellParser *parser, ShellProgram *program);
static ShellCommand *parseAndOr(ShellParser *parser);
static ShellPipeline *parsePipeline(ShellParser *parser);
static ShellCommand *parsePipelineCommand(ShellParser *parser);
static ShellCommand *parseCommand(ShellParser *parser);
static ShellCommand *parseSimpleCommand(ShellParser *parser);
static ShellCommand *parseArithmeticCommand(ShellParser *parser);
static ShellCommand *parseCompoundCommand(ShellParser *parser);
static ShellCommand *parseBraceGroup(ShellParser *parser);
static ShellCommand *parseSubshell(ShellParser *parser);
static ShellProgram *parseCompoundListUntil(ShellParser *parser, ShellTokenType terminator1,
                                           ShellTokenType terminator2, ShellTokenType terminator3);
static ShellCommand *parseIfClause(ShellParser *parser);
static ShellCommand *parseWhileClause(ShellParser *parser, bool is_until);
static ShellCommand *parseForClause(ShellParser *parser);
static ShellCommand *parseCStyleForClause(ShellParser *parser, int line, int column);
static ShellCommand *parseCaseClause(ShellParser *parser);
static ShellCommand *parseFunctionDefinition(ShellParser *parser);
static ShellCommand *parseFunctionDefinitionFromName(ShellParser *parser);
static bool parserIsFunctionDefinitionStart(ShellParser *parser);
static void parseLinebreak(ShellParser *parser);
static bool tokenStartsCommand(const ShellToken *token);

static ShellWord *parseWordToken(ShellParser *parser, const char *context_message);
static ShellRedirection *parseRedirection(ShellParser *parser, bool *strip_tabs_out);

static void populateWordExpansions(ShellWord *word);
static bool parseDollarCommandSubstitution(const char *text, size_t start, size_t *out_span,
                                           char **out_command);
static bool parseBacktickCommandSubstitution(const char *text, size_t start, size_t *out_span,
                                             char **out_command);
static char *normalizeDollarCommand(const char *command, size_t len);
static char *normalizeBacktickCommand(const char *command, size_t len);

ShellProgram *shellParseString(const char *source, ShellParser *parser) {
    if (!parser) {
        return NULL;
    }

    memset(parser, 0, sizeof(*parser));
    shellInitLexer(&parser->lexer, source);
    parser->had_error = false;
    parser->panic_mode = false;
    parser->next_rule_mask = RULE_MASK_COMMAND_START;
    parser->pending_here_docs = (ShellPendingHereDocArray *)calloc(1, sizeof(ShellPendingHereDocArray));
    if (!parser->pending_here_docs) {
        return NULL;
    }
    pendingHereDocArrayInit(parser->pending_here_docs);

    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);

    ShellProgram *program = shellCreateProgram();
    if (!program) {
        return NULL;
    }

    if (!parseCompleteCommands(parser, program)) {
        if (parser->had_error) {
            shellFreeProgram(program);
            program = NULL;
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
    if (parser->pending_here_docs) {
        pendingHereDocArrayFree(parser->pending_here_docs);
        free(parser->pending_here_docs);
        parser->pending_here_docs = NULL;
    }
}

static void pendingHereDocArrayInit(ShellPendingHereDocArray *array) {
    if (!array) {
        return;
    }
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static void pendingHereDocArrayFree(ShellPendingHereDocArray *array) {
    if (!array) {
        return;
    }
    for (size_t i = 0; i < array->count; ++i) {
        free(array->items[i].delimiter);
    }
    free(array->items);
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static void pendingHereDocArrayPush(ShellPendingHereDocArray *array, ShellRedirection *redir,
                                    const char *delimiter, bool strip_tabs, bool quoted) {
    if (!array || !redir || !delimiter) {
        return;
    }
    if (array->count + 1 > array->capacity) {
        size_t new_capacity = array->capacity ? array->capacity * 2 : 4;
        ShellPendingHereDoc *new_items =
            (ShellPendingHereDoc *)realloc(array->items, new_capacity * sizeof(ShellPendingHereDoc));
        if (!new_items) {
            return;
        }
        array->items = new_items;
        array->capacity = new_capacity;
    }
    ShellPendingHereDoc *entry = &array->items[array->count++];
    entry->redir = redir;
    entry->delimiter = strdup(delimiter);
    entry->strip_tabs = strip_tabs;
    entry->quoted = quoted;
}

static void parserWordArrayInit(ShellWordArray *array) {
    if (!array) {
        return;
    }
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static bool parserWordArrayAppend(ShellWordArray *array, ShellWord *word) {
    if (!array || !word) {
        return false;
    }
    if (array->count + 1 > array->capacity) {
        size_t new_capacity = array->capacity ? array->capacity * 2 : 4;
        ShellWord **new_items = (ShellWord **)realloc(array->items, new_capacity * sizeof(ShellWord *));
        if (!new_items) {
            return false;
        }
        array->items = new_items;
        array->capacity = new_capacity;
    }
    array->items[array->count++] = word;
    return true;
}

static void parserWordArrayFree(ShellWordArray *array) {
    if (!array) {
        return;
    }
    for (size_t i = 0; i < array->count; ++i) {
        shellFreeWord(array->items[i]);
    }
    free(array->items);
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static void parserScheduleRuleMask(ShellParser *parser, unsigned int mask) {
    if (!parser) {
        return;
    }
    parser->next_rule_mask = mask;
}

static unsigned int parserStructuralCloserBit(ShellTokenType type) {
    switch (type) {
        case SHELL_TOKEN_RPAREN:
            return STRUCTURAL_CLOSER_RPAREN;
        case SHELL_TOKEN_RBRACE:
            return STRUCTURAL_CLOSER_RBRACE;
        default:
            return 0u;
    }
}

static void applyLexicalRules(ShellToken *token) {
    if (!token) {
        return;
    }
    unsigned int mask = token->rule_mask;
    bool reserved_allowed = (mask & SHELL_LEXER_RULE_1) != 0;
    bool treat_as_assignment = (mask & SHELL_LEXER_RULE_7) != 0;
    bool treat_as_for_name = (mask & SHELL_LEXER_RULE_5) != 0;
    bool treat_as_function_name = (mask & SHELL_LEXER_RULE_8) != 0;
    bool force_word_context = (mask & (SHELL_LEXER_RULE_2 | SHELL_LEXER_RULE_3 | SHELL_LEXER_RULE_4 |
                                      SHELL_LEXER_RULE_9)) != 0;

    if (force_word_context && token->reserved_candidate) {
        token->type = SHELL_TOKEN_WORD;
    } else if (token->reserved_candidate) {
        token->type = reserved_allowed ? token->reserved_type : SHELL_TOKEN_WORD;
    }

    if (!treat_as_assignment && token->type == SHELL_TOKEN_ASSIGNMENT_WORD && !token->assignment_candidate) {
        token->type = SHELL_TOKEN_WORD;
    }
    if (treat_as_assignment && token->assignment_candidate) {
        token->type = SHELL_TOKEN_ASSIGNMENT_WORD;
    }

    if ((treat_as_for_name || treat_as_function_name) && token->name_candidate) {
        token->type = SHELL_TOKEN_NAME;
    }

    if ((mask & SHELL_LEXER_RULE_6) != 0 && token->reserved_candidate) {
        token->type = token->reserved_type;
    }

    if ((mask & SHELL_LEXER_RULE_1) != 0 && token->lexeme && token->length == 1) {
        ShellTokenType structural = SHELL_TOKEN_ERROR;
        switch (token->lexeme[0]) {
            case '(': structural = SHELL_TOKEN_LPAREN; break;
            case ')': structural = SHELL_TOKEN_RPAREN; break;
            case '{': structural = SHELL_TOKEN_LBRACE; break;
            case '}': structural = SHELL_TOKEN_RBRACE; break;
            default: break;
        }
        if (structural != SHELL_TOKEN_ERROR) {
            token->type = structural;
            token->base_type = structural;
            token->reserved_type = structural;
        }
    }
}

static void shellParserAdvance(ShellParser *parser) {
    if (!parser) {
        return;
    }
    shellFreeToken(&parser->previous);
    parser->previous = parser->current;

    if (parser->previous.type == SHELL_TOKEN_NEWLINE) {
        parserConsumePendingHereDocs(parser);
    }

    shellLexerSetRuleMask(&parser->lexer, parser->next_rule_mask);
    parser->current = shellNextToken(&parser->lexer);
    applyLexicalRules(&parser->current);
}

static bool shellParserCheck(const ShellParser *parser, ShellTokenType type) {
    return parser && parser->current.type == type;
}

static bool shellParserMatch(ShellParser *parser, ShellTokenType type) {
    if (!parser || !shellParserCheck(parser, type)) {
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
    fprintf(stderr, "shell parse error at %d:%d: %s\n", line, column,
            message ? message : "error");
    parser->had_error = true;
    parser->panic_mode = true;
}

static void shellParserConsume(ShellParser *parser, ShellTokenType type, const char *message) {
    if (!parser) {
        return;
    }
    if (parser->current.type == type) {
        shellParserAdvance(parser);
        return;
    }
    parserErrorAt(parser, &parser->current, message);
}

static void shellParserSynchronize(ShellParser *parser) {
    if (!parser) {
        return;
    }
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
            case SHELL_TOKEN_CASE:
            case SHELL_TOKEN_ESAC:
                parser->panic_mode = false;
                return;
            default:
                break;
        }
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
    }
}

static void parserReclassifyCurrentToken(ShellParser *parser, unsigned int mask) {
    if (!parser) {
        return;
    }
    parser->current.rule_mask = mask;
    applyLexicalRules(&parser->current);
}

static bool parserConsumePendingHereDocs(ShellParser *parser) {
    if (!parser || !parser->pending_here_docs || parser->pending_here_docs->count == 0) {
        return true;
    }
    ShellPendingHereDocArray *array = parser->pending_here_docs;
    ShellLexer *lexer = &parser->lexer;
    const char *src = lexer->src;
    size_t length = lexer->length;

    for (size_t idx = 0; idx < array->count; ++idx) {
        ShellPendingHereDoc *pending = &array->items[idx];
        size_t buffer_capacity = 0;
        size_t buffer_length = 0;
        char *buffer = NULL;
        bool done = false;
        while (!done) {
            if (lexer->pos >= length) {
                parserErrorAt(parser, NULL, "Unexpected EOF in here-document");
                free(buffer);
                return false;
            }
            size_t line_start = lexer->pos;
            int line_number = lexer->line;
            int column_number = lexer->column;
            while (lexer->pos < length && src[lexer->pos] != '\n') {
                lexer->pos++;
                lexer->column++;
            }
            size_t raw_len = lexer->pos - line_start;
            const char *raw_text = src + line_start;

            char *line = (char *)malloc(raw_len + 1);
            if (!line) {
                free(buffer);
                return false;
            }
            memcpy(line, raw_text, raw_len);
            line[raw_len] = '\0';

            if (lexer->pos < length && src[lexer->pos] == '\n') {
                lexer->pos++;
                lexer->line++;
                lexer->column = 1;
            }

            const char *comparison = line;
            if (pending->strip_tabs) {
                while (*comparison == '\t') {
                    comparison++;
                }
            }

            bool matches = strcmp(comparison, pending->delimiter) == 0;
            if (matches) {
                free(line);
                done = true;
                break;
            }

            const char *body_line = line;
            if (pending->strip_tabs) {
                while (*body_line == '\t') {
                    body_line++;
                }
            }

            size_t body_len = strlen(body_line);
            if (buffer_length + body_len + 1 > buffer_capacity) {
                size_t new_capacity = buffer_capacity ? buffer_capacity * 2 : 64;
                while (new_capacity < buffer_length + body_len + 1) {
                    new_capacity *= 2;
                }
                char *tmp = (char *)realloc(buffer, new_capacity);
                if (!tmp) {
                    free(line);
                    free(buffer);
                    return false;
                }
                buffer = tmp;
                buffer_capacity = new_capacity;
            }
            memcpy(buffer + buffer_length, body_line, body_len);
            buffer_length += body_len;
            buffer[buffer_length++] = '\n';
            free(line);
            (void)line_number;
            (void)column_number;
        }
        if (!pending->redir) {
            free(buffer);
            continue;
        }
        if (!buffer) {
            buffer = strdup("");
        } else {
            if (buffer_length == 0) {
                char *tmp = (char *)realloc(buffer, 1);
                if (tmp) {
                    buffer = tmp;
                }
            } else {
                char *tmp = (char *)realloc(buffer, buffer_length + 1);
                if (tmp) {
                    buffer = tmp;
                }
                buffer[buffer_length] = '\0';
            }
        }
        shellRedirectionSetHereDocument(pending->redir, buffer ? buffer : "", pending->quoted);
        free(buffer);
        free(pending->delimiter);
        pending->delimiter = NULL;
    }

    array->count = 0;
    return true;
}

static char *parserCopyWordWithoutMarkers(const ShellWord *word) {
    if (!word || !word->text) {
        return NULL;
    }
    size_t len = strlen(word->text);
    char *result = (char *)malloc(len + 1);
    if (!result) {
        return NULL;
    }
    size_t out = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = word->text[i];
        if (c == SHELL_QUOTE_MARK_SINGLE || c == SHELL_QUOTE_MARK_DOUBLE) {
            continue;
        }
        result[out++] = c;
    }
    result[out] = '\0';
    char *shrunk = (char *)realloc(result, out + 1);
    return shrunk ? shrunk : result;
}

static char *parserCopyTrimmedRange(const char *src, size_t start, size_t end) {
    if (!src || end <= start) {
        return strdup("");
    }
    while (start < end && isspace((unsigned char)src[start])) {
        start++;
    }
    while (end > start && isspace((unsigned char)src[end - 1])) {
        end--;
    }
    size_t len = end > start ? (end - start) : 0;
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    if (len > 0) {
        memcpy(out, src + start, len);
    }
    out[len] = '\0';
    return out;
}

static bool parserExtractArithmeticCommandExpression(ShellParser *parser, size_t start_pos, char **expr_out) {
    if (!parser || !parser->lexer.src) {
        return false;
    }
    ShellLexer *lexer = &parser->lexer;
    const char *src = lexer->src;
    size_t length = lexer->length;
    size_t pos = start_pos;
    int depth = 1;
    size_t expr_end = SIZE_MAX;
    int line = lexer->line;
    int column = lexer->column;

    while (pos < length) {
        char ch = src[pos];
        if (ch == '(') {
            depth++;
        } else if (ch == ')') {
            depth--;
            if (depth == 0) {
                expr_end = pos;
                pos++;
                column++;
                break;
            }
        }
        if (ch == '\n') {
            line++;
            column = 1;
        } else {
            column++;
        }
        pos++;
    }

    if (expr_end == SIZE_MAX) {
        parserErrorAt(parser, &parser->current, "Expected '))' to close arithmetic command");
        return false;
    }
    if (pos >= length || src[pos] != ')') {
        parserErrorAt(parser, &parser->current, "Expected '))' to close arithmetic command");
        return false;
    }

    column++;
    pos++;

    char *expr = parserCopyTrimmedRange(src, start_pos, expr_end);
    if (!expr) {
        parserErrorAt(parser, &parser->current, "Out of memory parsing arithmetic command");
        return false;
    }

    if (expr_out) {
        *expr_out = expr;
    } else {
        free(expr);
    }

    lexer->pos = pos;
    lexer->line = line;
    lexer->column = column;
    lexer->at_line_start = (column == 1);

    shellLexerSetRuleMask(lexer, parser->next_rule_mask);
    shellFreeToken(&parser->current);
    parser->current = shellNextToken(lexer);
    applyLexicalRules(&parser->current);
    return true;
}

static bool parserExtractCStyleForSegments(ShellParser *parser, size_t start_pos, char **init_out, char **cond_out,
                                          char **update_out) {
    if (!parser || !parser->lexer.src) {
        return false;
    }
    ShellLexer *lexer = &parser->lexer;
    const char *src = lexer->src;
    size_t length = lexer->length;
    size_t pos = start_pos;
    int depth = 1;
    size_t semicolons[2] = {SIZE_MAX, SIZE_MAX};
    size_t semicolon_count = 0;
    size_t expr_end = SIZE_MAX;
    int line = lexer->line;
    int column = lexer->column;

    while (pos < length) {
        char ch = src[pos];
        if (ch == '(') {
            depth++;
        } else if (ch == ')') {
            depth--;
            if (depth == 0) {
                expr_end = pos;
                pos++;
                column++;
                break;
            }
        } else if (ch == ';' && depth == 1 && semicolon_count < 2) {
            semicolons[semicolon_count++] = pos;
        }
        if (ch == '\n') {
            line++;
            column = 1;
        } else {
            column++;
        }
        pos++;
    }

    if (expr_end == SIZE_MAX) {
        parserErrorAt(parser, &parser->current, "Expected '))' to close arithmetic for clause");
        return false;
    }
    if (pos >= length || src[pos] != ')') {
        parserErrorAt(parser, &parser->current, "Expected '))' to close arithmetic for clause");
        return false;
    }

    column++;
    pos++;

    if (semicolon_count < 2 || semicolons[0] == SIZE_MAX || semicolons[1] == SIZE_MAX) {
        parserErrorAt(parser, &parser->current,
                      "Arithmetic for clause requires two ';' separators");
        return false;
    }

    size_t init_start = start_pos;
    size_t init_end = semicolons[0];
    size_t cond_start = semicolons[0] + 1;
    size_t cond_end = semicolons[1];
    size_t update_start = semicolons[1] + 1;
    size_t update_end = expr_end;

    char *init = parserCopyTrimmedRange(src, init_start, init_end);
    char *cond = parserCopyTrimmedRange(src, cond_start, cond_end);
    char *update = parserCopyTrimmedRange(src, update_start, update_end);
    if (!init || !cond || !update) {
        free(init);
        free(cond);
        free(update);
        parserErrorAt(parser, &parser->current, "Out of memory parsing arithmetic for clause");
        return false;
    }

    if (init_out) {
        *init_out = init;
    } else {
        free(init);
    }
    if (cond_out) {
        *cond_out = cond;
    } else {
        free(cond);
    }
    if (update_out) {
        *update_out = update;
    } else {
        free(update);
    }

    lexer->pos = pos;
    lexer->line = line;
    lexer->column = column;
    lexer->at_line_start = (column == 1);

    shellLexerSetRuleMask(lexer, parser->next_rule_mask);
    shellFreeToken(&parser->current);
    parser->current = shellNextToken(lexer);
    applyLexicalRules(&parser->current);
    return true;
}

static void parseLinebreak(ShellParser *parser) {
    if (!parser) {
        return;
    }
    while (parser->current.type == SHELL_TOKEN_NEWLINE) {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
    }
}

static bool tokenStartsCommand(const ShellToken *token) {
    if (!token) {
        return false;
    }
    switch (token->type) {
        case SHELL_TOKEN_WORD:
        case SHELL_TOKEN_ASSIGNMENT_WORD:
        case SHELL_TOKEN_NAME:
        case SHELL_TOKEN_PARAMETER:
        case SHELL_TOKEN_IO_NUMBER:
        case SHELL_TOKEN_LPAREN:
        case SHELL_TOKEN_LBRACE:
        case SHELL_TOKEN_BANG:
        case SHELL_TOKEN_FUNCTION:
        case SHELL_TOKEN_IF:
        case SHELL_TOKEN_WHILE:
        case SHELL_TOKEN_UNTIL:
        case SHELL_TOKEN_FOR:
        case SHELL_TOKEN_CASE:
            return true;
        default:
            return false;
    }
}

static bool parseCompleteCommands(ShellParser *parser, ShellProgram *program) {
    if (!parser || !program) {
        return false;
    }
    parseLinebreak(parser);
    while (!parser->had_error && parser->current.type != SHELL_TOKEN_EOF) {
        if (!parseCompleteCommand(parser, program)) {
            if (parser->panic_mode) {
                shellParserSynchronize(parser);
            } else {
                return false;
            }
        }
        parseLinebreak(parser);
    }
    return !parser->had_error;
}

static bool parseCompleteCommand(ShellParser *parser, ShellProgram *program) {
    if (!parser || !program) {
        return false;
    }
    if (!parseList(parser, program)) {
        return false;
    }

    if (parser->current.type == SHELL_TOKEN_SEMICOLON || parser->current.type == SHELL_TOKEN_AMPERSAND) {
        ShellTokenType sep = parser->current.type;
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        if (sep == SHELL_TOKEN_AMPERSAND && program->commands.count > 0) {
            ShellCommand *cmd = program->commands.items[program->commands.count - 1];
            if (cmd) {
                cmd->exec.runs_in_background = true;
                cmd->exec.is_async_parent = true;
            }
        }
        parseLinebreak(parser);
    }
    return true;
}

static bool parseList(ShellParser *parser, ShellProgram *program) {
    if (!parser || !program) {
        return false;
    }
    while (true) {
        ShellCommand *command = parseAndOr(parser);
        if (!command) {
            return false;
        }
        shellProgramAddCommand(program, command);

        if (parser->current.type == SHELL_TOKEN_AMPERSAND) {
            command->exec.runs_in_background = true;
            command->exec.is_async_parent = true;
            parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
            shellParserAdvance(parser);
            parseLinebreak(parser);
            if (!tokenStartsCommand(&parser->current)) {
                break;
            }
            continue;
        }
        if (parser->current.type == SHELL_TOKEN_SEMICOLON) {
            parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
            shellParserAdvance(parser);
            parseLinebreak(parser);
            if (!tokenStartsCommand(&parser->current)) {
                break;
            }
            continue;
        }
        break;
    }
    return true;
}

static ShellCommand *parseAndOr(ShellParser *parser) {
    if (!parser) {
        return NULL;
    }
    ShellPipeline *first = parsePipeline(parser);
    if (!first) {
        return NULL;
    }

    ShellLogicalList *logical = NULL;
    while (parser->current.type == SHELL_TOKEN_AND_AND || parser->current.type == SHELL_TOKEN_OR_OR) {
        ShellLogicalConnector connector =
            (parser->current.type == SHELL_TOKEN_AND_AND) ? SHELL_LOGICAL_AND : SHELL_LOGICAL_OR;
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        parseLinebreak(parser);
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
            if (first && first->command_count > 0 && first->commands[0]) {
                cmd->line = first->commands[0]->line;
                cmd->column = first->commands[0]->column;
            } else {
                cmd->line = parser->current.line;
                cmd->column = parser->current.column;
            }
        }
        return cmd;
    }

    ShellCommand *cmd = shellCreatePipelineCommand(first);
    if (cmd && first) {
        if (first->command_count > 0 && first->commands[0]) {
            cmd->line = first->commands[0]->line;
            cmd->column = first->commands[0]->column;
        } else {
            cmd->line = parser->current.line;
            cmd->column = parser->current.column;
        }
        for (size_t i = 0; i < first->command_count; ++i) {
            ShellCommand *member = first->commands[i];
            member->exec.pipeline_index = (int)i;
            member->exec.is_pipeline_head = (i == 0);
            member->exec.is_pipeline_tail = (i + 1 == first->command_count);
        }
    }
    return cmd;
}

static ShellPipeline *parsePipeline(ShellParser *parser) {
    if (!parser) {
        return NULL;
    }
    bool negate = false;
    while (parser->current.type == SHELL_TOKEN_BANG) {
        negate = !negate;
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        parseLinebreak(parser);
    }

    ShellPipeline *pipeline = shellCreatePipeline();
    if (!pipeline) {
        return NULL;
    }
    shellPipelineSetNegated(pipeline, negate);

    ShellCommand *command = parsePipelineCommand(parser);
    if (!command) {
        return pipeline;
    }
    shellPipelineAddCommand(pipeline, command);

    while (parser->current.type == SHELL_TOKEN_PIPE || parser->current.type == SHELL_TOKEN_PIPE_AMP) {
        ShellTokenType op = parser->current.type;
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        parseLinebreak(parser);
        ShellCommand *next = parsePipelineCommand(parser);
        if (!next) {
            break;
        }
        shellPipelineAddCommand(pipeline, next);
        if (op == SHELL_TOKEN_PIPE_AMP && pipeline->command_count >= 2) {
            shellPipelineSetMergeStderr(pipeline, pipeline->command_count - 2, true);
        }
    }

    for (size_t i = 0; i < pipeline->command_count; ++i) {
        ShellCommand *member = pipeline->commands[i];
        member->exec.pipeline_index = (int)i;
        member->exec.is_pipeline_head = (i == 0);
        member->exec.is_pipeline_tail = (i + 1 == pipeline->command_count);
    }

    return pipeline;
}

static ShellCommand *parsePipelineCommand(ShellParser *parser) {
    if (!parser) {
        return NULL;
    }
    ShellCommand *command = parseCommand(parser);
    if (command && parser->pending_here_docs && parser->pending_here_docs->count > 0) {
        parserConsumePendingHereDocs(parser);
    }
    return command;
}

static ShellCommand *parseCommand(ShellParser *parser) {
    if (!parser) {
        return NULL;
    }
    switch (parser->current.type) {
        case SHELL_TOKEN_FUNCTION:
            return parseFunctionDefinition(parser);
        case SHELL_TOKEN_LBRACE:
        case SHELL_TOKEN_LPAREN:
        case SHELL_TOKEN_IF:
        case SHELL_TOKEN_FOR:
        case SHELL_TOKEN_WHILE:
        case SHELL_TOKEN_UNTIL:
        case SHELL_TOKEN_CASE:
            return parseCompoundCommand(parser);
        default:
            if (parserIsFunctionDefinitionStart(parser)) {
                return parseFunctionDefinitionFromName(parser);
            }
            return parseSimpleCommand(parser);
    }
}

static ShellCommand *parseArithmeticCommand(ShellParser *parser) {
    if (!parser) {
        return NULL;
    }

    int line = parser->current.line;
    int column = parser->current.column;

    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);

    size_t start_pos = parser->lexer.pos;
    if (parser->current.length <= parser->lexer.pos) {
        start_pos = parser->lexer.pos - parser->current.length;
    }

    char *expression = NULL;
    if (!parserExtractArithmeticCommandExpression(parser, start_pos, &expression)) {
        free(expression);
        return NULL;
    }

    ShellCommand *command = shellCreateArithmeticCommand(expression);
    if (!command) {
        return NULL;
    }
    command->line = line;
    command->column = column;

    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_CONTINUATION);

    while (!parser->had_error) {
        bool strip_tabs = false;
        ShellRedirection *redir = parseRedirection(parser, &strip_tabs);
        if (!redir) {
            break;
        }
        shellCommandAddRedirection(command, redir);
        if (redir->type == SHELL_REDIRECT_HEREDOC && parser->pending_here_docs) {
            ShellWord *target = shellRedirectionGetWordTarget(redir);
            char *delimiter = parserCopyWordWithoutMarkers(target);
            if (!delimiter) {
                delimiter = strdup("");
            }
            bool quoted = target && (target->single_quoted || target->double_quoted);
            pendingHereDocArrayPush(parser->pending_here_docs, redir, delimiter ? delimiter : "",
                                    strip_tabs, quoted);
            free(delimiter);
        }
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_CONTINUATION);
    }

    if (parser->had_error) {
        shellFreeCommand(command);
        return NULL;
    }

    return command;
}

static ShellCommand *parseSimpleCommand(ShellParser *parser) {
    if (!parser) {
        return NULL;
    }
    if (parser->current.type == SHELL_TOKEN_DLPAREN) {
        return parseArithmeticCommand(parser);
    }
    ShellCommand *command = shellCreateSimpleCommand();
    if (!command) {
        return NULL;
    }
    command->line = parser->current.line;
    command->column = parser->current.column;

    bool seen_word = false;
    while (!parser->had_error) {
        if (parser->current.type == SHELL_TOKEN_WORD && parser->current.lexeme && parser->current.length == 1) {
            char ch = parser->current.lexeme[0];
            bool treat_as_closer = false;
            if (ch == ')' && (parser->structural_closer_mask & STRUCTURAL_CLOSER_RPAREN)) {
                treat_as_closer = true;
            } else if (ch == '}' && (parser->structural_closer_mask & STRUCTURAL_CLOSER_RBRACE)) {
                treat_as_closer = true;
            }
            if (treat_as_closer && !parser->current.single_quoted && !parser->current.double_quoted) {
                parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
                if (parser->current.type == SHELL_TOKEN_RPAREN || parser->current.type == SHELL_TOKEN_RBRACE) {
                    break;
                }
            }
        }
        if (parser->current.type == SHELL_TOKEN_WORD || parser->current.type == SHELL_TOKEN_ASSIGNMENT_WORD ||
            parser->current.type == SHELL_TOKEN_NAME || parser->current.type == SHELL_TOKEN_PARAMETER) {
            ShellWord *word = parseWordToken(parser, NULL);
            if (word) {
                populateWordExpansions(word);
                shellCommandAddWord(command, word);
                seen_word = true;
            }
            parserScheduleRuleMask(parser, RULE_MASK_COMMAND_CONTINUATION);
            continue;
        }

        bool strip_tabs = false;
        ShellRedirection *redir = parseRedirection(parser, &strip_tabs);
        if (redir) {
            shellCommandAddRedirection(command, redir);
            if (redir->type == SHELL_REDIRECT_HEREDOC && parser->pending_here_docs) {
                ShellWord *target = shellRedirectionGetWordTarget(redir);
                char *delimiter = parserCopyWordWithoutMarkers(target);
                if (!delimiter) {
                    delimiter = strdup("");
                }
                bool quoted = target && (target->single_quoted || target->double_quoted);
                pendingHereDocArrayPush(parser->pending_here_docs, redir, delimiter ? delimiter : "",
                                        strip_tabs, quoted);
                free(delimiter);
            }
            parserScheduleRuleMask(parser, RULE_MASK_COMMAND_CONTINUATION);
            continue;
        }
        break;
    }

    if (!seen_word && command->data.simple.words.count == 0 && command->redirections.count == 0) {
        shellFreeCommand(command);
        parserErrorAt(parser, &parser->current, "Expected command");
        return NULL;
    }
    return command;
}

static ShellCommand *parseCompoundCommand(ShellParser *parser) {
    if (!parser) {
        return NULL;
    }
    ShellCommand *command = NULL;
    switch (parser->current.type) {
        case SHELL_TOKEN_LBRACE:
            command = parseBraceGroup(parser);
            break;
        case SHELL_TOKEN_LPAREN:
            command = parseSubshell(parser);
            break;
        case SHELL_TOKEN_IF:
            command = parseIfClause(parser);
            break;
        case SHELL_TOKEN_WHILE:
            command = parseWhileClause(parser, false);
            break;
        case SHELL_TOKEN_UNTIL:
            command = parseWhileClause(parser, true);
            break;
        case SHELL_TOKEN_FOR:
            command = parseForClause(parser);
            break;
        case SHELL_TOKEN_CASE:
            command = parseCaseClause(parser);
            break;
        default:
            break;
    }

    if (!command) {
        return NULL;
    }

    while (true) {
        bool strip_tabs = false;
        ShellRedirection *redir = parseRedirection(parser, &strip_tabs);
        if (!redir) {
            break;
        }
        shellCommandAddRedirection(command, redir);
        if (redir->type == SHELL_REDIRECT_HEREDOC && parser->pending_here_docs) {
            ShellWord *target = shellRedirectionGetWordTarget(redir);
            char *delimiter = parserCopyWordWithoutMarkers(target);
            if (!delimiter) {
                delimiter = strdup("");
            }
            bool quoted = target && (target->single_quoted || target->double_quoted);
            pendingHereDocArrayPush(parser->pending_here_docs, redir, delimiter ? delimiter : "",
                                    strip_tabs, quoted);
            free(delimiter);
        }
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_CONTINUATION);
    }

    return command;
}

static ShellCommand *parseBraceGroup(ShellParser *parser) {
    int line = parser->current.line;
    int column = parser->current.column;
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);
    parseLinebreak(parser);
    ShellProgram *body = parseCompoundListUntil(parser, SHELL_TOKEN_RBRACE, SHELL_TOKEN_EOF, SHELL_TOKEN_EOF);
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    shellParserConsume(parser, SHELL_TOKEN_RBRACE, "Expected '}' to close brace group");
    ShellCommand *command = shellCreateBraceGroupCommand(body);
    if (command) {
        command->line = line;
        command->column = column;
    }
    return command;
}

static ShellCommand *parseSubshell(ShellParser *parser) {
    int line = parser->current.line;
    int column = parser->current.column;
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);
    parseLinebreak(parser);
    ShellProgram *body = parseCompoundListUntil(parser, SHELL_TOKEN_RPAREN, SHELL_TOKEN_EOF, SHELL_TOKEN_EOF);
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    shellParserConsume(parser, SHELL_TOKEN_RPAREN, "Expected ')' to close subshell");
    ShellCommand *command = shellCreateSubshellCommand(body);
    if (command) {
        command->line = line;
        command->column = column;
    }
    return command;
}

static ShellProgram *parseCompoundListUntil(ShellParser *parser, ShellTokenType terminator1,
                                           ShellTokenType terminator2, ShellTokenType terminator3) {
    if (!parser) {
        return NULL;
    }
    unsigned int saved_closer_mask = parser->structural_closer_mask;
    parser->structural_closer_mask |= parserStructuralCloserBit(terminator1);
    parser->structural_closer_mask |= parserStructuralCloserBit(terminator2);
    parser->structural_closer_mask |= parserStructuralCloserBit(terminator3);

    ShellProgram *program = shellCreateProgram();
    if (!program) {
        parser->structural_closer_mask = saved_closer_mask;
        return NULL;
    }
    parseLinebreak(parser);
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    while (!parser->had_error && parser->current.type != terminator1 && parser->current.type != terminator2 &&
           parser->current.type != terminator3 && parser->current.type != SHELL_TOKEN_EOF) {
        if (!parseList(parser, program)) {
            break;
        }
        parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
        if (parser->current.type == SHELL_TOKEN_SEMICOLON || parser->current.type == SHELL_TOKEN_AMPERSAND) {
            parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
            shellParserAdvance(parser);
            parseLinebreak(parser);
            parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
        }
        parseLinebreak(parser);
        parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    }
    parser->structural_closer_mask = saved_closer_mask;
    return program;
}

static ShellCommand *parseIfClause(ShellParser *parser) {
    int line = parser->current.line;
    int column = parser->current.column;
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);
    ShellCommand *condition = parseAndOr(parser);
    parseLinebreak(parser);
    if (parser->current.type == SHELL_TOKEN_SEMICOLON) {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        parseLinebreak(parser);
    }
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    shellParserConsume(parser, SHELL_TOKEN_THEN, "Expected 'then' after if condition");
    parseLinebreak(parser);
    ShellProgram *then_block = parseCompoundListUntil(parser, SHELL_TOKEN_ELIF, SHELL_TOKEN_ELSE, SHELL_TOKEN_FI);

    ShellProgram *else_block = NULL;
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    if (parser->current.type == SHELL_TOKEN_ELIF) {
        ShellCommand *elif_cmd = parseIfClause(parser);
        else_block = shellCreateProgram();
        shellProgramAddCommand(else_block, elif_cmd);
    } else if (parser->current.type == SHELL_TOKEN_ELSE) {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        parseLinebreak(parser);
        else_block = parseCompoundListUntil(parser, SHELL_TOKEN_FI, SHELL_TOKEN_EOF, SHELL_TOKEN_EOF);
        parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
        shellParserConsume(parser, SHELL_TOKEN_FI, "Expected 'fi' to close if");
    } else {
        parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
        shellParserConsume(parser, SHELL_TOKEN_FI, "Expected 'fi' to close if");
    }

    ShellConditional *conditional = shellCreateConditional(condition, then_block, else_block);
    ShellCommand *command = shellCreateConditionalCommand(conditional);
    if (command) {
        command->line = line;
        command->column = column;
    }
    return command;
}

static ShellCommand *parseWhileClause(ShellParser *parser, bool is_until) {
    int line = parser->current.line;
    int column = parser->current.column;
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);
    ShellCommand *condition = parseAndOr(parser);
    parseLinebreak(parser);
    if (parser->current.type == SHELL_TOKEN_SEMICOLON) {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        parseLinebreak(parser);
    }
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    shellParserConsume(parser, SHELL_TOKEN_DO, "Expected 'do' after loop condition");
    parseLinebreak(parser);
    ShellProgram *body = parseCompoundListUntil(parser, SHELL_TOKEN_DONE, SHELL_TOKEN_EOF, SHELL_TOKEN_EOF);
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    shellParserConsume(parser, SHELL_TOKEN_DONE, "Expected 'done' to close loop");
    ShellLoop *loop = shellCreateLoop(is_until, condition, body);
    ShellCommand *command = shellCreateLoopCommand(loop);
    if (command) {
        command->line = line;
        command->column = column;
    }
    return command;
}

static ShellCommand *parseCStyleForClause(ShellParser *parser, int line, int column) {
    if (!parser) {
        return NULL;
    }
    char *init = NULL;
    char *cond = NULL;
    char *update = NULL;

    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);

    size_t start_pos = parser->lexer.pos;
    if (parser->current.length <= parser->lexer.pos) {
        start_pos = parser->lexer.pos - parser->current.length;
    }

    if (!parserExtractCStyleForSegments(parser, start_pos, &init, &cond, &update)) {
        free(init);
        free(cond);
        free(update);
        return NULL;
    }

    parseLinebreak(parser);
    if (parser->current.type == SHELL_TOKEN_SEMICOLON) {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        parseLinebreak(parser);
    }
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    shellParserConsume(parser, SHELL_TOKEN_DO, "Expected 'do' in for clause");
    parseLinebreak(parser);
    ShellProgram *body = parseCompoundListUntil(parser, SHELL_TOKEN_DONE, SHELL_TOKEN_EOF, SHELL_TOKEN_EOF);
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    shellParserConsume(parser, SHELL_TOKEN_DONE, "Expected 'done' to close for clause");

    ShellLoop *loop = shellCreateCStyleForLoop(init, cond, update, body);
    free(init);
    free(cond);
    free(update);
    if (!loop) {
        shellFreeProgram(body);
        return NULL;
    }

    ShellCommand *command = shellCreateLoopCommand(loop);
    if (command) {
        command->line = line;
        command->column = column;
    }
    return command;
}

static ShellCommand *parseForClause(ShellParser *parser) {
    int line = parser->current.line;
    int column = parser->current.column;
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);

    if (parser->current.type == SHELL_TOKEN_DLPAREN) {
        return parseCStyleForClause(parser, line, column);
    }

    parserScheduleRuleMask(parser, RULE_MASK_FOR_NAME);
    parserReclassifyCurrentToken(parser, RULE_MASK_FOR_NAME);
    shellParserAdvance(parser);
    if (parser->previous.type != SHELL_TOKEN_NAME) {
        parserErrorAt(parser, &parser->previous, "Expected name after 'for'");
        return NULL;
    }
    const char *name_text = parser->previous.lexeme ? parser->previous.lexeme : "";
    ShellWord *name_word = shellCreateWord(name_text, false, false, false, false, parser->previous.line,
                                           parser->previous.column);

    ShellProgram *body = NULL;
    ShellWordArray value_words;
    parserWordArrayInit(&value_words);

    parseLinebreak(parser);
    parserReclassifyCurrentToken(parser, RULE_MASK_FOR_LIST);
    if (parser->current.type == SHELL_TOKEN_IN) {
        parserScheduleRuleMask(parser, RULE_MASK_FOR_LIST);
        shellParserAdvance(parser);
        parseLinebreak(parser);
        while (parser->current.type == SHELL_TOKEN_WORD || parser->current.type == SHELL_TOKEN_ASSIGNMENT_WORD ||
               parser->current.type == SHELL_TOKEN_NAME || parser->current.type == SHELL_TOKEN_PARAMETER) {
            ShellWord *word = parseWordToken(parser, NULL);
            if (word) {
                populateWordExpansions(word);
                if (!parserWordArrayAppend(&value_words, word)) {
                    shellFreeWord(word);
                    parserWordArrayFree(&value_words);
                    shellFreeWord(name_word);
                    return NULL;
                }
            }
            parserScheduleRuleMask(parser, RULE_MASK_COMMAND_CONTINUATION);
            if (parser->current.type == SHELL_TOKEN_SEMICOLON || parser->current.type == SHELL_TOKEN_NEWLINE) {
                break;
            }
        }
    }

    if (parser->current.type == SHELL_TOKEN_SEMICOLON || parser->current.type == SHELL_TOKEN_NEWLINE) {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        parseLinebreak(parser);
    }

    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    shellParserConsume(parser, SHELL_TOKEN_DO, "Expected 'do' in for clause");
    parseLinebreak(parser);
    body = parseCompoundListUntil(parser, SHELL_TOKEN_DONE, SHELL_TOKEN_EOF, SHELL_TOKEN_EOF);
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    shellParserConsume(parser, SHELL_TOKEN_DONE, "Expected 'done' to close for clause");

    ShellLoop *loop = shellCreateForLoop(name_word, &value_words, body);
    if (!loop) {
        parserWordArrayFree(&value_words);
        shellFreeWord(name_word);
        return NULL;
    }
    ShellCommand *command = shellCreateLoopCommand(loop);
    if (command) {
        command->line = line;
        command->column = column;
    }
    return command;
}

static ShellCommand *parseCaseClause(ShellParser *parser) {
    int line = parser->current.line;
    int column = parser->current.column;
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);
    ShellWord *subject = parseWordToken(parser, "Expected word after 'case'");
    parseLinebreak(parser);
    parserScheduleRuleMask(parser, RULE_MASK_FOR_LIST);
    parserReclassifyCurrentToken(parser, RULE_MASK_FOR_LIST);
    shellParserConsume(parser, SHELL_TOKEN_IN, "Expected 'in' after case value");
    parseLinebreak(parser);

    ShellCase *case_stmt = shellCreateCase(subject);
    if (!case_stmt) {
        return NULL;
    }

    while (parser->current.type != SHELL_TOKEN_ESAC && parser->current.type != SHELL_TOKEN_EOF) {
        if (parser->current.type == SHELL_TOKEN_NEWLINE) {
            parseLinebreak(parser);
            continue;
        }
        ShellCaseClause *clause = shellCreateCaseClause(parser->current.line, parser->current.column);
        if (!clause) {
            shellFreeCase(case_stmt);
            return NULL;
        }
        parserScheduleRuleMask(parser, RULE_MASK_CASE_PATTERN);
        if (parser->current.type == SHELL_TOKEN_LPAREN) {
            shellParserAdvance(parser);
            parserScheduleRuleMask(parser, RULE_MASK_CASE_PATTERN);
        }
        while (parser->current.type == SHELL_TOKEN_WORD || parser->current.type == SHELL_TOKEN_NAME ||
               parser->current.type == SHELL_TOKEN_ASSIGNMENT_WORD || parser->current.type == SHELL_TOKEN_PARAMETER) {
            ShellWord *pattern = parseWordToken(parser, "Expected pattern");
            if (pattern) {
                populateWordExpansions(pattern);
                shellCaseClauseAddPattern(clause, pattern);
            }
            if (!shellParserMatch(parser, SHELL_TOKEN_PIPE)) {
                break;
            }
            parserScheduleRuleMask(parser, RULE_MASK_CASE_PATTERN);
        }
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserConsume(parser, SHELL_TOKEN_RPAREN, "Expected ')' after case pattern");
        parseLinebreak(parser);
        ShellProgram *body = parseCompoundListUntil(parser, SHELL_TOKEN_DSEMI, SHELL_TOKEN_ESAC, SHELL_TOKEN_EOF);
        clause->body = body;
        shellCaseAddClause(case_stmt, clause);
        if (parser->current.type == SHELL_TOKEN_DSEMI) {
            parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
            shellParserAdvance(parser);
            parseLinebreak(parser);
        } else {
            break;
        }
    }
    parserReclassifyCurrentToken(parser, RULE_MASK_COMMAND_START);
    shellParserConsume(parser, SHELL_TOKEN_ESAC, "Expected 'esac' to close case");

    ShellCommand *command = shellCreateCaseCommand(case_stmt);
    if (command) {
        command->line = line;
        command->column = column;
    }
    return command;
}

static bool parserIsFunctionDefinitionStart(ShellParser *parser) {
    if (!parser) {
        return false;
    }
    const ShellToken *token = &parser->current;
    bool is_name_token = (token->type == SHELL_TOKEN_NAME) ||
                         (token->type == SHELL_TOKEN_WORD && token->name_candidate && !token->reserved_candidate);
    if (!is_name_token) {
        return false;
    }

    ShellLexer lookahead = parser->lexer;
    shellLexerSetRuleMask(&lookahead, RULE_MASK_COMMAND_START);
    ShellToken next = shellNextToken(&lookahead);
    applyLexicalRules(&next);
    bool result = false;
    if (next.type == SHELL_TOKEN_LPAREN) {
        shellFreeToken(&next);
        shellLexerSetRuleMask(&lookahead, RULE_MASK_COMMAND_START);
        ShellToken closing = shellNextToken(&lookahead);
        applyLexicalRules(&closing);
        if (closing.type == SHELL_TOKEN_RPAREN) {
            result = true;
        }
        shellFreeToken(&closing);
    } else {
        shellFreeToken(&next);
    }
    return result;
}

static ShellCommand *parseFunctionDefinitionFromName(ShellParser *parser) {
    if (!parser) {
        return NULL;
    }
    int line = parser->current.line;
    int column = parser->current.column;
    const char *name_lexeme = parser->current.lexeme ? parser->current.lexeme : "";
    char *name_copy = strdup(name_lexeme);
    if (!name_copy) {
        parserErrorAt(parser, &parser->current, "Out of memory");
        return NULL;
    }

    parserScheduleRuleMask(parser, RULE_MASK_FUNCTION_NAME);
    parserReclassifyCurrentToken(parser, RULE_MASK_FUNCTION_NAME);
    shellParserAdvance(parser);

    if (parser->current.type == SHELL_TOKEN_LPAREN) {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserConsume(parser, SHELL_TOKEN_RPAREN, "Expected ')' after function name");
    } else {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    }
    parseLinebreak(parser);

    ShellCommand *body_command = parseCompoundCommand(parser);
    if (!body_command) {
        free(name_copy);
        return NULL;
    }

    ShellProgram *body_program = shellCreateProgram();
    shellProgramAddCommand(body_program, body_command);

    ShellFunction *function = shellCreateFunction(name_copy, "", body_program);
    free(name_copy);
    ShellCommand *command = shellCreateFunctionCommand(function);
    if (command) {
        command->line = line;
        command->column = column;
    }
    return command;
}

static ShellCommand *parseFunctionDefinition(ShellParser *parser) {
    int line = parser->current.line;
    int column = parser->current.column;
    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);

    parserScheduleRuleMask(parser, RULE_MASK_FUNCTION_NAME);
    parserReclassifyCurrentToken(parser, RULE_MASK_FUNCTION_NAME);
    shellParserAdvance(parser);
    if (parser->previous.type != SHELL_TOKEN_NAME) {
        parserErrorAt(parser, &parser->previous, "Expected function name");
        return NULL;
    }
    const char *name_lexeme = parser->previous.lexeme ? parser->previous.lexeme : "";
    char *name_copy = strdup(name_lexeme);
    if (!name_copy) {
        parserErrorAt(parser, &parser->previous, "Out of memory");
        return NULL;
    }

    if (parser->current.type == SHELL_TOKEN_LPAREN) {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserConsume(parser, SHELL_TOKEN_RPAREN, "Expected ')' after function name");
    } else {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    }
    parseLinebreak(parser);

    if (parser->had_error) {
        free(name_copy);
        return NULL;
    }

    ShellCommand *body_command = parseCompoundCommand(parser);
    if (!body_command) {
        free(name_copy);
        return NULL;
    }

    ShellProgram *body_program = shellCreateProgram();
    shellProgramAddCommand(body_program, body_command);

    ShellFunction *function = shellCreateFunction(name_copy, "", body_program);
    free(name_copy);
    ShellCommand *command = shellCreateFunctionCommand(function);
    if (command) {
        command->line = line;
        command->column = column;
    }
    return command;
}

static ShellWord *parseWordToken(ShellParser *parser, const char *context_message) {
    if (!parser) {
        return NULL;
    }
    if (parser->current.type != SHELL_TOKEN_WORD && parser->current.type != SHELL_TOKEN_ASSIGNMENT_WORD &&
        parser->current.type != SHELL_TOKEN_NAME && parser->current.type != SHELL_TOKEN_PARAMETER) {
        if (context_message) {
            parserErrorAt(parser, &parser->current, context_message);
        } else {
            parserErrorAt(parser, &parser->current, "Expected word");
        }
        return NULL;
    }
    ShellToken token = parser->current;
    unsigned int scheduled_mask = parser->next_rule_mask;
    unsigned int continuation_mask = RULE_MASK_COMMAND_CONTINUATION;
    if ((parser->current.rule_mask & SHELL_LEXER_RULE_4) != 0u || (scheduled_mask & SHELL_LEXER_RULE_4) != 0u) {
        continuation_mask = RULE_MASK_CASE_PATTERN;
    }
    parserScheduleRuleMask(parser, continuation_mask);
    shellParserAdvance(parser);
    ShellWord *word = shellCreateWord(token.lexeme ? token.lexeme : "", token.single_quoted, token.double_quoted,
                                      token.contains_parameter_expansion, token.contains_arithmetic_expansion,
                                      token.line, token.column);
    if (word) {
        if (token.type == SHELL_TOKEN_ASSIGNMENT_WORD) {
            word->is_assignment = true;
        }
        if (token.contains_command_substitution) {
            word->has_command_substitution = true;
        }
        if (token.type == SHELL_TOKEN_PARAMETER && token.lexeme && token.lexeme[0] == '$' && token.lexeme[1]) {
            shellWordAddExpansion(word, token.lexeme + 1);
        }
    }
    return word;
}

static ShellRedirection *parseRedirection(ShellParser *parser, bool *strip_tabs_out) {
    if (strip_tabs_out) {
        *strip_tabs_out = false;
    }
    if (!parser) {
        return NULL;
    }
    ShellToken number_token = parser->current;
    char *io_number_copy = NULL;
    if (number_token.type == SHELL_TOKEN_IO_NUMBER && number_token.lexeme) {
        io_number_copy = strdup(number_token.lexeme);
    }
    ShellTokenType redir_type = SHELL_TOKEN_ERROR;
    if (parser->current.type == SHELL_TOKEN_IO_NUMBER) {
        parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
        shellParserAdvance(parser);
        redir_type = parser->current.type;
    } else if (parser->current.type == SHELL_TOKEN_LT || parser->current.type == SHELL_TOKEN_GT ||
               parser->current.type == SHELL_TOKEN_DGREAT || parser->current.type == SHELL_TOKEN_DLESS ||
               parser->current.type == SHELL_TOKEN_DLESSDASH || parser->current.type == SHELL_TOKEN_TLESS ||
               parser->current.type == SHELL_TOKEN_GREATAND ||
               parser->current.type == SHELL_TOKEN_LESSAND || parser->current.type == SHELL_TOKEN_LESSGREAT ||
               parser->current.type == SHELL_TOKEN_CLOBBER) {
        redir_type = parser->current.type;
    } else {
        return NULL;
    }

    if (number_token.type != SHELL_TOKEN_IO_NUMBER) {
        number_token.lexeme = NULL;
    }

    bool strip_tabs = (redir_type == SHELL_TOKEN_DLESSDASH);
    if (strip_tabs_out) {
        *strip_tabs_out = strip_tabs;
    }

    parserScheduleRuleMask(parser, RULE_MASK_COMMAND_START);
    shellParserAdvance(parser);

    ShellRedirectionType type;
    switch (redir_type) {
        case SHELL_TOKEN_LT: type = SHELL_REDIRECT_INPUT; break;
        case SHELL_TOKEN_GT: type = SHELL_REDIRECT_OUTPUT; break;
        case SHELL_TOKEN_DGREAT: type = SHELL_REDIRECT_APPEND; break;
        case SHELL_TOKEN_DLESS:
        case SHELL_TOKEN_DLESSDASH: type = SHELL_REDIRECT_HEREDOC; break;
        case SHELL_TOKEN_TLESS: type = SHELL_REDIRECT_HERE_STRING; break;
        case SHELL_TOKEN_LESSAND: type = SHELL_REDIRECT_DUP_INPUT; break;
        case SHELL_TOKEN_GREATAND: type = SHELL_REDIRECT_DUP_OUTPUT; break;
        case SHELL_TOKEN_LESSGREAT: type = SHELL_REDIRECT_INPUT; break;
        case SHELL_TOKEN_CLOBBER: type = SHELL_REDIRECT_CLOBBER; break;
        default: return NULL;
    }

    parserScheduleRuleMask(parser,
                           type == SHELL_REDIRECT_HEREDOC ? RULE_MASK_HEREDOC_DELIMITER : RULE_MASK_REDIRECT_TARGET);
    ShellWord *target = parseWordToken(parser, "Expected redirection target");
    if (!target) {
        return NULL;
    }
    populateWordExpansions(target);

    const char *io_number_text = io_number_copy ? io_number_copy : number_token.lexeme;
    ShellRedirection *redir = shellCreateRedirection(type, io_number_text, target, number_token.line,
                                                    number_token.column);
    free(io_number_copy);
    if (redir && (type == SHELL_REDIRECT_DUP_INPUT || type == SHELL_REDIRECT_DUP_OUTPUT)) {
        char *dup_copy = parserCopyWordWithoutMarkers(target);
        const char *text = dup_copy ? dup_copy : (target && target->text ? target->text : "");
        if (text && text[0] == '&') {
            shellRedirectionSetDupTarget(redir, text + 1);
        } else {
            shellRedirectionSetDupTarget(redir, text);
        }
        free(dup_copy);
    }
    if (redir && type == SHELL_REDIRECT_HERE_STRING) {
        char *literal = parserCopyWordWithoutMarkers(target);
        shellRedirectionSetHereStringLiteral(redir, literal ? literal : "");
        free(literal);
    }
    return redir;
}

static bool parseDollarCommandSubstitution(const char *text, size_t start, size_t *out_span,
                                           char **out_command) {
    if (!text || start >= strlen(text) || text[start] != '$' || text[start + 1] != '(') {
        return false;
    }
    size_t i = start + 2;
    int depth = 1;
    while (text[i] && depth > 0) {
        if (text[i] == '(') {
            depth++;
        } else if (text[i] == ')') {
            depth--;
        }
        i++;
    }
    if (depth != 0) {
        return false;
    }
    size_t len = i - start;
    if (out_span) {
        *out_span = len;
    }
    if (out_command) {
        *out_command = normalizeDollarCommand(text + start + 2, len - 3);
    }
    return true;
}

static bool parseBacktickCommandSubstitution(const char *text, size_t start, size_t *out_span,
                                             char **out_command) {
    if (!text || text[start] != '`') {
        return false;
    }
    size_t i = start + 1;
    while (text[i]) {
        if (text[i] == '`') {
            break;
        }
        if (text[i] == '\\' && text[i + 1]) {
            i += 2;
            continue;
        }
        i++;
    }
    if (text[i] != '`') {
        return false;
    }
    if (out_span) {
        *out_span = i - start + 1;
    }
    if (out_command) {
        *out_command = normalizeBacktickCommand(text + start + 1, i - start - 1);
    }
    return true;
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
        if (c == SHELL_QUOTE_MARK_SINGLE) {
            out[j++] = '\'';
            continue;
        }
        if (c == SHELL_QUOTE_MARK_DOUBLE) {
            out[j++] = '"';
            continue;
        }
        if (c == '\\' && i + 1 < len && command[i + 1] == '\n') {
            i++;
            continue;
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
        if (c == SHELL_QUOTE_MARK_SINGLE) {
            out[j++] = '\'';
            continue;
        }
        if (c == SHELL_QUOTE_MARK_DOUBLE) {
            out[j++] = '"';
            continue;
        }
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
