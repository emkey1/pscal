#include "shell/lexer.h"
#include "core/utils.h"
#include "shell/quote_markers.h"
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

static void skipCommentToNewline(ShellLexer *lexer) {
    if (!lexer) {
        return;
    }
    // The caller peeked '#' but has not consumed it yet.
    advanceChar(lexer); // consume '#'
    while (true) {
        int next = peekChar(lexer);
        if (next == '\n' || next == EOF) {
            break;
        }
        advanceChar(lexer);
    }
}

static void skipInlineWhitespace(ShellLexer *lexer) {
    while (true) {
        int c = peekChar(lexer);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            advanceChar(lexer);
            continue;
        }
        if (c == '#') {
            // Shell comments: skip until newline but preserve the newline itself.
            skipCommentToNewline(lexer);
            continue;
        }
        break;
    }
}

static bool isValidNameLexeme(const char *lexeme, size_t length) {
    if (!lexeme || length == 0) {
        return false;
    }
    unsigned char first = (unsigned char)lexeme[0];
    if (!(isalpha(first) || first == '_')) {
        return false;
    }
    const unsigned char backslash = 92;
    const unsigned char single_quote = 39;
    const unsigned char double_quote = 34;
    bool in_brackets = false;
    bool in_single = false;
    bool in_double = false;
    for (size_t i = 1; i < length; ++i) {
        unsigned char ch = (unsigned char)lexeme[i];
        if (in_single) {
            if (ch == backslash && i + 1 < length) {
                ++i;
                continue;
            }
            if (ch == single_quote) {
                in_single = false;
            }
            continue;
        }
        if (in_double) {
            if (ch == backslash && i + 1 < length) {
                ++i;
                continue;
            }
            if (ch == double_quote) {
                in_double = false;
            }
            continue;
        }
        if (in_brackets) {
            if (ch == backslash && i + 1 < length) {
                ++i;
                continue;
            }
            if (ch == single_quote) {
                in_single = true;
                continue;
            }
            if (ch == double_quote) {
                in_double = true;
                continue;
            }
            if (ch == 93) { /* ']' */
                in_brackets = false;
                continue;
            }
            if (ch == 91) { /* '[' */
                return false;
            }
            continue;
        }
        if (ch == 91) {
            in_brackets = true;
            continue;
        }
        if (!(isalnum(ch) || ch == '_')) {
            return false;
        }
    }
    return !in_brackets && !in_single && !in_double;
}



static ShellTokenType checkReservedWord(const char *lexeme);

static ShellToken makeSimpleToken(ShellLexer *lexer, ShellTokenType type, const char *lexeme, size_t len) {
    ShellToken tok;
    tok.type = type;
    tok.base_type = type;
    tok.reserved_type = type;
    tok.length = len;
    tok.single_quoted = false;
    tok.double_quoted = false;
    tok.contains_parameter_expansion = false;
    tok.contains_command_substitution = false;
    tok.contains_arithmetic_expansion = false;
    tok.reserved_candidate = false;
    tok.assignment_candidate = false;
    tok.name_candidate = false;
    tok.rule_mask = lexer ? lexer->rule_mask : 0u;
    tok.command_starts = lexer ? (lexer->rule_mask & SHELL_LEXER_RULE_1) != 0 : false;
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
    tok.base_type = type;
    tok.reserved_type = type;
    tok.length = (end > start) ? (end - start) : 0;
    tok.single_quoted = singleQuoted;
    tok.double_quoted = doubleQuoted;
    tok.contains_parameter_expansion = hasParam;
    tok.contains_command_substitution = false;
    tok.contains_arithmetic_expansion = hasArithmetic;
    tok.reserved_candidate = false;
    tok.assignment_candidate = false;
    tok.name_candidate = false;
    tok.rule_mask = lexer ? lexer->rule_mask : 0u;
    tok.command_starts = lexer ? (lexer->rule_mask & SHELL_LEXER_RULE_1) != 0 : false;
    tok.line = lexer ? lexer->line : 1;
    tok.column = lexer ? lexer->column : 1;
    tok.lexeme = (lexer && lexer->src) ? shellCopyRange(lexer->src, start, end) : NULL;
    return tok;
}

