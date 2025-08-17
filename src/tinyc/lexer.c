#include "tinyc/lexer.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

void tinyc_initLexer(TinyCLexer *lexer, const char *source) {
    lexer->src = source;
    lexer->pos = 0;
    lexer->line = 1;
}

static char peek(TinyCLexer *lexer) {
    return lexer->src[lexer->pos];
}

static char advance(TinyCLexer *lexer) {
    char c = lexer->src[lexer->pos++];
    if (c == '\n') lexer->line++;
    return c;
}

static bool match(TinyCLexer *lexer, char expected) {
    if (peek(lexer) != expected) return false;
    lexer->pos++;
    return true;
}

static TinyCToken makeToken(TinyCLexer *lexer, TinyCTokenType type, const char *start, int length) {
    TinyCToken t;
    t.type = type;
    t.lexeme = start;
    t.length = length;
    t.line = lexer->line;
    t.int_val = 0;
    return t;
}

static TinyCToken identifierOrKeyword(TinyCLexer *lexer, const char *start) {
    while (isAlpha(peek(lexer)) || isDigit(peek(lexer))) advance(lexer);
    int length = &lexer->src[lexer->pos] - start;
    if (length == 3 && strncmp(start, "int", 3) == 0) return makeToken(lexer, TINYCTOKEN_INT, start, length);
    if (length == 4 && strncmp(start, "void", 4) == 0) return makeToken(lexer, TINYCTOKEN_VOID, start, length);
    if (length == 2 && strncmp(start, "if", 2) == 0) return makeToken(lexer, TINYCTOKEN_IF, start, length);
    if (length == 4 && strncmp(start, "else", 4) == 0) return makeToken(lexer, TINYCTOKEN_ELSE, start, length);
    if (length == 5 && strncmp(start, "while", 5) == 0) return makeToken(lexer, TINYCTOKEN_WHILE, start, length);
    if (length == 6 && strncmp(start, "return", 6) == 0) return makeToken(lexer, TINYCTOKEN_RETURN, start, length);
    return makeToken(lexer, TINYCTOKEN_IDENTIFIER, start, length);
}

static TinyCToken numberToken(TinyCLexer *lexer, const char *start) {
    while (isDigit(peek(lexer))) advance(lexer);
    int length = &lexer->src[lexer->pos] - start;
    TinyCToken t = makeToken(lexer, TINYCTOKEN_NUMBER, start, length);
    t.int_val = atoi(start);
    return t;
}

TinyCToken tinyc_nextToken(TinyCLexer *lexer) {
    while (1) {
        char c = peek(lexer);
        if (c == '\0') return makeToken(lexer, TINYCTOKEN_EOF, "", 0);
        if (isspace(c)) { advance(lexer); continue; }
        const char *start = &lexer->src[lexer->pos];
        if (isAlpha(c)) return identifierOrKeyword(lexer, start);
        if (isDigit(c)) return numberToken(lexer, start);
        advance(lexer);
        switch (c) {
            case '+': return makeToken(lexer, TINYCTOKEN_PLUS, start, 1);
            case '-': return makeToken(lexer, TINYCTOKEN_MINUS, start, 1);
            case '*': return makeToken(lexer, TINYCTOKEN_STAR, start, 1);
            case '/': return makeToken(lexer, TINYCTOKEN_SLASH, start, 1);
            case ';': return makeToken(lexer, TINYCTOKEN_SEMICOLON, start, 1);
            case ',': return makeToken(lexer, TINYCTOKEN_COMMA, start, 1);
            case '(': return makeToken(lexer, TINYCTOKEN_LPAREN, start, 1);
            case ')': return makeToken(lexer, TINYCTOKEN_RPAREN, start, 1);
            case '{': return makeToken(lexer, TINYCTOKEN_LBRACE, start, 1);
            case '}': return makeToken(lexer, TINYCTOKEN_RBRACE, start, 1);
            case '[': return makeToken(lexer, TINYCTOKEN_LBRACKET, start, 1);
            case ']': return makeToken(lexer, TINYCTOKEN_RBRACKET, start, 1);
            case '!': {
                bool hasEq = match(lexer, '=');
                return makeToken(lexer, hasEq ? TINYCTOKEN_BANG_EQUAL : TINYCTOKEN_BANG, start, hasEq ? 2 : 1);
            }
            case '=': {
                bool hasEq = match(lexer, '=');
                return makeToken(lexer, hasEq ? TINYCTOKEN_EQUAL_EQUAL : TINYCTOKEN_EQUAL, start, hasEq ? 2 : 1);
            }
            case '<': {
                bool hasEq = match(lexer, '=');
                return makeToken(lexer, hasEq ? TINYCTOKEN_LESS_EQUAL : TINYCTOKEN_LESS, start, hasEq ? 2 : 1);
            }
            case '>': {
                bool hasEq = match(lexer, '=');
                return makeToken(lexer, hasEq ? TINYCTOKEN_GREATER_EQUAL : TINYCTOKEN_GREATER, start, hasEq ? 2 : 1);
            }
            case '&': if (match(lexer,'&')) return makeToken(lexer, TINYCTOKEN_AND_AND, start, 2); break;
            case '|': if (match(lexer,'|')) return makeToken(lexer, TINYCTOKEN_OR_OR, start, 2); break;
        }
        return makeToken(lexer, TINYCTOKEN_UNKNOWN, start, 1);
    }
}

const char* tinycTokenTypeToString(TinyCTokenType type) {
    switch(type) {
        case TINYCTOKEN_INT: return "TOKEN_INT";
        case TINYCTOKEN_VOID: return "TOKEN_VOID";
        case TINYCTOKEN_IF: return "TOKEN_IF";
        case TINYCTOKEN_ELSE: return "TOKEN_ELSE";
        case TINYCTOKEN_WHILE: return "TOKEN_WHILE";
        case TINYCTOKEN_RETURN: return "TOKEN_RETURN";
        case TINYCTOKEN_IDENTIFIER: return "TOKEN_IDENTIFIER";
        case TINYCTOKEN_NUMBER: return "TOKEN_NUMBER";
        case TINYCTOKEN_PLUS: return "+";
        case TINYCTOKEN_MINUS: return "-";
        case TINYCTOKEN_STAR: return "*";
        case TINYCTOKEN_SLASH: return "/";
        case TINYCTOKEN_BANG: return "!";
        case TINYCTOKEN_BANG_EQUAL: return "!=";
        case TINYCTOKEN_EQUAL: return "=";
        case TINYCTOKEN_EQUAL_EQUAL: return "==";
        case TINYCTOKEN_LESS: return "<";
        case TINYCTOKEN_LESS_EQUAL: return "<=";
        case TINYCTOKEN_GREATER: return ">";
        case TINYCTOKEN_GREATER_EQUAL: return ">=";
        case TINYCTOKEN_AND_AND: return "&&";
        case TINYCTOKEN_OR_OR: return "||";
        case TINYCTOKEN_SEMICOLON: return ";";
        case TINYCTOKEN_COMMA: return ",";
        case TINYCTOKEN_LPAREN: return "(";
        case TINYCTOKEN_RPAREN: return ")";
        case TINYCTOKEN_LBRACE: return "{";
        case TINYCTOKEN_RBRACE: return "}";
        case TINYCTOKEN_LBRACKET: return "[";
        case TINYCTOKEN_RBRACKET: return "]";
        case TINYCTOKEN_EOF: return "EOF";
        default: return "UNKNOWN";
    }
}

