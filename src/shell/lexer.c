#include "shell/lexer.h"
#include "core/utils.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *shellCopyRange(const char *src, size_t start, size_t end) {
    if (end <= start) {
        return strdup("");
    }
    size_t len = end - start;
    char *out = (char *)malloc(len + 1);
    if (!out) {
        fprintf(stderr, "shell lexer: allocation failed\n");
        return NULL;
    }
    memcpy(out, src + start, len);
    out[len] = '\0';
    return out;
}

static int peekChar(const ShellLexer *lexer) {
    if (!lexer || lexer->pos >= lexer->length) {
        return EOF;
    }
    return (unsigned char)lexer->src[lexer->pos];
}

static int advanceChar(ShellLexer *lexer) {
    if (!lexer || lexer->pos >= lexer->length) {
        return EOF;
    }
    unsigned char c = lexer->src[lexer->pos++];
    if (c == '\n') {
        lexer->line++;
        lexer->column = 1;
        lexer->at_line_start = true;
    } else {
        lexer->column++;
        lexer->at_line_start = false;
    }
    return c;
}

static void skipInlineWhitespace(ShellLexer *lexer) {
    while (true) {
        int c = peekChar(lexer);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            advanceChar(lexer);
            continue;
        }
        if (c == '#') {
            // Shell comments: skip until newline
            while (c != '\n' && c != EOF) {
                c = advanceChar(lexer);
            }
            continue;
        }
        break;
    }
}

static ShellToken makeSimpleToken(ShellLexer *lexer, ShellTokenType type, const char *lexeme, size_t len) {
    ShellToken tok;
    tok.type = type;
    tok.length = len;
    tok.single_quoted = false;
    tok.double_quoted = false;
    tok.contains_parameter_expansion = false;
    tok.contains_command_substitution = false;
    tok.contains_arithmetic_expansion = false;
    tok.line = lexer ? lexer->line : 1;
    tok.column = lexer ? lexer->column : 1;
    tok.lexeme = NULL;
    if (lexeme && len > 0) {
        tok.lexeme = shellCopyRange(lexeme, 0, len);
    } else if (lexeme) {
        tok.lexeme = strdup("");
    }
    return tok;
}

static ShellToken makeTokenFromRange(ShellLexer *lexer, ShellTokenType type, size_t start, size_t end,
                                     bool singleQuoted, bool doubleQuoted, bool hasParam, bool hasArithmetic) {
    ShellToken tok;
    tok.type = type;
    tok.length = (end > start) ? (end - start) : 0;
    tok.single_quoted = singleQuoted;
    tok.double_quoted = doubleQuoted;
    tok.contains_parameter_expansion = hasParam;
    tok.contains_command_substitution = false;
    tok.contains_arithmetic_expansion = hasArithmetic;
    tok.line = lexer ? lexer->line : 1;
    tok.column = lexer ? lexer->column : 1;
    tok.lexeme = (lexer && lexer->src) ? shellCopyRange(lexer->src, start, end) : NULL;
    return tok;
}

static ShellToken makeEOFToken(ShellLexer *lexer) {
    ShellToken tok;
    tok.type = SHELL_TOKEN_EOF;
    tok.lexeme = strdup("");
    tok.length = 0;
    tok.line = lexer ? lexer->line : 1;
    tok.column = lexer ? lexer->column : 1;
    tok.single_quoted = false;
    tok.double_quoted = false;
    tok.contains_parameter_expansion = false;
    tok.contains_command_substitution = false;
    tok.contains_arithmetic_expansion = false;
    return tok;
}

static ShellToken makeErrorToken(ShellLexer *lexer, const char *message) {
    ShellToken tok;
    tok.type = SHELL_TOKEN_ERROR;
    tok.lexeme = message ? strdup(message) : strdup("lexer error");
    tok.length = tok.lexeme ? strlen(tok.lexeme) : 0;
    tok.line = lexer ? lexer->line : 1;
    tok.column = lexer ? lexer->column : 1;
    tok.single_quoted = false;
    tok.double_quoted = false;
    tok.contains_parameter_expansion = false;
    tok.contains_command_substitution = false;
    tok.contains_arithmetic_expansion = false;
    return tok;
}