static ShellToken makeEOFToken(ShellLexer *lexer) {
    ShellToken tok;
    tok.type = SHELL_TOKEN_EOF;
    tok.base_type = SHELL_TOKEN_EOF;
    tok.reserved_type = SHELL_TOKEN_EOF;
    tok.lexeme = strdup("");
    tok.length = 0;
    tok.line = lexer ? lexer->line : 1;
    tok.column = lexer ? lexer->column : 1;
    tok.single_quoted = false;
    tok.double_quoted = false;
    tok.contains_parameter_expansion = false;
    tok.contains_command_substitution = false;
    tok.contains_arithmetic_expansion = false;
    tok.reserved_candidate = false;
    tok.assignment_candidate = false;
    tok.name_candidate = false;
    tok.rule_mask = lexer ? lexer->rule_mask : 0u;
    tok.command_starts = false;
    return tok;
}

static ShellToken makeErrorToken(ShellLexer *lexer, const char *message) {
    ShellToken tok;
    tok.type = SHELL_TOKEN_ERROR;
    tok.base_type = SHELL_TOKEN_ERROR;
    tok.reserved_type = SHELL_TOKEN_ERROR;
    tok.lexeme = message ? strdup(message) : strdup("lexer error");
    tok.length = tok.lexeme ? strlen(tok.lexeme) : 0;
    tok.line = lexer ? lexer->line : 1;
    tok.column = lexer ? lexer->column : 1;
    tok.single_quoted = false;
    tok.double_quoted = false;
    tok.contains_parameter_expansion = false;
    tok.contains_command_substitution = false;
    tok.contains_arithmetic_expansion = false;
    tok.reserved_candidate = false;
    tok.assignment_candidate = false;
    tok.name_candidate = false;
    tok.rule_mask = lexer ? lexer->rule_mask : 0u;
    tok.command_starts = lexer ? (lexer->rule_mask & SHELL_LEXER_RULE_1) != 0 : false;
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
        if (c == '?' || c == '@' || c == '*' || c == '!' || c == '-' || c == '$') {
            advanceChar(lexer);
        } else {
            while (isalnum(c) || c == '_' || c == '#') {
                advanceChar(lexer);
                c = peekChar(lexer);
            }
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

static bool isStructuralWordCandidate(int c) {
    switch (c) {
        case '(':
        case ')':
        case '{':
        case '}':
            return true;
        default:
            return false;
    }
}

static bool lexerAllowsStructuralWordLiterals(const ShellLexer *lexer) {
    if (!lexer) {
        return false;
    }

    unsigned int mask = lexer->rule_mask;
    if ((mask & SHELL_LEXER_RULE_4) != 0) {
        // Case patterns rely on ')' remaining a structural token even though
        // they are parsed outside command-start contexts.
        return false;
    }

    return true;
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

    bool allowStructuralLiterals = lexerAllowsStructuralWordLiterals(lexer);

    bool inArrayLiteral = false;
    int arrayParenDepth = 0;

    int eqSuppressDepth = 0;
    size_t firstUnquotedEq = (size_t)-1;

    char *buffer = NULL;
    size_t bufLen = 0;
    size_t bufCap = 0;

    while (true) {
        int c = peekChar(lexer);
        if (c == EOF) {
            break;
        }
        bool startingArrayLiteral = false;
        if (!singleQuoted && !doubleQuoted) {
            if (!inArrayLiteral && firstUnquotedEq != (size_t)-1 && eqSuppressDepth == 0 && c == '(' &&
                bufLen == firstUnquotedEq + 1) {
                startingArrayLiteral = true;
            }

            bool allowArrayWhitespace = inArrayLiteral && arrayParenDepth > 0;
            if (!allowArrayWhitespace) {
                if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                    break;
                }
                if (c == '\n') {
                    break;
                }
            }

            if (!(startingArrayLiteral || (inArrayLiteral && arrayParenDepth > 0 && (c == '(' || c == ')')))) {
                bool treat_as_operator = isOperatorDelimiter(c);
                if (treat_as_operator && inArrayLiteral && arrayParenDepth > 0 && c == '\n') {
                    treat_as_operator = false;
                }
                if (treat_as_operator && isStructuralWordCandidate(c)) {
                    if (allowStructuralLiterals && (lexer->rule_mask & SHELL_LEXER_RULE_1) == 0) {
                        treat_as_operator = false;
                    }
                }
                if (treat_as_operator) {
                    break;
                }
            }
        }
        advanceChar(lexer);
        if (!singleQuoted && !doubleQuoted && eqSuppressDepth == 0 && firstUnquotedEq != (size_t)-1) {
            if (startingArrayLiteral) {
                inArrayLiteral = true;
                arrayParenDepth = 1;
            } else if (inArrayLiteral) {
                if (c == '(') {
                    arrayParenDepth++;
                } else if (c == ')') {
                    if (arrayParenDepth > 0) {
                        arrayParenDepth--;
                        if (arrayParenDepth == 0) {
                            inArrayLiteral = false;
                        }
                    }
                }
            }
        }
        if (c == '\\') {
            int next = peekChar(lexer);
            if (singleQuoted || next == EOF) {
                c = '\\';
            } else if (!doubleQuoted) {
                if (next == '\n') {
                    advanceChar(lexer);
                    continue;
                }
                advanceChar(lexer);
                c = next;
            } else {
                if (next == '\n') {
                    advanceChar(lexer);
                    continue;
                }
                if (next == '\\' || next == '"' || next == '$' || next == '`') {
                    advanceChar(lexer);
                    c = next;
                } else {
                    c = '\\';
                }
            }
        } else if (c == '\'' && !doubleQuoted) {
            bool enteringSingle = !singleQuoted;
            singleQuoted = !singleQuoted;
            if (bufLen + 1 >= bufCap) {
                bufCap = bufCap ? bufCap * 2 : 32;
                char *tmp = (char *)realloc(buffer, bufCap);
                if (!tmp) {
                    free(buffer);
                    return makeErrorToken(lexer, "Out of memory while scanning word");
                }
                buffer = tmp;
            }
            buffer[bufLen++] = SHELL_QUOTE_MARK_SINGLE;
            if (enteringSingle) {
                sawSingleQuotedSegment = true;
            }
            continue;
        } else if (c == '"' && !singleQuoted) {
            bool enteringDouble = !doubleQuoted;
            doubleQuoted = !doubleQuoted;
            if (bufLen + 1 >= bufCap) {
                bufCap = bufCap ? bufCap * 2 : 32;
                char *tmp = (char *)realloc(buffer, bufCap);
                if (!tmp) {
                    free(buffer);
                    return makeErrorToken(lexer, "Out of memory while scanning word");
                }
                buffer = tmp;
            }
            buffer[bufLen++] = SHELL_QUOTE_MARK_DOUBLE;
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
                        eqSuppressDepth++;
                        int depth = 1;
                        while (depth > 0) {
                            int inner = peekChar(lexer);
                            if (inner == EOF) {
                                break;
                            }
                            if (bufLen + 1 >= bufCap) {
                                bufCap = bufCap ? bufCap * 2 : 32;
                                char *tmp2 = (char *)realloc(buffer, bufCap);
                                if (!tmp2) {
                                    free(buffer);
                                    return makeErrorToken(lexer, "Out of memory while scanning word");
                                }
                                buffer = tmp2;
                            }
                            buffer[bufLen++] = (char)advanceChar(lexer);
                            if (inner == '(') depth++;
                            else if (inner == ')') depth--;
                        }
                        if (peekChar(lexer) == ')') {
                            if (bufLen + 1 >= bufCap) {
                                bufCap = bufCap ? bufCap * 2 : 32;
                                char *tmp3 = (char *)realloc(buffer, bufCap);
                                if (!tmp3) {
                                    free(buffer);
                                    return makeErrorToken(lexer, "Out of memory while scanning word");
                                }
                                buffer = tmp3;
                            }
                            buffer[bufLen++] = (char)advanceChar(lexer);
                        }
                        eqSuppressDepth--;
                        continue;
                    }
                    hasCommand = true;
                }
                buffer[bufLen++] = (char)advanceChar(lexer);
                eqSuppressDepth++;
                int depth = 1;
                while (depth > 0) {
                    int inner = peekChar(lexer);
                    if (inner == EOF) {
                        break;
                    }
                    if (bufLen + 1 >= bufCap) {
                        bufCap = bufCap ? bufCap * 2 : 32;
                        char *tmp2 = (char *)realloc(buffer, bufCap);
                        if (!tmp2) {
                            free(buffer);
                            return makeErrorToken(lexer, "Out of memory while scanning word");
                        }
                        buffer = tmp2;
                    }
                    buffer[bufLen++] = (char)advanceChar(lexer);
                    if (inner == '{' || inner == '(') depth++;
                    else if (inner == '}' || inner == ')') depth--;
                }
                eqSuppressDepth--;
                continue;
            } else {
                if (next == '?' || next == '@' || next == '*' || next == '!' || next == '-' || next == '$') {
                    if (bufLen + 1 >= bufCap) {
                        bufCap = bufCap ? bufCap * 2 : 32;
                        char *tmp3 = (char *)realloc(buffer, bufCap);
                        if (!tmp3) {
                            free(buffer);
                            return makeErrorToken(lexer, "Out of memory while scanning word");
                        }
                        buffer = tmp3;
                    }
                    buffer[bufLen++] = (char)advanceChar(lexer);
                } else {
                    while (isalnum(next) || next == '_' || next == '#') {
                        if (bufLen + 1 >= bufCap) {
                            bufCap = bufCap ? bufCap * 2 : 32;
                            char *tmp3 = (char *)realloc(buffer, bufCap);
                            if (!tmp3) {
                                free(buffer);
                                return makeErrorToken(lexer, "Out of memory while scanning word");
                            }
                            buffer = tmp3;
                        }
                        buffer[bufLen++] = (char)advanceChar(lexer);
                        next = peekChar(lexer);
                    }
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
        if (firstUnquotedEq == (size_t)-1 && c == '=' && !singleQuoted && !doubleQuoted && eqSuppressDepth == 0) {
            firstUnquotedEq = bufLen - 1;
        }
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
    tok.base_type = SHELL_TOKEN_WORD;
    tok.reserved_type = SHELL_TOKEN_WORD;
    tok.length = bufLen;
    tok.lexeme = buffer ? buffer : strdup("");
    tok.line = lexer->line;
    tok.column = lexer->column;
    tok.single_quoted = sawSingleQuotedSegment && !sawDoubleQuotedSegment && !sawUnquotedSegment;
    tok.double_quoted = sawDoubleQuotedSegment && !sawSingleQuotedSegment && !sawUnquotedSegment;
    tok.contains_parameter_expansion = hasParam;
    tok.contains_command_substitution = hasCommand;
    tok.contains_arithmetic_expansion = hasArithmetic;
    tok.reserved_candidate = false;
    tok.assignment_candidate = false;
    tok.name_candidate = false;
    tok.rule_mask = lexer->rule_mask;
    tok.command_starts = (lexer->rule_mask & SHELL_LEXER_RULE_1) != 0;

    ShellTokenType reserved = SHELL_TOKEN_WORD;
    if (tok.lexeme) {
        reserved = checkReservedWord(tok.lexeme);
    }
    if (reserved != SHELL_TOKEN_WORD) {
        tok.reserved_candidate = true;
        tok.reserved_type = reserved;
        tok.type = reserved;
    }

    if (firstUnquotedEq != (size_t)-1 && firstUnquotedEq > 0 && tok.lexeme && firstUnquotedEq < tok.length) {
        if (isValidNameLexeme(tok.lexeme, firstUnquotedEq)) {
            tok.assignment_candidate = true;
            if (!tok.reserved_candidate) {
                tok.type = SHELL_TOKEN_ASSIGNMENT_WORD;
            }
        }
    }

    if (!tok.assignment_candidate && reserved == SHELL_TOKEN_WORD && tok.lexeme && firstUnquotedEq == (size_t)-1) {
        if (isValidNameLexeme(tok.lexeme, tok.length)) {
            tok.name_candidate = true;
        }
    }

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
    lexer->rule_mask = SHELL_LEXER_RULE_1;
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
            // Skip comment but leave newline for subsequent handling.
            skipCommentToNewline(lexer);
            continue;
        }
        break;
    }

    skipInlineWhitespace(lexer);

    int c = peekChar(lexer);
    if (c == EOF) {
        return makeEOFToken(lexer);
    }

    bool command_starts = (lexer->rule_mask & SHELL_LEXER_RULE_1) != 0;
    if (!command_starts && lexerAllowsStructuralWordLiterals(lexer) && isStructuralWordCandidate(c)) {
        ShellToken word = scanWord(lexer);
        if (!word.lexeme) {
            return makeErrorToken(lexer, "Failed to allocate word");
        }
        return word;
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
        case '!': {
            if ((lexer->rule_mask & SHELL_LEXER_RULE_1) != 0) {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_BANG, "!", 1);
            }
            break;
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
            if (peekChar(lexer) == '(') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_DLPAREN, "((", 2);
            }
            return makeSimpleToken(lexer, SHELL_TOKEN_LPAREN, "(", 1);
        }
        case ')': {
            advanceChar(lexer);
            if (peekChar(lexer) == ')') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_DRPAREN, "))", 2);
            }
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
                int third = peekChar(lexer);
                if (third == '<') {
                    advanceChar(lexer);
                    return makeSimpleToken(lexer, SHELL_TOKEN_TLESS, "<<<", 3);
                }
                if (third == '-') {
                    advanceChar(lexer);
                    return makeSimpleToken(lexer, SHELL_TOKEN_DLESSDASH, "<<-", 3);
                }
                return makeSimpleToken(lexer, SHELL_TOKEN_DLESS, "<<", 2);
            }
            if (next == '>') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_LESSGREAT, "<>", 2);
            }
            if (next == '&') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_LESSAND, "<&", 2);
            }
            return makeSimpleToken(lexer, SHELL_TOKEN_LT, "<", 1);
        }
        case '>': {
            advanceChar(lexer);
            int next = peekChar(lexer);
            if (next == '>') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_DGREAT, ">>", 2);
            }
            if (next == '&') {
                advanceChar(lexer);
                return makeSimpleToken(lexer, SHELL_TOKEN_GREATAND, ">&", 2);
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
    return word;
}

