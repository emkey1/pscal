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

static void consumeDigits(ReaLexer *lexer) {
    while (isDigit(peek(lexer))) advance(lexer);
}

static void consumeExponent(ReaLexer *lexer) {
    size_t exponent_start = lexer->pos;
    advance(lexer); // consume 'e' or 'E'
    if (peek(lexer) == '+' || peek(lexer) == '-') advance(lexer);
    if (isDigit(peek(lexer))) {
        consumeDigits(lexer);
    } else {
        lexer->pos = exponent_start;
    }
}

static int exponentAfterDotHasDigits(ReaLexer *lexer) {
    size_t pos = lexer->pos + 1; // position of potential exponent marker
    char c = lexer->source[pos];
    if (c != 'e' && c != 'E') return 0;
    pos++;
    c = lexer->source[pos];
    if (c == '+' || c == '-') {
        pos++;
        c = lexer->source[pos];
    }
    if (c == '\0') return 0;
    return isDigit(c);
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
            case '#':
                if (lexer->pos == 0 && peekNext(lexer) == '!') {
                    lexer->pos += 2;
                    while (peek(lexer) != '\n' && peek(lexer) != '\0') {
                        lexer->pos++;
                    }
                } else {
                    return;
                }
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
            if (strncmp(start, "my", 2) == 0) return REA_TOKEN_MYSELF;
            break;
        case 3:
            if (strncmp(start, "for", 3) == 0) return REA_TOKEN_FOR;
            if (strncmp(start, "int", 3) == 0) return REA_TOKEN_INT;
            if (strncmp(start, "str", 3) == 0) return REA_TOKEN_STR;
            if (strncmp(start, "new", 3) == 0) return REA_TOKEN_NEW;
            if (strncmp(start, "nil", 3) == 0) return REA_TOKEN_NIL;
            if (strncmp(start, "xor", 3) == 0) return REA_TOKEN_XOR;
            if (strncmp(start, "try", 3) == 0) return REA_TOKEN_TRY;
            break;
        case 4:
            if (strncmp(start, "else", 4) == 0) return REA_TOKEN_ELSE;
            if (strncmp(start, "true", 4) == 0) return REA_TOKEN_TRUE;
            if (strncmp(start, "void", 4) == 0) return REA_TOKEN_VOID;
            if (strncmp(start, "bool", 4) == 0) return REA_TOKEN_BOOL;
            if (strncmp(start, "case", 4) == 0) return REA_TOKEN_CASE;
            if (strncmp(start, "char", 4) == 0) return REA_TOKEN_CHAR;
            if (strncmp(start, "byte", 4) == 0) return REA_TOKEN_BYTE;
            if (strncmp(start, "text", 4) == 0) return REA_TOKEN_TEXT;
            if (strncmp(start, "int8", 4) == 0) return REA_TOKEN_INT8;
            if (strncmp(start, "join", 4) == 0) return REA_TOKEN_JOIN;
            if (strncmp(start, "type", 4) == 0) return REA_TOKEN_TYPE;
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
            if (strncmp(start, "spawn", 5) == 0) return REA_TOKEN_SPAWN;
            if (strncmp(start, "alias", 5) == 0) return REA_TOKEN_ALIAS;
            if (strncmp(start, "match", 5) == 0) return REA_TOKEN_MATCH;
            if (strncmp(start, "catch", 5) == 0) return REA_TOKEN_CATCH;
            if (strncmp(start, "throw", 5) == 0) return REA_TOKEN_THROW;
            break;
        case 6:
            if (strncmp(start, "return", 6) == 0) return REA_TOKEN_RETURN;
            if (strncmp(start, "import", 6) == 0) return REA_TOKEN_IMPORT;
            if (strncmp(start, "switch", 6) == 0) return REA_TOKEN_SWITCH;
            if (strncmp(start, "double", 6) == 0) return REA_TOKEN_FLOAT;
            if (strncmp(start, "myself", 6) == 0) return REA_TOKEN_MYSELF;
            if (strncmp(start, "string", 6) == 0) return REA_TOKEN_STR;
            if (strncmp(start, "module", 6) == 0) return REA_TOKEN_MODULE;
            if (strncmp(start, "export", 6) == 0) return REA_TOKEN_EXPORT;
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
        case '[':
            return makeToken(lexer, REA_TOKEN_LEFT_BRACKET, start);
        case ']':
            return makeToken(lexer, REA_TOKEN_RIGHT_BRACKET, start);
        case ',':
            return makeToken(lexer, REA_TOKEN_COMMA, start);
        case '.':
            if (isDigit(peek(lexer))) {
                consumeDigits(lexer);
                if (peek(lexer) == 'e' || peek(lexer) == 'E') {
                    consumeExponent(lexer);
                }
                return makeToken(lexer, REA_TOKEN_NUMBER, start);
            }
            return makeToken(lexer, REA_TOKEN_DOT, start);
        case ';':
            return makeToken(lexer, REA_TOKEN_SEMICOLON, start);
        case ':':
            return makeToken(lexer, REA_TOKEN_COLON, start);
        case '?':
            return makeToken(lexer, REA_TOKEN_QUESTION, start);
        case '+':
            if (match(lexer, '+')) return makeToken(lexer, REA_TOKEN_PLUS_PLUS, start);
            return makeToken(lexer, match(lexer, '=') ? REA_TOKEN_PLUS_EQUAL : REA_TOKEN_PLUS, start);
        case '-':
            if (match(lexer, '-')) return makeToken(lexer, REA_TOKEN_MINUS_MINUS, start);
            if (match(lexer, '>')) return makeToken(lexer, REA_TOKEN_ARROW, start);
            return makeToken(lexer, match(lexer, '=') ? REA_TOKEN_MINUS_EQUAL : REA_TOKEN_MINUS, start);
        case '*':
            return makeToken(lexer, REA_TOKEN_STAR, start);
        case '/':
            return makeToken(lexer, REA_TOKEN_SLASH, start);
        case '%':
            return makeToken(lexer, REA_TOKEN_PERCENT, start);
        case '!':
            return makeToken(lexer, match(lexer, '=') ? REA_TOKEN_BANG_EQUAL : REA_TOKEN_BANG, start);
        case '&':
            if (match(lexer, '&')) return makeToken(lexer, REA_TOKEN_AND_AND, start);
            return makeToken(lexer, REA_TOKEN_AND, start);
        case '|':
            if (match(lexer, '|')) return makeToken(lexer, REA_TOKEN_OR_OR, start);
            return makeToken(lexer, REA_TOKEN_OR, start);
        case '^':
            return makeToken(lexer, REA_TOKEN_XOR, start);
        case '=':
            return makeToken(lexer, match(lexer, '=') ? REA_TOKEN_EQUAL_EQUAL : REA_TOKEN_EQUAL, start);
        case '<':
            if (match(lexer, '<')) {
                return makeToken(lexer, REA_TOKEN_SHIFT_LEFT, start);
            }
            return makeToken(lexer, match(lexer, '=') ? REA_TOKEN_LESS_EQUAL : REA_TOKEN_LESS, start);
        case '>':
            if (match(lexer, '>')) {
                return makeToken(lexer, REA_TOKEN_SHIFT_RIGHT, start);
            }
            return makeToken(lexer, match(lexer, '=') ? REA_TOKEN_GREATER_EQUAL : REA_TOKEN_GREATER, start);
        case '#':
            while (isAlpha(peek(lexer))) advance(lexer);
            return makeToken(lexer, REA_TOKEN_IMPORT, start);
        case '"':
            while (peek(lexer) != '\0') {
                char ch = peek(lexer);
                if (ch == '\n') {
                    // Implicitly terminate the string at newline if no closing quote
                    break;
                }
                if (ch == '\\') {
                    advance(lexer);
                    if (peek(lexer) != '\0') advance(lexer);
                } else if (ch == '"') {
                    break;
                } else {
                    advance(lexer);
                }
            }
            if (peek(lexer) == '"') {
                advance(lexer);
            }
            return makeToken(lexer, REA_TOKEN_STRING, start);
        case '\'':
            while (peek(lexer) != '\0') {
                char ch = peek(lexer);
                if (ch == '\n') {
                    break;
                }
                if (ch == '\\') {
                    advance(lexer);
                    if (peek(lexer) != '\0') advance(lexer);
                } else if (ch == '\'') {
                    break;
                } else {
                    advance(lexer);
                }
            }
            if (peek(lexer) == '\'') {
                advance(lexer);
            }
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
        if (peek(lexer) == '.') {
            char next = peekNext(lexer);
            if (isDigit(next)) {
                advance(lexer);
                consumeDigits(lexer);
            } else if (exponentAfterDotHasDigits(lexer)) {
                advance(lexer);
            } else if (!isAlpha(next) && next != '_') {
                advance(lexer);
            }
        }
        if (peek(lexer) == 'e' || peek(lexer) == 'E') {
            consumeExponent(lexer);
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

const char* reaTokenTypeToString(ReaTokenType type) {
    switch (type) {
        case REA_TOKEN_EOF: return "EOF";
        case REA_TOKEN_UNKNOWN: return "UNKNOWN";
        case REA_TOKEN_IDENTIFIER: return "IDENTIFIER";
        case REA_TOKEN_NUMBER: return "NUMBER";
        case REA_TOKEN_STRING: return "STRING";
        case REA_TOKEN_LEFT_PAREN: return "LEFT_PAREN";
        case REA_TOKEN_RIGHT_PAREN: return "RIGHT_PAREN";
        case REA_TOKEN_LEFT_BRACE: return "LEFT_BRACE";
        case REA_TOKEN_RIGHT_BRACE: return "RIGHT_BRACE";
        case REA_TOKEN_LEFT_BRACKET: return "LEFT_BRACKET";
        case REA_TOKEN_RIGHT_BRACKET: return "RIGHT_BRACKET";
        case REA_TOKEN_COMMA: return "COMMA";
        case REA_TOKEN_DOT: return "DOT";
        case REA_TOKEN_SEMICOLON: return "SEMICOLON";
        case REA_TOKEN_COLON: return "COLON";
        case REA_TOKEN_QUESTION: return "QUESTION";
        case REA_TOKEN_ARROW: return "ARROW";
        case REA_TOKEN_PLUS: return "PLUS";
        case REA_TOKEN_PLUS_PLUS: return "PLUS_PLUS";
        case REA_TOKEN_PLUS_EQUAL: return "PLUS_EQUAL";
        case REA_TOKEN_MINUS: return "MINUS";
        case REA_TOKEN_MINUS_MINUS: return "MINUS_MINUS";
        case REA_TOKEN_MINUS_EQUAL: return "MINUS_EQUAL";
        case REA_TOKEN_STAR: return "STAR";
        case REA_TOKEN_SLASH: return "SLASH";
        case REA_TOKEN_PERCENT: return "PERCENT";
        case REA_TOKEN_EQUAL: return "EQUAL";
        case REA_TOKEN_EQUAL_EQUAL: return "EQUAL_EQUAL";
        case REA_TOKEN_BANG: return "BANG";
        case REA_TOKEN_BANG_EQUAL: return "BANG_EQUAL";
        case REA_TOKEN_AND: return "AND";
        case REA_TOKEN_AND_AND: return "AND_AND";
        case REA_TOKEN_OR: return "OR";
        case REA_TOKEN_OR_OR: return "OR_OR";
        case REA_TOKEN_XOR: return "XOR";
        case REA_TOKEN_SHIFT_LEFT: return "SHIFT_LEFT";
        case REA_TOKEN_SHIFT_RIGHT: return "SHIFT_RIGHT";
        case REA_TOKEN_GREATER: return "GREATER";
        case REA_TOKEN_GREATER_EQUAL: return "GREATER_EQUAL";
        case REA_TOKEN_LESS: return "LESS";
        case REA_TOKEN_LESS_EQUAL: return "LESS_EQUAL";
        case REA_TOKEN_CLASS: return "CLASS";
        case REA_TOKEN_EXTENDS: return "EXTENDS";
        case REA_TOKEN_NEW: return "NEW";
        case REA_TOKEN_MYSELF: return "MYSELF";
        case REA_TOKEN_SUPER: return "SUPER";
        case REA_TOKEN_IF: return "IF";
        case REA_TOKEN_ELSE: return "ELSE";
        case REA_TOKEN_WHILE: return "WHILE";
        case REA_TOKEN_FOR: return "FOR";
        case REA_TOKEN_DO: return "DO";
        case REA_TOKEN_SWITCH: return "SWITCH";
        case REA_TOKEN_CASE: return "CASE";
        case REA_TOKEN_DEFAULT: return "DEFAULT";
        case REA_TOKEN_TYPE: return "TYPE";
        case REA_TOKEN_ALIAS: return "ALIAS";
        case REA_TOKEN_MATCH: return "MATCH";
        case REA_TOKEN_TRY: return "TRY";
        case REA_TOKEN_CATCH: return "CATCH";
        case REA_TOKEN_THROW: return "THROW";
        case REA_TOKEN_BREAK: return "BREAK";
        case REA_TOKEN_CONTINUE: return "CONTINUE";
        case REA_TOKEN_RETURN: return "RETURN";
        case REA_TOKEN_TRUE: return "TRUE";
        case REA_TOKEN_FALSE: return "FALSE";
        case REA_TOKEN_NIL: return "NIL";
        case REA_TOKEN_CONST: return "CONST";
        case REA_TOKEN_IMPORT: return "IMPORT";
        case REA_TOKEN_SPAWN: return "SPAWN";
        case REA_TOKEN_JOIN: return "JOIN";
        case REA_TOKEN_INT: return "INT";
        case REA_TOKEN_INT64: return "INT64";
        case REA_TOKEN_INT32: return "INT32";
        case REA_TOKEN_INT16: return "INT16";
        case REA_TOKEN_INT8: return "INT8";
        case REA_TOKEN_FLOAT: return "FLOAT";
        case REA_TOKEN_FLOAT32: return "FLOAT32";
        case REA_TOKEN_LONG_DOUBLE: return "LONG_DOUBLE";
        case REA_TOKEN_CHAR: return "CHAR";
        case REA_TOKEN_BYTE: return "BYTE";
        case REA_TOKEN_STR: return "STR";
        case REA_TOKEN_TEXT: return "TEXT";
        case REA_TOKEN_MSTREAM: return "MSTREAM";
        case REA_TOKEN_VOID: return "VOID";
        case REA_TOKEN_BOOL: return "BOOL";
        default: return "UNKNOWN";
    }
}
