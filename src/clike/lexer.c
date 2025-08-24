#include "clike/lexer.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

void clike_initLexer(ClikeLexer *lexer, const char *source) {
    lexer->src = source;
    lexer->pos = 0;
    lexer->line = 1;
    lexer->column = 1;
}

static char peek(ClikeLexer *lexer) {
    return lexer->src[lexer->pos];
}

static char advance(ClikeLexer *lexer) {
    char c = lexer->src[lexer->pos++];
    if (c == '\n') { lexer->line++; lexer->column = 1; }
    else lexer->column++;
    return c;
}

static bool match(ClikeLexer *lexer, char expected) {
    if (peek(lexer) != expected) return false;
    lexer->pos++;
    return true;
}

static ClikeToken makeToken(ClikeLexer *lexer, ClikeTokenType type, const char *start, int length, int column) {
    ClikeToken t;
    t.type = type;
    t.lexeme = start;
    t.length = length;
    t.line = lexer->line;
    t.column = column;
    t.int_val = 0;
    t.float_val = 0.0;
    return t;
}
static ClikeToken identifierOrKeyword(ClikeLexer *lexer, const char *start, int column) {
    while (isAlpha(peek(lexer)) || isDigit(peek(lexer))) advance(lexer);
    int length = &lexer->src[lexer->pos] - start;
    if (length == 3 && strncmp(start, "int", 3) == 0) return makeToken(lexer, CLIKE_TOKEN_INT, start, length, column);
    if (length == 4 && strncmp(start, "long", 4) == 0) return makeToken(lexer, CLIKE_TOKEN_LONG, start, length, column);
    if (length == 4 && strncmp(start, "void", 4) == 0) return makeToken(lexer, CLIKE_TOKEN_VOID, start, length, column);
    if (length == 5 && strncmp(start, "float", 5) == 0) return makeToken(lexer, CLIKE_TOKEN_FLOAT, start, length, column);
    if (length == 6 && strncmp(start, "double", 6) == 0) return makeToken(lexer, CLIKE_TOKEN_DOUBLE, start, length, column);
    if (length == 3 && strncmp(start, "str", 3) == 0) return makeToken(lexer, CLIKE_TOKEN_STR, start, length, column);
    if (length == 4 && strncmp(start, "text", 4) == 0) return makeToken(lexer, CLIKE_TOKEN_TEXT, start, length, column);
    if (length == 7 && strncmp(start, "mstream", 7) == 0) return makeToken(lexer, CLIKE_TOKEN_MSTREAM, start, length, column);
    if (length == 4 && strncmp(start, "char", 4) == 0) return makeToken(lexer, CLIKE_TOKEN_CHAR, start, length, column);
    if (length == 4 && strncmp(start, "byte", 4) == 0) return makeToken(lexer, CLIKE_TOKEN_BYTE, start, length, column);
    if (length == 2 && strncmp(start, "if", 2) == 0) return makeToken(lexer, CLIKE_TOKEN_IF, start, length, column);
    if (length == 4 && strncmp(start, "else", 4) == 0) return makeToken(lexer, CLIKE_TOKEN_ELSE, start, length, column);
    if (length == 5 && strncmp(start, "while", 5) == 0) return makeToken(lexer, CLIKE_TOKEN_WHILE, start, length, column);
    if (length == 3 && strncmp(start, "for", 3) == 0) return makeToken(lexer, CLIKE_TOKEN_FOR, start, length, column);
    if (length == 2 && strncmp(start, "do", 2) == 0) return makeToken(lexer, CLIKE_TOKEN_DO, start, length, column);
    if (length == 6 && strncmp(start, "switch", 6) == 0) return makeToken(lexer, CLIKE_TOKEN_SWITCH, start, length, column);
    if (length == 4 && strncmp(start, "case", 4) == 0) return makeToken(lexer, CLIKE_TOKEN_CASE, start, length, column);
    if (length == 7 && strncmp(start, "default", 7) == 0) return makeToken(lexer, CLIKE_TOKEN_DEFAULT, start, length, column);
    if (length == 6 && strncmp(start, "struct", 6) == 0) return makeToken(lexer, CLIKE_TOKEN_STRUCT, start, length, column);
    if (length == 4 && strncmp(start, "enum", 4) == 0) return makeToken(lexer, CLIKE_TOKEN_ENUM, start, length, column);
    if (length == 5 && strncmp(start, "const", 5) == 0) return makeToken(lexer, CLIKE_TOKEN_CONST, start, length, column);
    if (length == 5 && strncmp(start, "break", 5) == 0) return makeToken(lexer, CLIKE_TOKEN_BREAK, start, length, column);
    if (length == 8 && strncmp(start, "continue", 8) == 0) return makeToken(lexer, CLIKE_TOKEN_CONTINUE, start, length, column);
    if (length == 6 && strncmp(start, "return", 6) == 0) return makeToken(lexer, CLIKE_TOKEN_RETURN, start, length, column);
    if (length == 6 && strncmp(start, "import", 6) == 0) return makeToken(lexer, CLIKE_TOKEN_IMPORT, start, length, column);
    return makeToken(lexer, CLIKE_TOKEN_IDENTIFIER, start, length, column);
}

