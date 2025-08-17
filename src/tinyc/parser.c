#include "tinyc/parser.h"
#include <stdio.h>
#include <stdlib.h>

static void advanceParser(ParserTinyC *p) {
    p->current = p->next;
    p->next = tinyc_nextToken(&p->lexer);
}

static int matchToken(ParserTinyC *p, TinyCTokenType type) {
    if (p->current.type == type) { advanceParser(p); return 1; }
    return 0;
}

static void expectToken(ParserTinyC *p, TinyCTokenType type, const char *msg) {
    if (!matchToken(p, type)) {
        fprintf(stderr, "Parse error at line %d: expected %s (%s)\n", p->current.line, msg, tinycTokenTypeToString(type));
    }
}

static ASTNodeTinyC* declaration(ParserTinyC *p);
static ASTNodeTinyC* varDeclaration(ParserTinyC *p, TinyCToken type_token, TinyCToken ident);
static ASTNodeTinyC* funDeclaration(ParserTinyC *p, TinyCToken type_token, TinyCToken ident);
static ASTNodeTinyC* params(ParserTinyC *p);
static ASTNodeTinyC* param(ParserTinyC *p);
static ASTNodeTinyC* compoundStmt(ParserTinyC *p);
static ASTNodeTinyC* statement(ParserTinyC *p);
static ASTNodeTinyC* expression(ParserTinyC *p);
static ASTNodeTinyC* assignment(ParserTinyC *p);
static ASTNodeTinyC* logicalOr(ParserTinyC *p);
static ASTNodeTinyC* logicalAnd(ParserTinyC *p);
static ASTNodeTinyC* equality(ParserTinyC *p);
static ASTNodeTinyC* relational(ParserTinyC *p);
static ASTNodeTinyC* additive(ParserTinyC *p);
static ASTNodeTinyC* term(ParserTinyC *p);
static ASTNodeTinyC* factor(ParserTinyC *p);
static ASTNodeTinyC* call(ParserTinyC *p, TinyCToken ident);
static ASTNodeTinyC* expressionStatement(ParserTinyC *p);
static ASTNodeTinyC* ifStatement(ParserTinyC *p);
static ASTNodeTinyC* whileStatement(ParserTinyC *p);
static ASTNodeTinyC* returnStatement(ParserTinyC *p);

void initParserTinyC(ParserTinyC *parser, const char *source) {
    tinyc_initLexer(&parser->lexer, source);
    parser->current = tinyc_nextToken(&parser->lexer);
    parser->next = tinyc_nextToken(&parser->lexer);
}

ASTNodeTinyC* parseProgramTinyC(ParserTinyC *parser) {
    ASTNodeTinyC *prog = newASTNodeTinyC(TCAST_PROGRAM, parser->current);
    while (parser->current.type != TINYCTOKEN_EOF) {
        ASTNodeTinyC *decl = declaration(parser);
        if (decl) addChildTinyC(prog, decl);
    }
    return prog;
}

static ASTNodeTinyC* declaration(ParserTinyC *p) {
    if (p->current.type == TINYCTOKEN_INT || p->current.type == TINYCTOKEN_VOID || p->current.type == TINYCTOKEN_STR) {
        TinyCToken type_tok = p->current; advanceParser(p);
        TinyCToken ident = p->current; expectToken(p, TINYCTOKEN_IDENTIFIER, "identifier");
        if (p->current.type == TINYCTOKEN_LPAREN) {
            return funDeclaration(p, type_tok, ident);
        } else {
            return varDeclaration(p, type_tok, ident);
        }
    }
    return NULL;
}

