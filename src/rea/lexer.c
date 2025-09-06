#include <ctype.h>
#include <string.h>
#include "rea/lexer.h"

// Character classification helpers
static int isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int isDigit(char c) {
    return c >= '0' && c <= '9';
}

static int isAlphaNumeric(char c) {
    return isAlpha(c) || isDigit(c);
}

static char peek(ReaLexer *lexer) {
    return lexer->source[lexer->pos];
}

static char peekNext(ReaLexer *lexer) {
    char c = peek(lexer);
    if (c == '\0') return '\0';
    return lexer->source[lexer->pos + 1];
}

static char advance(ReaLexer *lexer) {
    return lexer->source[lexer->pos++];
}

static int match(ReaLexer *lexer, char expected) {
    if (peek(lexer) != expected) return 0;
    lexer->pos++;
    return 1;
}

static void skipWhitespace(ReaLexer *lexer) {
    for (;;) {
        char c = peek(lexer);
        switch (c) {
            case ' ': case '\r': case '\t':
                lexer->pos++;
                break;
            case '\n':
                lexer->line++;
                lexer->pos++;
                break;
            case '/':
                if (peekNext(lexer) == '/') {
                    lexer->pos += 2;
                    while (peek(lexer) != '\n' && peek(lexer) != '\0') {
                        lexer->pos++;
                    }
                } else if (peekNext(lexer) == '*') {
                    lexer->pos += 2;
                    while (peek(lexer) != '\0') {
                        if (peek(lexer) == '\n') lexer->line++;
                        if (peek(lexer) == '*' && peekNext(lexer) == '/') {
                            lexer->pos += 2;
                            break;
                        }
                        lexer->pos++;
                    }
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

static ReaToken makeToken(ReaLexer *lexer, ReaTokenType type, size_t start) {
    ReaToken t;
    t.type = type;
    t.start = lexer->source + start;
    t.length = lexer->pos - start;
    t.line = lexer->line;
    return t;
}

static ReaTokenType keywordType(const char *start, size_t length) {
    switch (length) {
        case 2:
            if (strncmp(start, "if", 2) == 0) return REA_TOKEN_IF;
            if (strncmp(start, "do", 2) == 0) return REA_TOKEN_DO;
            break;
        case 3:
            if (strncmp(start, "for", 3) == 0) return REA_TOKEN_FOR;
            if (strncmp(start, "int", 3) == 0) return REA_TOKEN_INT;
            if (strncmp(start, "str", 3) == 0) return REA_TOKEN_STR;
            if (strncmp(start, "new", 3) == 0) return REA_TOKEN_NEW;
            break;
        case 4:
            if (strncmp(start, "else", 4) == 0) return REA_TOKEN_ELSE;
            if (strncmp(start, "this", 4) == 0) return REA_TOKEN_THIS;
            if (strncmp(start, "true", 4) == 0) return REA_TOKEN_TRUE;
            if (strncmp(start, "void", 4) == 0) return REA_TOKEN_VOID;
            if (strncmp(start, "bool", 4) == 0) return REA_TOKEN_BOOL;
            if (strncmp(start, "case", 4) == 0) return REA_TOKEN_CASE;
            if (strncmp(start, "char", 4) == 0) return REA_TOKEN_CHAR;
            if (strncmp(start, "byte", 4) == 0) return REA_TOKEN_BYTE;
            if (strncmp(start, "text", 4) == 0) return REA_TOKEN_TEXT;
            if (strncmp(start, "int8", 4) == 0) return REA_TOKEN_INT8;
            break;
        case 5:
            if (strncmp(start, "class", 5) == 0) return REA_TOKEN_CLASS;
            if (strncmp(start, "while", 5) == 0) return REA_TOKEN_WHILE;
            if (strncmp(start, "break", 5) == 0) return REA_TOKEN_BREAK;
            if (strncmp(start, "super", 5) == 0) return REA_TOKEN_SUPER;
            if (strncmp(start, "float", 5) == 0) return REA_TOKEN_FLOAT;
            if (strncmp(start, "const", 5) == 0) return REA_TOKEN_CONST;
            if (strncmp(start, "false", 5) == 0) return REA_TOKEN_FALSE;
            if (strncmp(start, "int16", 5) == 0) return REA_TOKEN_INT16;
            if (strncmp(start, "int32", 5) == 0) return REA_TOKEN_INT32;
            if (strncmp(start, "int64", 5) == 0) return REA_TOKEN_INT64;
            break;
        case 6:
            if (strncmp(start, "return", 6) == 0) return REA_TOKEN_RETURN;
            if (strncmp(start, "import", 6) == 0) return REA_TOKEN_IMPORT;
            if (strncmp(start, "switch", 6) == 0) return REA_TOKEN_SWITCH;
            break;
        case 7:
            if (strncmp(start, "extends", 7) == 0) return REA_TOKEN_EXTENDS;
            if (strncmp(start, "default", 7) == 0) return REA_TOKEN_DEFAULT;
            if (strncmp(start, "float32", 7) == 0) return REA_TOKEN_FLOAT32;
            if (strncmp(start, "mstream", 7) == 0) return REA_TOKEN_MSTREAM;
            break;
        case 8:
            if (strncmp(start, "continue", 8) == 0) return REA_TOKEN_CONTINUE;
            break;
    }
    return REA_TOKEN_IDENTIFIER;
}

void reaInitLexer(ReaLexer *lexer, const char *source) {
    lexer->source = source;
    lexer->pos = 0;
    lexer->line = 1;
}

ReaToken reaNextToken(ReaLexer *lexer) {
    skipWhitespace(lexer);
    size_t start = lexer->pos;
    if (peek(lexer) == '\0') {
        return makeToken(lexer, REA_TOKEN_EOF, start);
    }

    char c = advance(lexer);
    switch (c) {
        case '(':
            return makeToken(lexer, REA_TOKEN_LEFT_PAREN, start);
        case ')':
            return makeToken(lexer, REA_TOKEN_RIGHT_PAREN, start);
        case '{':
            return makeToken(lexer, REA_TOKEN_LEFT_BRACE, start);
        case '}':
            return makeToken(lexer, REA_TOKEN_RIGHT_BRACE, start);
        case ',':
            return makeToken(lexer, REA_TOKEN_COMMA, start);
        case '.':
            return makeToken(lexer, REA_TOKEN_DOT, start);
        case ';':
            return makeToken(lexer, REA_TOKEN_SEMICOLON, start);
        case ':':
            return makeToken(lexer, REA_TOKEN_COLON, start);
        case '+':
            return makeToken(lexer, REA_TOKEN_PLUS, start);
        case '-':
            return makeToken(lexer, REA_TOKEN_MINUS, start);
        case '*':
            return makeToken(lexer, REA_TOKEN_STAR, start);
        case '/':
            return makeToken(lexer, REA_TOKEN_SLASH, start);
        case '!':
            return makeToken(lexer, match(lexer, '=') ? REA_TOKEN_BANG_EQUAL : REA_TOKEN_BANG, start);
        case '&':
            if (match(lexer, '&')) return makeToken(lexer, REA_TOKEN_AND_AND, start);
            break;
        case '|':
            if (match(lexer, '|')) return makeToken(lexer, REA_TOKEN_OR_OR, start);
            break;
        case '=':
            return makeToken(lexer, match(lexer, '=') ? REA_TOKEN_EQUAL_EQUAL : REA_TOKEN_EQUAL, start);
        case '<':
            return makeToken(lexer, match(lexer, '=') ? REA_TOKEN_LESS_EQUAL : REA_TOKEN_LESS, start);
        case '>':
            return makeToken(lexer, match(lexer, '=') ? REA_TOKEN_GREATER_EQUAL : REA_TOKEN_GREATER, start);
        case '#':
            while (isAlpha(peek(lexer))) advance(lexer);
            return makeToken(lexer, REA_TOKEN_IMPORT, start);
        case '"':
            while (peek(lexer) != '"' && peek(lexer) != '\0') {
                if (peek(lexer) == '\n') lexer->line++;
                advance(lexer);
            }
            if (peek(lexer) == '"') advance(lexer);
            return makeToken(lexer, REA_TOKEN_STRING, start);
        default:
            break;
    }

    if (isDigit(c)) {
        if (c == '0' && (peek(lexer) == 'x' || peek(lexer) == 'X')) {
            advance(lexer); // consume 'x'
            while (isxdigit(peek(lexer))) advance(lexer);
            return makeToken(lexer, REA_TOKEN_NUMBER, start);
        }
        while (isDigit(peek(lexer))) advance(lexer);
        if (peek(lexer) == '.' && isDigit(peekNext(lexer))) {
            advance(lexer);
            while (isDigit(peek(lexer))) advance(lexer);
        }
        return makeToken(lexer, REA_TOKEN_NUMBER, start);
    }

    if (isAlpha(c)) {
        while (isAlphaNumeric(peek(lexer))) advance(lexer);
        // Special-case multi-word type: long double
        size_t save_pos = lexer->pos;
        ReaTokenType type = keywordType(lexer->source + start, lexer->pos - start);
        if (type == REA_TOKEN_IDENTIFIER) {
            // Check for 'long' followed by 'double'
            const char *lex = lexer->source + start;
            size_t len = save_pos - start;
            if (len == 4 && strncmp(lex, "long", 4) == 0) {
                // Skip whitespace
                while (peek(lexer) == ' ' || peek(lexer) == '\t' || peek(lexer) == '\r' || peek(lexer) == '\n') {
                    if (peek(lexer) == '\n') lexer->line++;
                    lexer->pos++;
                }
                if (strncmp(lexer->source + lexer->pos, "double", 6) == 0 &&
                    !isAlphaNumeric(*(lexer->source + lexer->pos + 6))) {
                    lexer->pos += 6; // consume 'double'
                    return makeToken(lexer, REA_TOKEN_LONG_DOUBLE, start);
                } else {
                    lexer->pos = save_pos; // restore if not matched
                }
            }
        }
        return makeToken(lexer, type, start);
    }

    return makeToken(lexer, REA_TOKEN_UNKNOWN, start);
}