static ClikeToken numberToken(ClikeLexer *lexer, const char *start, int column) {
    bool isFloat = false;
    if (start[0] == '0' && (peek(lexer) == 'x' || peek(lexer) == 'X')) {
        advance(lexer); // consume 'x'
        while (isxdigit(peek(lexer))) advance(lexer);
        int length = &lexer->src[lexer->pos] - start;
        ClikeToken t = makeToken(lexer, CLIKE_TOKEN_NUMBER, start, length, column);
        char *tmp = strndup(start, length);
        t.int_val = strtoll(tmp, NULL, 0);
        free(tmp);
        return t;
    }
    while (isDigit(peek(lexer))) advance(lexer);
    if (peek(lexer) == '.' && isDigit(lexer->src[lexer->pos + 1])) {
        isFloat = true;
        advance(lexer); // consume '.'
        while (isDigit(peek(lexer))) advance(lexer);
    }
    int length = &lexer->src[lexer->pos] - start;
    ClikeToken t = makeToken(lexer, isFloat ? CLIKE_TOKEN_FLOAT_LITERAL : CLIKE_TOKEN_NUMBER, start, length, column);
    if (isFloat) t.float_val = atof(start); else t.int_val = strtoll(start, NULL, 10);
    return t;
}

static ClikeToken stringToken(ClikeLexer *lexer, const char *start, int column) {
    advance(lexer); // consume opening quote
    while (peek(lexer) != '"' && peek(lexer) != '\0') {
        advance(lexer);
    }
    int length = &lexer->src[lexer->pos] - start - 1;
    if (peek(lexer) == '"') advance(lexer); // consume closing quote
    return makeToken(lexer, CLIKE_TOKEN_STRING, start + 1, length, column);
}

static ClikeToken charToken(ClikeLexer *lexer, const char *start, int column) {
    advance(lexer); // consume opening quote
    char c = advance(lexer);
    if (c == '\\') {
        char esc = advance(lexer);
        switch (esc) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case '\\': c = '\\'; break;
            case '\'': c = '\''; break;
            default: c = esc; break;
        }
    }
    if (peek(lexer) == '\'') advance(lexer); // consume closing quote
    ClikeToken t = makeToken(lexer, CLIKE_TOKEN_CHAR_LITERAL, start + 1, 1, column);
    t.int_val = (unsigned char)c;
    return t;
}

