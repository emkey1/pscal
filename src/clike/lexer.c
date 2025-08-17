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
}

static char peek(ClikeLexer *lexer) {
    return lexer->src[lexer->pos];
}

static char advance(ClikeLexer *lexer) {
    char c = lexer->src[lexer->pos++];
    if (c == '\n') lexer->line++;
    return c;
}

static bool match(ClikeLexer *lexer, char expected) {
    if (peek(lexer) != expected) return false;
    lexer->pos++;
    return true;
}

static ClikeToken makeToken(ClikeLexer *lexer, ClikeTokenType type, const char *start, int length) {
    ClikeToken t;
    t.type = type;
    t.lexeme = start;
    t.length = length;
    t.line = lexer->line;
    t.int_val = 0;
    t.float_val = 0.0;
    return t;
}

static ClikeToken identifierOrKeyword(ClikeLexer *lexer, const char *start) {
    while (isAlpha(peek(lexer)) || isDigit(peek(lexer))) advance(lexer);
    int length = &lexer->src[lexer->pos] - start;
    if (length == 3 && strncmp(start, "int", 3) == 0) return makeToken(lexer, CLIKE_TOKEN_INT, start, length);
    if (length == 4 && strncmp(start, "void", 4) == 0) return makeToken(lexer, CLIKE_TOKEN_VOID, start, length);
    if (length == 5 && strncmp(start, "float", 5) == 0) return makeToken(lexer, CLIKE_TOKEN_FLOAT, start, length);
    if (length == 3 && strncmp(start, "str", 3) == 0) return makeToken(lexer, CLIKE_TOKEN_STR, start, length);
    if (length == 2 && strncmp(start, "if", 2) == 0) return makeToken(lexer, CLIKE_TOKEN_IF, start, length);
    if (length == 4 && strncmp(start, "else", 4) == 0) return makeToken(lexer, CLIKE_TOKEN_ELSE, start, length);
    if (length == 5 && strncmp(start, "while", 5) == 0) return makeToken(lexer, CLIKE_TOKEN_WHILE, start, length);
    if (length == 6 && strncmp(start, "return", 6) == 0) return makeToken(lexer, CLIKE_TOKEN_RETURN, start, length);
    return makeToken(lexer, CLIKE_TOKEN_IDENTIFIER, start, length);
}

static ClikeToken numberToken(ClikeLexer *lexer, const char *start) {
    bool isFloat = false;
    while (isDigit(peek(lexer))) advance(lexer);
    if (peek(lexer) == '.' && isDigit(lexer->src[lexer->pos + 1])) {
        isFloat = true;
        advance(lexer); // consume '.'
        while (isDigit(peek(lexer))) advance(lexer);
    }
    int length = &lexer->src[lexer->pos] - start;
    ClikeToken t = makeToken(lexer, isFloat ? CLIKE_TOKEN_FLOAT_LITERAL : CLIKE_TOKEN_NUMBER, start, length);
    if (isFloat) t.float_val = atof(start); else t.int_val = atoi(start);
    return t;
}

static ClikeToken stringToken(ClikeLexer *lexer, const char *start) {
    advance(lexer); // consume opening quote
    while (peek(lexer) != '"' && peek(lexer) != '\0') {
        if (peek(lexer) == '\n') lexer->line++;
        advance(lexer);
    }
    int length = &lexer->src[lexer->pos] - start - 1;
    if (peek(lexer) == '"') advance(lexer); // consume closing quote
    return makeToken(lexer, CLIKE_TOKEN_STRING, start + 1, length);
}