static ASTNodeTinyC* varDeclaration(ParserTinyC *p, TinyCToken type_token, TinyCToken ident) {
    ASTNodeTinyC *node = newASTNodeTinyC(TCAST_VAR_DECL, ident);
    if (matchToken(p, TINYCTOKEN_LBRACKET)) {
        TinyCToken num = p->current; expectToken(p, TINYCTOKEN_NUMBER, "array size");
        node->left = newASTNodeTinyC(TCAST_NUMBER, num);
        expectToken(p, TINYCTOKEN_RBRACKET, "]");
    }
    expectToken(p, TINYCTOKEN_SEMICOLON, ";");
    return node;
}

static ASTNodeTinyC* funDeclaration(ParserTinyC *p, TinyCToken type_token, TinyCToken ident) {
    expectToken(p, TINYCTOKEN_LPAREN, "(");
    ASTNodeTinyC *paramsNode = params(p);
    expectToken(p, TINYCTOKEN_RPAREN, ")");
    ASTNodeTinyC *body = compoundStmt(p);
    ASTNodeTinyC *node = newASTNodeTinyC(TCAST_FUN_DECL, ident);
    node->left = paramsNode;
    node->right = body;
    return node;
}

static ASTNodeTinyC* params(ParserTinyC *p) {
    /* allow both "void" and empty parameter lists */
    if (p->current.type == TINYCTOKEN_VOID) { advanceParser(p); return NULL; }
    if (p->current.type == TINYCTOKEN_RPAREN) { return NULL; }
    ASTNodeTinyC *paramList = newASTNodeTinyC(TCAST_PARAM, p->current);
    ASTNodeTinyC *first = param(p);
    addChildTinyC(paramList, first);
    while (matchToken(p, TINYCTOKEN_COMMA)) {
        ASTNodeTinyC *pr = param(p);
        addChildTinyC(paramList, pr);
    }
    return paramList;
}

static ASTNodeTinyC* param(ParserTinyC *p) {
    TinyCToken type_tok = p->current; advanceParser(p);
    TinyCToken ident = p->current; expectToken(p, TINYCTOKEN_IDENTIFIER, "param name");
    ASTNodeTinyC *node = newASTNodeTinyC(TCAST_PARAM, ident);
    node->left = newASTNodeTinyC(TCAST_IDENTIFIER, type_tok);
    return node;
}

static ASTNodeTinyC* compoundStmt(ParserTinyC *p) {
    expectToken(p, TINYCTOKEN_LBRACE, "{");
    ASTNodeTinyC *node = newASTNodeTinyC(TCAST_COMPOUND, p->current);
    while (p->current.type == TINYCTOKEN_INT || p->current.type == TINYCTOKEN_VOID || p->current.type == TINYCTOKEN_STR) {
        TinyCToken type_tok = p->current; advanceParser(p);
        TinyCToken ident = p->current; expectToken(p, TINYCTOKEN_IDENTIFIER, "identifier");
        ASTNodeTinyC *decl = varDeclaration(p, type_tok, ident);
        addChildTinyC(node, decl);
    }
    while (p->current.type != TINYCTOKEN_RBRACE && p->current.type != TINYCTOKEN_EOF) {
        ASTNodeTinyC *stmt = statement(p);
        if (stmt) addChildTinyC(node, stmt);
    }
    expectToken(p, TINYCTOKEN_RBRACE, "}");
    return node;
}

static ASTNodeTinyC* statement(ParserTinyC *p) {
    switch (p->current.type) {
        case TINYCTOKEN_IF: return ifStatement(p);
        case TINYCTOKEN_WHILE: return whileStatement(p);
        case TINYCTOKEN_RETURN: return returnStatement(p);
        case TINYCTOKEN_LBRACE: return compoundStmt(p);
        default: return expressionStatement(p);
    }
}