ClikeToken clike_nextToken(ClikeLexer *lexer) {
    while (1) {
        char c = peek(lexer);
        if (c == '\0') return makeToken(lexer, CLIKE_TOKEN_EOF, "", 0, lexer->column);
        if (isspace(c)) { advance(lexer); continue; }
        if (c == '#') {
            while (peek(lexer) != '\n' && peek(lexer) != '\0') advance(lexer);
            continue;
        }
        if (c == '/' && lexer->src[lexer->pos + 1] == '/') {
            while (peek(lexer) != '\n' && peek(lexer) != '\0') advance(lexer);
            continue;
        }
        if (c == '/' && lexer->src[lexer->pos + 1] == '*') {
            advance(lexer); advance(lexer);
            while (!(peek(lexer) == '*' && lexer->src[lexer->pos + 1] == '/') && peek(lexer) != '\0') {
                advance(lexer);
            }
            if (peek(lexer) == '*' && lexer->src[lexer->pos + 1] == '/') { advance(lexer); advance(lexer); }
            continue;
        }
        const char *start = &lexer->src[lexer->pos];
        int startColumn = lexer->column;
        if (isAlpha(c)) return identifierOrKeyword(lexer, start, startColumn);
        if (isDigit(c)) return numberToken(lexer, start, startColumn);
        if (c == '"') return stringToken(lexer, start, startColumn);
        if (c == '\'') return charToken(lexer, start, startColumn);
        advance(lexer);
        switch (c) {
            case '+': {
                if (match(lexer, '+')) return makeToken(lexer, CLIKE_TOKEN_PLUS_PLUS, start, 2, startColumn);
                if (match(lexer, '=')) return makeToken(lexer, CLIKE_TOKEN_PLUS_EQUAL, start, 2, startColumn);
                return makeToken(lexer, CLIKE_TOKEN_PLUS, start, 1, startColumn);
            }
            case '-': {
                if (match(lexer, '-')) return makeToken(lexer, CLIKE_TOKEN_MINUS_MINUS, start, 2, startColumn);
                if (match(lexer, '>')) return makeToken(lexer, CLIKE_TOKEN_ARROW, start, 2, startColumn);
                return makeToken(lexer, CLIKE_TOKEN_MINUS, start, 1, startColumn);
            }
            case '*': return makeToken(lexer, CLIKE_TOKEN_STAR, start, 1, startColumn);
            case '/': return makeToken(lexer, CLIKE_TOKEN_SLASH, start, 1, startColumn);
            case '%': return makeToken(lexer, CLIKE_TOKEN_PERCENT, start, 1, startColumn);
            case '~': return makeToken(lexer, CLIKE_TOKEN_TILDE, start, 1, startColumn);
            case ';': return makeToken(lexer, CLIKE_TOKEN_SEMICOLON, start, 1, startColumn);
            case ',': return makeToken(lexer, CLIKE_TOKEN_COMMA, start, 1, startColumn);
            case '(': return makeToken(lexer, CLIKE_TOKEN_LPAREN, start, 1, startColumn);
            case ')': return makeToken(lexer, CLIKE_TOKEN_RPAREN, start, 1, startColumn);
            case '{': return makeToken(lexer, CLIKE_TOKEN_LBRACE, start, 1, startColumn);
            case '}': return makeToken(lexer, CLIKE_TOKEN_RBRACE, start, 1, startColumn);
            case '[': return makeToken(lexer, CLIKE_TOKEN_LBRACKET, start, 1, startColumn);
            case ']': return makeToken(lexer, CLIKE_TOKEN_RBRACKET, start, 1, startColumn);
            case '!': {
                bool hasEq = match(lexer, '=');
                return makeToken(lexer, hasEq ? CLIKE_TOKEN_BANG_EQUAL : CLIKE_TOKEN_BANG, start, hasEq ? 2 : 1, startColumn);
            }
            case '=': {
                bool hasEq = match(lexer, '=');
                return makeToken(lexer, hasEq ? CLIKE_TOKEN_EQUAL_EQUAL : CLIKE_TOKEN_EQUAL, start, hasEq ? 2 : 1, startColumn);
            }
            case '<': {
                if (match(lexer, '<')) return makeToken(lexer, CLIKE_TOKEN_SHL, start, 2, startColumn);
                bool hasEq = match(lexer, '=');
                return makeToken(lexer, hasEq ? CLIKE_TOKEN_LESS_EQUAL : CLIKE_TOKEN_LESS, start, hasEq ? 2 : 1, startColumn);
            }
            case '>': {
                if (match(lexer, '>')) return makeToken(lexer, CLIKE_TOKEN_SHR, start, 2, startColumn);
                bool hasEq = match(lexer, '=');
                return makeToken(lexer, hasEq ? CLIKE_TOKEN_GREATER_EQUAL : CLIKE_TOKEN_GREATER, start, hasEq ? 2 : 1, startColumn);
            }
            case '&': {
                if (match(lexer,'&')) return makeToken(lexer, CLIKE_TOKEN_AND_AND, start, 2, startColumn);
                return makeToken(lexer, CLIKE_TOKEN_BIT_AND, start, 1, startColumn);
            }
              case '|': {
                  if (match(lexer,'|')) return makeToken(lexer, CLIKE_TOKEN_OR_OR, start, 2, startColumn);
                  return makeToken(lexer, CLIKE_TOKEN_BIT_OR, start, 1, startColumn);
              }
              case '?': return makeToken(lexer, CLIKE_TOKEN_QUESTION, start, 1, startColumn);
              case ':': return makeToken(lexer, CLIKE_TOKEN_COLON, start, 1, startColumn);
              case '.': return makeToken(lexer, CLIKE_TOKEN_DOT, start, 1, startColumn);
        }
        return makeToken(lexer, CLIKE_TOKEN_UNKNOWN, start, 1, startColumn);
    }
}