ClikeToken clike_nextToken(ClikeLexer *lexer) {
    while (1) {
        char c = peek(lexer);
        if (c == '\0') return makeToken(lexer, CLIKE_TOKEN_EOF, "", 0);
        if (isspace(c)) { advance(lexer); continue; }
        const char *start = &lexer->src[lexer->pos];
        if (isAlpha(c)) return identifierOrKeyword(lexer, start);
        if (isDigit(c)) return numberToken(lexer, start);
        if (c == '"') return stringToken(lexer, start);
        advance(lexer);
        switch (c) {
            case '+': return makeToken(lexer, CLIKE_TOKEN_PLUS, start, 1);
            case '-': return makeToken(lexer, CLIKE_TOKEN_MINUS, start, 1);
            case '*': return makeToken(lexer, CLIKE_TOKEN_STAR, start, 1);
            case '/': return makeToken(lexer, CLIKE_TOKEN_SLASH, start, 1);
            case ';': return makeToken(lexer, CLIKE_TOKEN_SEMICOLON, start, 1);
            case ',': return makeToken(lexer, CLIKE_TOKEN_COMMA, start, 1);
            case '(': return makeToken(lexer, CLIKE_TOKEN_LPAREN, start, 1);
            case ')': return makeToken(lexer, CLIKE_TOKEN_RPAREN, start, 1);
            case '{': return makeToken(lexer, CLIKE_TOKEN_LBRACE, start, 1);
            case '}': return makeToken(lexer, CLIKE_TOKEN_RBRACE, start, 1);
            case '[': return makeToken(lexer, CLIKE_TOKEN_LBRACKET, start, 1);
            case ']': return makeToken(lexer, CLIKE_TOKEN_RBRACKET, start, 1);
            case '!': {
                bool hasEq = match(lexer, '=');
                return makeToken(lexer, hasEq ? CLIKE_TOKEN_BANG_EQUAL : CLIKE_TOKEN_BANG, start, hasEq ? 2 : 1);
            }
            case '=': {
                bool hasEq = match(lexer, '=');
                return makeToken(lexer, hasEq ? CLIKE_TOKEN_EQUAL_EQUAL : CLIKE_TOKEN_EQUAL, start, hasEq ? 2 : 1);
            }
            case '<': {
                bool hasEq = match(lexer, '=');
                return makeToken(lexer, hasEq ? CLIKE_TOKEN_LESS_EQUAL : CLIKE_TOKEN_LESS, start, hasEq ? 2 : 1);
            }
            case '>': {
                bool hasEq = match(lexer, '=');
                return makeToken(lexer, hasEq ? CLIKE_TOKEN_GREATER_EQUAL : CLIKE_TOKEN_GREATER, start, hasEq ? 2 : 1);
            }
            case '&': if (match(lexer,'&')) return makeToken(lexer, CLIKE_TOKEN_AND_AND, start, 2); break;
            case '|': if (match(lexer,'|')) return makeToken(lexer, CLIKE_TOKEN_OR_OR, start, 2); break;
        }
        return makeToken(lexer, CLIKE_TOKEN_UNKNOWN, start, 1);
    }
}

const char* clikeTokenTypeToString(ClikeTokenType type) {
    switch(type) {
        case CLIKE_TOKEN_INT: return "TOKEN_INT";
        case CLIKE_TOKEN_VOID: return "TOKEN_VOID";
        case CLIKE_TOKEN_FLOAT: return "TOKEN_FLOAT";
        case CLIKE_TOKEN_STR: return "TOKEN_STR";
        case CLIKE_TOKEN_IF: return "TOKEN_IF";
        case CLIKE_TOKEN_ELSE: return "TOKEN_ELSE";
        case CLIKE_TOKEN_WHILE: return "TOKEN_WHILE";
        case CLIKE_TOKEN_RETURN: return "TOKEN_RETURN";
        case CLIKE_TOKEN_IDENTIFIER: return "TOKEN_IDENTIFIER";
        case CLIKE_TOKEN_NUMBER: return "TOKEN_NUMBER";
        case CLIKE_TOKEN_FLOAT_LITERAL: return "TOKEN_FLOAT_LITERAL";
        case CLIKE_TOKEN_STRING: return "TOKEN_STRING";
        case CLIKE_TOKEN_PLUS: return "+";
        case CLIKE_TOKEN_MINUS: return "-";
        case CLIKE_TOKEN_STAR: return "*";
        case CLIKE_TOKEN_SLASH: return "/";
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