static ASTNodeTinyC* ifStatement(ParserTinyC *p) {
    expectToken(p, TINYCTOKEN_IF, "if");
    expectToken(p, TINYCTOKEN_LPAREN, "(");
    ASTNodeTinyC *cond = expression(p);
    expectToken(p, TINYCTOKEN_RPAREN, ")");
    ASTNodeTinyC *thenBranch = statement(p);
    ASTNodeTinyC *elseBranch = NULL;
    if (matchToken(p, TINYCTOKEN_ELSE)) {
        elseBranch = statement(p);
    }
    ASTNodeTinyC *node = newASTNodeTinyC(TCAST_IF, p->current);
    node->left = cond;
    node->right = thenBranch;
    node->third = elseBranch;
    return node;
}

static ASTNodeTinyC* whileStatement(ParserTinyC *p) {
    expectToken(p, TINYCTOKEN_WHILE, "while");
    expectToken(p, TINYCTOKEN_LPAREN, "(");
    ASTNodeTinyC *cond = expression(p);
    expectToken(p, TINYCTOKEN_RPAREN, ")");
    ASTNodeTinyC *body = statement(p);
    ASTNodeTinyC *node = newASTNodeTinyC(TCAST_WHILE, p->current);
    node->left = cond;
    node->right = body;
    return node;
}

static ASTNodeTinyC* returnStatement(ParserTinyC *p) {
    expectToken(p, TINYCTOKEN_RETURN, "return");
    ASTNodeTinyC *expr = NULL;
    if (p->current.type != TINYCTOKEN_SEMICOLON) {
        expr = expression(p);
    }
    expectToken(p, TINYCTOKEN_SEMICOLON, ";");
    ASTNodeTinyC *node = newASTNodeTinyC(TCAST_RETURN, p->current);
    node->left = expr;
    return node;
}

static ASTNodeTinyC* expressionStatement(ParserTinyC *p) {
    if (p->current.type == TINYCTOKEN_SEMICOLON) {
        advanceParser(p);
        return newASTNodeTinyC(TCAST_EXPR_STMT, p->current);
    }
    ASTNodeTinyC *expr = expression(p);
    expectToken(p, TINYCTOKEN_SEMICOLON, ";");
    ASTNodeTinyC *node = newASTNodeTinyC(TCAST_EXPR_STMT, p->current);
    node->left = expr;
    return node;
}

static ASTNodeTinyC* expression(ParserTinyC *p) { return assignment(p); }

static ASTNodeTinyC* assignment(ParserTinyC *p) {
    ASTNodeTinyC *node = logicalOr(p);
    if (p->current.type == TINYCTOKEN_EQUAL) {
        TinyCToken op = p->current; advanceParser(p);
        ASTNodeTinyC *right = assignment(p);
        ASTNodeTinyC *assign = newASTNodeTinyC(TCAST_ASSIGN, op);
        assign->left = node;
        assign->right = right;
        return assign;
    }
    return node;
}

static ASTNodeTinyC* logicalOr(ParserTinyC *p) {
    ASTNodeTinyC *node = logicalAnd(p);
    while (p->current.type == TINYCTOKEN_OR_OR) {
        TinyCToken op = p->current; advanceParser(p);
        ASTNodeTinyC *rhs = logicalAnd(p);
        ASTNodeTinyC *bin = newASTNodeTinyC(TCAST_BINOP, op);
        bin->left = node; bin->right = rhs; node = bin;
    }
    return node;
}

static ASTNodeTinyC* logicalAnd(ParserTinyC *p) {
    ASTNodeTinyC *node = equality(p);
    while (p->current.type == TINYCTOKEN_AND_AND) {
        TinyCToken op = p->current; advanceParser(p);
        ASTNodeTinyC *rhs = equality(p);
        ASTNodeTinyC *bin = newASTNodeTinyC(TCAST_BINOP, op);
        bin->left = node; bin->right = rhs; node = bin;
    }
    return node;
}

static ASTNodeTinyC* equality(ParserTinyC *p) {
    ASTNodeTinyC *node = relational(p);
    while (p->current.type == TINYCTOKEN_EQUAL_EQUAL || p->current.type == TINYCTOKEN_BANG_EQUAL) {
        TinyCToken op = p->current; advanceParser(p);
        ASTNodeTinyC *rhs = relational(p);
        ASTNodeTinyC *bin = newASTNodeTinyC(TCAST_BINOP, op);
        bin->left = node; bin->right = rhs; node = bin;
    }
    return node;
}

