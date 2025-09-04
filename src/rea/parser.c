#include "rea/parser.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    ReaLexer lexer;
    ReaToken current;
    ReaToken previous;
} ReaParser;

static void advance(ReaParser *p) {
    p->previous = p->current;
    p->current = reaNextToken(&p->lexer);
}

static int match(ReaParser *p, ReaTokenType type) {
    if (p->current.type != type) return 0;
    advance(p);
    return 1;
}

static ReaToken copyToken(const ReaToken *src) {
    ReaToken t = *src;
    t.start = NULL;
    if (src->start && src->length > 0) {
        char *lex = (char *)malloc(src->length + 1);
        if (lex) {
            memcpy(lex, src->start, src->length);
            lex[src->length] = '\0';
            t.start = lex;
        }
    }
    return t;
}

static ReaAST *parseExpression(ReaParser *p);

static ReaAST *parsePrimary(ReaParser *p) {
    if (match(p, REA_TOKEN_NUMBER)) {
        ReaAST *num = reaNewASTNode(REA_AST_NUMBER);
        num->token = copyToken(&p->previous);
        return num;
    }
    if (match(p, REA_TOKEN_LEFT_PAREN)) {
        ReaAST *expr = parseExpression(p);
        match(p, REA_TOKEN_RIGHT_PAREN); // best effort
        return expr;
    }
    return NULL;
}

static ReaAST *parseFactor(ReaParser *p) {
    ReaAST *node = parsePrimary(p);
    while (p->current.type == REA_TOKEN_STAR || p->current.type == REA_TOKEN_SLASH) {
        advance(p);
        ReaToken op = copyToken(&p->previous);
        ReaAST *right = parsePrimary(p);
        ReaAST *bin = reaNewASTNode(REA_AST_BINARY);
        bin->token = op;
        reaAddChild(bin, node);
        reaAddChild(bin, right);
        node = bin;
    }
    return node;
}

static ReaAST *parseTerm(ReaParser *p) {
    ReaAST *node = parseFactor(p);
    while (p->current.type == REA_TOKEN_PLUS || p->current.type == REA_TOKEN_MINUS) {
        advance(p);
        ReaToken op = copyToken(&p->previous);
        ReaAST *right = parseFactor(p);
        ReaAST *bin = reaNewASTNode(REA_AST_BINARY);
        bin->token = op;
        reaAddChild(bin, node);
        reaAddChild(bin, right);
        node = bin;
    }
    return node;
}

static ReaAST *parseExpression(ReaParser *p) {
    return parseTerm(p);
}

ReaAST *parseRea(const char *source) {
    ReaParser p;
    reaInitLexer(&p.lexer, source);
    p.current.type = REA_TOKEN_UNKNOWN;
    advance(&p);

    ReaAST *program = reaNewASTNode(REA_AST_PROGRAM);

    while (p.current.type != REA_TOKEN_EOF) {
        ReaAST *expr = parseExpression(&p);
        if (!expr) break;
        reaAddChild(program, expr);
        if (!match(&p, REA_TOKEN_SEMICOLON)) break;
    }

    return program;
}