const char *shellTokenTypeName(ShellTokenType type) {
    switch (type) {
        case SHELL_TOKEN_WORD: return "WORD";
        case SHELL_TOKEN_NAME: return "NAME";
        case SHELL_TOKEN_ASSIGNMENT_WORD: return "ASSIGNMENT_WORD";
        case SHELL_TOKEN_PARAMETER: return "PARAM";
        case SHELL_TOKEN_IO_NUMBER: return "IO_NUMBER";
        case SHELL_TOKEN_NEWLINE: return "NEWLINE";
        case SHELL_TOKEN_SEMICOLON: return "SEMICOLON";
        case SHELL_TOKEN_AMPERSAND: return "AMPERSAND";
        case SHELL_TOKEN_BANG: return "BANG";
        case SHELL_TOKEN_PIPE: return "PIPE";
        case SHELL_TOKEN_PIPE_AMP: return "PIPE_AMP";
        case SHELL_TOKEN_AND_AND: return "AND_AND";
        case SHELL_TOKEN_OR_OR: return "OR_OR";
        case SHELL_TOKEN_LPAREN: return "LPAREN";
        case SHELL_TOKEN_RPAREN: return "RPAREN";
        case SHELL_TOKEN_DLPAREN: return "DLPAREN";
        case SHELL_TOKEN_DRPAREN: return "DRPAREN";
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
        case SHELL_TOKEN_DGREAT: return "DGREAT";
        case SHELL_TOKEN_DLESS: return "DLESS";
        case SHELL_TOKEN_DLESSDASH: return "DLESSDASH";
        case SHELL_TOKEN_TLESS: return "TLESS";
        case SHELL_TOKEN_LESSGREAT: return "LESSGREAT";
        case SHELL_TOKEN_GREATAND: return "GREATAND";
        case SHELL_TOKEN_LESSAND: return "LESSAND";
        case SHELL_TOKEN_CLOBBER: return "CLOBBER";
        case SHELL_TOKEN_COMMENT: return "COMMENT";
        case SHELL_TOKEN_EOF: return "EOF";
        case SHELL_TOKEN_ERROR: return "ERROR";
    }
    return "UNKNOWN";
}

void shellLexerSetRuleMask(ShellLexer *lexer, unsigned int mask) {
    if (!lexer) {
        return;
    }
    lexer->rule_mask = mask;
}

unsigned int shellLexerGetRuleMask(const ShellLexer *lexer) {
    return lexer ? lexer->rule_mask : 0u;
}