static ShellToken scanParameter(ShellLexer *lexer) {
    size_t start = lexer->pos;
    int first = advanceChar(lexer); // consume '$'
    (void)first;
    int c = peekChar(lexer);
    bool command_sub = false;
    bool arithmetic = false;
    if (c == '{') {
        advanceChar(lexer); // consume '{'
        while (true) {
            c = peekChar(lexer);
            if (c == EOF || c == '\n') {
                return makeErrorToken(lexer, "Unterminated parameter expansion");
            }
            if (c == '}') {
                advanceChar(lexer);
                break;
            }
            advanceChar(lexer);
        }
    } else if (c == '(') {
        advanceChar(lexer);
        if (peekChar(lexer) == '(') {
            arithmetic = true;
            advanceChar(lexer);
            int depth = 1;
            while (depth > 0) {
                c = peekChar(lexer);
                if (c == EOF) {
                    return makeErrorToken(lexer, "Unterminated arithmetic expansion");
                }
                if (c == '(') depth++;
                if (c == ')') depth--;
                advanceChar(lexer);
            }
            if (peekChar(lexer) == ')') {
                advanceChar(lexer);
            } else {
                return makeErrorToken(lexer, "Unterminated arithmetic expansion");
            }
        } else {
            command_sub = true;
            int depth = 1;
            while (depth > 0) {
                c = peekChar(lexer);
                if (c == EOF) {
                    return makeErrorToken(lexer, "Unterminated command substitution");
                }
                if (c == '(') depth++;
                if (c == ')') depth--;
                advanceChar(lexer);
            }
        }
    } else {
        while (isalnum(c) || c == '_' || c == '#') {
            advanceChar(lexer);
            c = peekChar(lexer);
        }
    }
    size_t end = lexer->pos;
    ShellToken tok = makeTokenFromRange(lexer, SHELL_TOKEN_PARAMETER, start, end, false, false, true, arithmetic);
    tok.contains_command_substitution = command_sub;
    return tok;
}

static bool isOperatorDelimiter(int c) {
    switch (c) {
        case '\n':
        case ';':
        case '&':
        case '|':
        case '(': case ')':
        case '{': case '}':
        case '<': case '>':
            return true;
        default:
            return false;
    }
}