static ASTNodeTinyC* relational(ParserTinyC *p) {
    ASTNodeTinyC *node = additive(p);
    while (p->current.type == TINYCTOKEN_LESS || p->current.type == TINYCTOKEN_LESS_EQUAL ||
           p->current.type == TINYCTOKEN_GREATER || p->current.type == TINYCTOKEN_GREATER_EQUAL) {
        TinyCToken op = p->current; advanceParser(p);
        ASTNodeTinyC *rhs = additive(p);
        ASTNodeTinyC *bin = newASTNodeTinyC(TCAST_BINOP, op);
        bin->left = node; bin->right = rhs; node = bin;
    }
    return node;
}

static ASTNodeTinyC* additive(ParserTinyC *p) {
    ASTNodeTinyC *node = term(p);
    while (p->current.type == TINYCTOKEN_PLUS || p->current.type == TINYCTOKEN_MINUS) {
        TinyCToken op = p->current; advanceParser(p);
        ASTNodeTinyC *rhs = term(p);
        ASTNodeTinyC *bin = newASTNodeTinyC(TCAST_BINOP, op);
        bin->left = node; bin->right = rhs; node = bin;
    }
    return node;
}

static ASTNodeTinyC* term(ParserTinyC *p) {
    ASTNodeTinyC *node = factor(p);
    while (p->current.type == TINYCTOKEN_STAR || p->current.type == TINYCTOKEN_SLASH) {
        TinyCToken op = p->current; advanceParser(p);
        ASTNodeTinyC *rhs = factor(p);
        ASTNodeTinyC *bin = newASTNodeTinyC(TCAST_BINOP, op);
        bin->left = node; bin->right = rhs; node = bin;
    }
    return node;
}

static ASTNodeTinyC* factor(ParserTinyC *p) {
    if (matchToken(p, TINYCTOKEN_LPAREN)) {
        ASTNodeTinyC *expr = expression(p);
        expectToken(p, TINYCTOKEN_RPAREN, ")");
        return expr;
    }
    if (p->current.type == TINYCTOKEN_NUMBER) {
        TinyCToken num = p->current; advanceParser(p);
        return newASTNodeTinyC(TCAST_NUMBER, num);
    }
    if (p->current.type == TINYCTOKEN_STRING) {
        TinyCToken str = p->current; advanceParser(p);
        return newASTNodeTinyC(TCAST_STRING, str);
    }
    if (p->current.type == TINYCTOKEN_IDENTIFIER) {
        TinyCToken ident = p->current; advanceParser(p);
        if (p->current.type == TINYCTOKEN_LPAREN) {
            return call(p, ident);
        }
        return newASTNodeTinyC(TCAST_IDENTIFIER, ident);
    }
    fprintf(stderr, "Unexpected token %s at line %d\n", tinycTokenTypeToString(p->current.type), p->current.line);
    advanceParser(p);
    return newASTNodeTinyC(TCAST_NUMBER, p->current); // error node
}

static ASTNodeTinyC* call(ParserTinyC *p, TinyCToken ident) {
    expectToken(p, TINYCTOKEN_LPAREN, "(");
    ASTNodeTinyC *node = newASTNodeTinyC(TCAST_CALL, ident);
    if (p->current.type != TINYCTOKEN_RPAREN) {
        ASTNodeTinyC *arg = expression(p);
        addChildTinyC(node, arg);
        while (matchToken(p, TINYCTOKEN_COMMA)) {
            ASTNodeTinyC *argn = expression(p);
            addChildTinyC(node, argn);
        }
    }
    expectToken(p, TINYCTOKEN_RPAREN, ")");
    return node;
}