const char* clikeTokenTypeToString(ClikeTokenType type) {
    switch(type) {
        case CLIKE_TOKEN_INT: return "TOKEN_INT";
        case CLIKE_TOKEN_LONG: return "TOKEN_LONG";
        case CLIKE_TOKEN_VOID: return "TOKEN_VOID";
        case CLIKE_TOKEN_FLOAT: return "TOKEN_FLOAT";
        case CLIKE_TOKEN_DOUBLE: return "TOKEN_DOUBLE";
        case CLIKE_TOKEN_STR: return "TOKEN_STR";
        case CLIKE_TOKEN_TEXT: return "TOKEN_TEXT";
        case CLIKE_TOKEN_MSTREAM: return "TOKEN_MSTREAM";
        case CLIKE_TOKEN_IF: return "TOKEN_IF";
        case CLIKE_TOKEN_ELSE: return "TOKEN_ELSE";
        case CLIKE_TOKEN_WHILE: return "TOKEN_WHILE";
        case CLIKE_TOKEN_FOR: return "TOKEN_FOR";
        case CLIKE_TOKEN_DO: return "TOKEN_DO";
        case CLIKE_TOKEN_SWITCH: return "TOKEN_SWITCH";
        case CLIKE_TOKEN_CASE: return "TOKEN_CASE";
        case CLIKE_TOKEN_DEFAULT: return "TOKEN_DEFAULT";
        case CLIKE_TOKEN_STRUCT: return "TOKEN_STRUCT";
        case CLIKE_TOKEN_ENUM: return "TOKEN_ENUM";
        case CLIKE_TOKEN_CONST: return "TOKEN_CONST";
        case CLIKE_TOKEN_BREAK: return "TOKEN_BREAK";
        case CLIKE_TOKEN_CONTINUE: return "TOKEN_CONTINUE";
        case CLIKE_TOKEN_RETURN: return "TOKEN_RETURN";
        case CLIKE_TOKEN_IMPORT: return "TOKEN_IMPORT";
        case CLIKE_TOKEN_IDENTIFIER: return "TOKEN_IDENTIFIER";
        case CLIKE_TOKEN_NUMBER: return "TOKEN_NUMBER";
        case CLIKE_TOKEN_FLOAT_LITERAL: return "TOKEN_FLOAT_LITERAL";
        case CLIKE_TOKEN_CHAR_LITERAL: return "TOKEN_CHAR";
        case CLIKE_TOKEN_CHAR: return "TOKEN_CHAR_TYPE";
        case CLIKE_TOKEN_BYTE: return "TOKEN_BYTE";
        case CLIKE_TOKEN_STRING: return "TOKEN_STRING";
        case CLIKE_TOKEN_PLUS: return "+";
        case CLIKE_TOKEN_PLUS_EQUAL: return "+=";
        case CLIKE_TOKEN_MINUS: return "-";
        case CLIKE_TOKEN_PLUS_PLUS: return "++";
        case CLIKE_TOKEN_MINUS_MINUS: return "--";
        case CLIKE_TOKEN_STAR: return "*";
        case CLIKE_TOKEN_SLASH: return "/";
        case CLIKE_TOKEN_PERCENT: return "%";
        case CLIKE_TOKEN_TILDE: return "~";
        case CLIKE_TOKEN_BIT_AND: return "&";
        case CLIKE_TOKEN_BIT_OR: return "|";
        case CLIKE_TOKEN_SHL: return "<<";
        case CLIKE_TOKEN_SHR: return ">>";
        case CLIKE_TOKEN_BANG: return "!";
        case CLIKE_TOKEN_BANG_EQUAL: return "!=";
        case CLIKE_TOKEN_EQUAL: return "=";
        case CLIKE_TOKEN_EQUAL_EQUAL: return "==";
        case CLIKE_TOKEN_LESS: return "<";
        case CLIKE_TOKEN_LESS_EQUAL: return "<=";
        case CLIKE_TOKEN_GREATER: return ">";
        case CLIKE_TOKEN_GREATER_EQUAL: return ">=";
        case CLIKE_TOKEN_AND_AND: return "&&";
        case CLIKE_TOKEN_OR_OR: return "||";
        case CLIKE_TOKEN_QUESTION: return "?";
        case CLIKE_TOKEN_COLON: return ":";
        case CLIKE_TOKEN_DOT: return ".";
        case CLIKE_TOKEN_ARROW: return "->";
        case CLIKE_TOKEN_SEMICOLON: return ";";
        case CLIKE_TOKEN_COMMA: return ",";
        case CLIKE_TOKEN_LPAREN: return "(";
        case CLIKE_TOKEN_RPAREN: return ")";
        case CLIKE_TOKEN_LBRACE: return "{";
        case CLIKE_TOKEN_RBRACE: return "}";
        case CLIKE_TOKEN_LBRACKET: return "[";
        case CLIKE_TOKEN_RBRACKET: return "]";
        case CLIKE_TOKEN_EOF: return "EOF";
        default: return "UNKNOWN";
    }
}