static ShellToken scanWord(ShellLexer *lexer) {
    bool singleQuoted = false;
    bool doubleQuoted = false;
    bool sawSingleQuotedSegment = false;
    bool sawDoubleQuotedSegment = false;
    bool sawUnquotedSegment = false;
    bool hasParam = false;
    bool hasCommand = false;
    bool hasArithmetic = false;

    char *buffer = NULL;
    size_t bufLen = 0;
    size_t bufCap = 0;

    while (true) {
        int c = peekChar(lexer);
        if (c == EOF) {
            break;
        }
        if (!singleQuoted && !doubleQuoted && (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v')) {
            break;
        }
        if (!singleQuoted && !doubleQuoted && isOperatorDelimiter(c)) {
            break;
        }
        if (c == '\n' && !singleQuoted && !doubleQuoted) {
            break;
        }
        advanceChar(lexer);
        if (c == '\\') {
            int next = peekChar(lexer);
            if (next != EOF) {
                advanceChar(lexer);
                c = next;
            }
        } else if (c == '\'' && !doubleQuoted) {
            bool enteringSingle = !singleQuoted;
            singleQuoted = !singleQuoted;
            if (enteringSingle) {
                sawSingleQuotedSegment = true;
            }
            continue;
        } else if (c == '"' && !singleQuoted) {
            bool enteringDouble = !doubleQuoted;
            doubleQuoted = !doubleQuoted;
            if (enteringDouble) {
                sawDoubleQuotedSegment = true;
            }
            continue;
        } else if (c == '$' && !singleQuoted) {
            hasParam = true;
            if (bufLen + 2 >= bufCap) {
                bufCap = bufCap ? bufCap * 2 : 32;
                char *tmp = (char *)realloc(buffer, bufCap);
                if (!tmp) {
                    free(buffer);
                    return makeErrorToken(lexer, "Out of memory while scanning word");
                }
                buffer = tmp;
            }
            buffer[bufLen++] = (char)c;
            int next = peekChar(lexer);
            if (next == '{' || next == '(') {
                if (next == '(') {
                    int after = EOF;
                    if (lexer->pos + 1 < lexer->length) {
                        after = (unsigned char)lexer->src[lexer->pos + 1];
                    }
                    if (after == '(') {
                        hasArithmetic = true;
                        buffer[bufLen++] = (char)advanceChar(lexer);
                        buffer[bufLen++] = (char)advanceChar(lexer);
                        int depth = 1;
                        while (depth > 0) {
                            int inner = peekChar(lexer);
                            if (inner == EOF) {
                                break;
                            }
                            buffer[bufLen++] = (char)advanceChar(lexer);
                            if (inner == '(') depth++;
                            else if (inner == ')') depth--;
                            if (bufLen + 1 >= bufCap) {
                                bufCap = bufCap ? bufCap * 2 : 32;
                                char *tmp2 = (char *)realloc(buffer, bufCap);
                                if (!tmp2) {
                                    free(buffer);
                                    return makeErrorToken(lexer, "Out of memory while scanning word");
                                }
                                buffer = tmp2;
                            }
                        }
                        if (peekChar(lexer) == ')') {
                            buffer[bufLen++] = (char)advanceChar(lexer);
                        }
                        continue;
                    }
                    hasCommand = true;
                }
                buffer[bufLen++] = (char)advanceChar(lexer);
                int depth = 1;
                while (depth > 0) {
                    int inner = peekChar(lexer);
                    if (inner == EOF) {
                        break;
                    }
                    buffer[bufLen++] = (char)advanceChar(lexer);
                    if (inner == '{' || inner == '(') depth++;
                    else if (inner == '}' || inner == ')') depth--;
                    if (bufLen + 1 >= bufCap) {
                        bufCap = bufCap ? bufCap * 2 : 32;
                        char *tmp2 = (char *)realloc(buffer, bufCap);
                        if (!tmp2) {
                            free(buffer);
                            return makeErrorToken(lexer, "Out of memory while scanning word");
                        }
                        buffer = tmp2;
                    }
                }
                continue;
            } else {
                while (isalnum(next) || next == '_' || next == '#') {
                    buffer[bufLen++] = (char)advanceChar(lexer);
                    if (bufLen + 1 >= bufCap) {
                        bufCap = bufCap ? bufCap * 2 : 32;
                        char *tmp3 = (char *)realloc(buffer, bufCap);
                        if (!tmp3) {
                            free(buffer);
                            return makeErrorToken(lexer, "Out of memory while scanning word");
                        }
                        buffer = tmp3;
                    }
                    next = peekChar(lexer);
                }
                continue;
            }
        }
        if (c == '`' && !singleQuoted) {
            hasCommand = true;
        }

        if (bufLen + 1 >= bufCap) {
            bufCap = bufCap ? bufCap * 2 : 32;
            char *tmp = (char *)realloc(buffer, bufCap);
            if (!tmp) {
                free(buffer);
                return makeErrorToken(lexer, "Out of memory while scanning word");
            }
            buffer = tmp;
        }
        buffer[bufLen++] = (char)c;
        if (singleQuoted) {
            sawSingleQuotedSegment = true;
        } else if (doubleQuoted) {
            sawDoubleQuotedSegment = true;
        } else {
            sawUnquotedSegment = true;
        }
    }

    if (buffer && bufLen < bufCap) {
        buffer[bufLen] = '\0';
    } else if (buffer) {
        char *tmp = (char *)realloc(buffer, bufLen + 1);
        if (!tmp) {
            free(buffer);
            return makeErrorToken(lexer, "Out of memory finalizing word");
        }
        buffer = tmp;
        buffer[bufLen] = '\0';
    }

    ShellToken tok;
    tok.type = SHELL_TOKEN_WORD;
    tok.length = bufLen;
    tok.lexeme = buffer ? buffer : strdup("");
    tok.line = lexer->line;
    tok.column = lexer->column;
    tok.single_quoted = sawSingleQuotedSegment && !sawDoubleQuotedSegment && !sawUnquotedSegment;
    tok.double_quoted = sawDoubleQuotedSegment && !sawSingleQuotedSegment && !sawUnquotedSegment;
    tok.contains_parameter_expansion = hasParam;
    tok.contains_command_substitution = hasCommand;
    tok.contains_arithmetic_expansion = hasArithmetic;
    return tok;
}

void shellInitLexer(ShellLexer *lexer, const char *source) {
    if (!lexer) {
        return;
    }
    lexer->src = source ? source : "";
    lexer->length = source ? strlen(source) : 0;
    lexer->pos = 0;
    lexer->line = 1;
    lexer->column = 1;
    lexer->at_line_start = true;
}

void shellFreeToken(ShellToken *token) {
    if (!token) {
        return;
    }
    if (token->lexeme) {
        free(token->lexeme);
        token->lexeme = NULL;
    }
}

static ShellTokenType checkReservedWord(const char *lexeme) {
    if (!lexeme) {
        return SHELL_TOKEN_WORD;
    }
    if (strcmp(lexeme, "function") == 0) return SHELL_TOKEN_FUNCTION;
    if (strcmp(lexeme, "if") == 0) return SHELL_TOKEN_IF;
    if (strcmp(lexeme, "then") == 0) return SHELL_TOKEN_THEN;
    if (strcmp(lexeme, "elif") == 0) return SHELL_TOKEN_ELIF;
    if (strcmp(lexeme, "else") == 0) return SHELL_TOKEN_ELSE;
    if (strcmp(lexeme, "fi") == 0) return SHELL_TOKEN_FI;
    if (strcmp(lexeme, "for") == 0) return SHELL_TOKEN_FOR;
    if (strcmp(lexeme, "while") == 0) return SHELL_TOKEN_WHILE;
    if (strcmp(lexeme, "until") == 0) return SHELL_TOKEN_UNTIL;
    if (strcmp(lexeme, "do") == 0) return SHELL_TOKEN_DO;
    if (strcmp(lexeme, "done") == 0) return SHELL_TOKEN_DONE;
    if (strcmp(lexeme, "in") == 0) return SHELL_TOKEN_IN;
    if (strcmp(lexeme, "case") == 0) return SHELL_TOKEN_CASE;
    if (strcmp(lexeme, "esac") == 0) return SHELL_TOKEN_ESAC;
    return SHELL_TOKEN_WORD;
}

ShellToken shellNextToken(ShellLexer *lexer) {
    if (!lexer) {
        return makeEOFToken(NULL);
    }

    while (true) {
        int c = peekChar(lexer);
        if (c == EOF) {
            return makeEOFToken(lexer);
        }
        if (c == '\n') {
            advanceChar(lexer);
            ShellToken tok = makeSimpleToken(lexer, SHELL_TOKEN_NEWLINE, "\n", 1);
            return tok;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            advanceChar(lexer);
            continue;
        }
        if (c == '#') {
            // skip comment until newline
            while (c != '\n' && c != EOF) {
                c = advanceChar(lexer);
            }
            continue;
        }
        break;
    }

    skipInlineWhitespace(lexer);

    int c = peekChar(lexer);
    if (c == EOF) {
        return makeEOFToken(lexer);
    }

    if (isdigit(c)) {
        size_t start = lexer->pos;
        while (isdigit(peekChar(lexer))) {
            advanceChar(lexer);
        }
        int next = peekChar(lexer);
        if (next == '<' || next == '>') {
            size_t end = lexer->pos;
            ShellToken tok = makeTokenFromRange(lexer, SHELL_TOKEN_IO_NUMBER, start, end, false, false, false, false);
            return tok;
        }
        lexer->pos = start;
        lexer->column -= (int)(lexer->pos - start);
    }

    switch (c) {
        case ';': {
            advanceChar(lexer);
            if (peekChar(lexer) == ';') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_DSEMI, ";;", 2);
            }
            return makeSimpleToken(lexer, SHELL_TOKEN_SEMICOLON, ";", 1);
        }
        case '&': {
            advanceChar(lexer);
            if (peekChar(lexer) == '&') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_AND_AND, "&&", 2);
            }
            return makeSimpleToken(lexer, SHELL_TOKEN_AMPERSAND, "&", 1);
        }
        case '|': {
            advanceChar(lexer);
            int next = peekChar(lexer);
            if (next == '|') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_OR_OR, "||", 2);
            }
            if (next == '&') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_PIPE_AMP, "|&", 2);
            }
            return makeSimpleToken(lexer, SHELL_TOKEN_PIPE, "|", 1);
        }
        case '(': {
            advanceChar(lexer);
            return makeSimpleToken(lexer, SHELL_TOKEN_LPAREN, "(", 1);
        }
        case ')': {
            advanceChar(lexer);
            return makeSimpleToken(lexer, SHELL_TOKEN_RPAREN, ")", 1);
        }
        case '{': {
            advanceChar(lexer);
            return makeSimpleToken(lexer, SHELL_TOKEN_LBRACE, "{", 1);
        }
        case '}': {
            advanceChar(lexer);
            return makeSimpleToken(lexer, SHELL_TOKEN_RBRACE, "}", 1);
        }
        case '<': {
            advanceChar(lexer);
            int next = peekChar(lexer);
            if (next == '<') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_LT_LT, "<<", 2);
            }
            if (next == '>') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_LT_GT, "<>", 2);
            }
            if (next == '&') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_LT_AND, "<&", 2);
            }
            return makeSimpleToken(lexer, SHELL_TOKEN_LT, "<", 1);
        }
        case '>': {
            advanceChar(lexer);
            int next = peekChar(lexer);
            if (next == '>') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_GT_GT, ">>", 2);
            }
            if (next == '&') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_GT_AND, ">&", 2);
            }
            if (next == '|') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_CLOBBER, ">|", 2);
            }
            return makeSimpleToken(lexer, SHELL_TOKEN_GT, ">", 1);
        }
        case '$':
            return scanParameter(lexer);
        default:
            break;
    }

    ShellToken word = scanWord(lexer);
    if (!word.lexeme) {
        return makeErrorToken(lexer, "Failed to allocate word");
    }
    ShellTokenType reserved = checkReservedWord(word.lexeme);
    if (reserved != SHELL_TOKEN_WORD) {
        word.type = reserved;
    } else if (!word.single_quoted && !word.double_quoted) {
        const char *eq = strchr(word.lexeme, '=');
        if (eq && eq != word.lexeme) {
            word.type = SHELL_TOKEN_ASSIGNMENT;
        }
    }
    return word;
}

const char *shellTokenTypeName(ShellTokenType type) {
    switch (type) {
        case SHELL_TOKEN_WORD: return "WORD";
        case SHELL_TOKEN_PARAMETER: return "PARAM";
        case SHELL_TOKEN_ASSIGNMENT: return "ASSIGN";
        case SHELL_TOKEN_IO_NUMBER: return "IO_NUMBER";
        case SHELL_TOKEN_NEWLINE: return "NEWLINE";
        case SHELL_TOKEN_SEMICOLON: return "SEMICOLON";
        case SHELL_TOKEN_AMPERSAND: return "AMPERSAND";
        case SHELL_TOKEN_PIPE: return "PIPE";
        case SHELL_TOKEN_PIPE_AMP: return "PIPE_AMP";
        case SHELL_TOKEN_AND_AND: return "AND_AND";
        case SHELL_TOKEN_OR_OR: return "OR_OR";
        case SHELL_TOKEN_LPAREN: return "LPAREN";
        case SHELL_TOKEN_RPAREN: return "RPAREN";
        case SHELL_TOKEN_LBRACE: return "LBRACE";
        case SHELL_TOKEN_RBRACE: return "RBRACE";
        case SHELL_TOKEN_FUNCTION: return "FUNCTION";
        case SHELL_TOKEN_IF: return "IF";
        case SHELL_TOKEN_THEN: return "THEN";
        case SHELL_TOKEN_ELIF: return "ELIF";
        case SHELL_TOKEN_ELSE: return "ELSE";
        case SHELL_TOKEN_FI: return "FI";
        case SHELL_TOKEN_FOR: return "FOR";
        case SHELL_TOKEN_WHILE: return "WHILE";
        case SHELL_TOKEN_UNTIL: return "UNTIL";
        case SHELL_TOKEN_DO: return "DO";
        case SHELL_TOKEN_DONE: return "DONE";
        case SHELL_TOKEN_IN: return "IN";
        case SHELL_TOKEN_CASE: return "CASE";
        case SHELL_TOKEN_ESAC: return "ESAC";
        case SHELL_TOKEN_DSEMI: return "DSEMI";
        case SHELL_TOKEN_LT: return "LT";
        case SHELL_TOKEN_GT: return "GT";
        case SHELL_TOKEN_GT_GT: return "GT_GT";
        case SHELL_TOKEN_LT_LT: return "LT_LT";
        case SHELL_TOKEN_LT_GT: return "LT_GT";
        case SHELL_TOKEN_GT_AND: return "GT_AND";
        case SHELL_TOKEN_LT_AND: return "LT_AND";
        case SHELL_TOKEN_CLOBBER: return "CLOBBER";
        case SHELL_TOKEN_COMMENT: return "COMMENT";
        case SHELL_TOKEN_EOF: return "EOF";
        case SHELL_TOKEN_ERROR: return "ERROR";
    }
    return "UNKNOWN";
}
