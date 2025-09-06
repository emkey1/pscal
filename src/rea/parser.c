#include "rea/parser.h"
#include "ast/ast.h"
#include "core/types.h"
#include "symbol/symbol.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Forward declaration from core/utils.c
Token *newToken(TokenType type, const char *value, int line, int column);
// Provided by front-end stubs for Rea
void insertType(const char* name, AST* typeDef);

typedef struct {
    ReaLexer lexer;
    ReaToken current;
    VarType currentFunctionType;
    bool hadError;
    const char* currentClassName; // non-owning pointer to current class name while parsing class body
} ReaParser;

static void reaAdvance(ReaParser *p) { p->current = reaNextToken(&p->lexer); }

static AST *parseExpression(ReaParser *p);
static AST *parseAssignment(ReaParser *p);
static AST *parseEquality(ReaParser *p);
static AST *parseComparison(ReaParser *p);
static AST *parseAdditive(ReaParser *p);
static AST *parseTerm(ReaParser *p);
static AST *parseFactor(ReaParser *p);
static AST *parseStatement(ReaParser *p);
static AST *parseVarDecl(ReaParser *p);
static AST *parseReturn(ReaParser *p);
static AST *parseIf(ReaParser *p);
static AST *parseWhile(ReaParser *p);
static AST *parseFor(ReaParser *p);
static AST *parseDoWhile(ReaParser *p);
static AST *parseSwitch(ReaParser *p);
static AST *parseBlock(ReaParser *p);
static AST *parseFunctionDecl(ReaParser *p, Token *nameTok, AST *typeNode, VarType vtype);

static AST *parseFactor(ReaParser *p) {
    if (p->current.type == REA_TOKEN_NEW) {
        // new ClassName(args)
        ReaToken newTok = p->current;
        reaAdvance(p); // consume 'new'
        if (p->current.type != REA_TOKEN_IDENTIFIER) return NULL;
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *ctorTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);
        reaAdvance(p); // consume class name

        AST *call_args = NULL;
        if (p->current.type == REA_TOKEN_LEFT_PAREN) {
            reaAdvance(p);
            call_args = newASTNode(AST_COMPOUND, NULL);
            while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
                AST *arg = parseExpression(p);
                if (!arg) break;
                addChild(call_args, arg);
                if (p->current.type == REA_TOKEN_COMMA) reaAdvance(p); else break;
            }
            if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);
        }
        AST *call = newASTNode(AST_PROCEDURE_CALL, ctorTok);
        if (call_args && call_args->child_count > 0) {
            call->children = call_args->children;
            call->child_count = call_args->child_count;
            call->child_capacity = call_args->child_capacity;
            for (int i = 0; i < call->child_count; i++) if (call->children[i]) call->children[i]->parent = call;
            call_args->children = NULL; call_args->child_count = 0; call_args->child_capacity = 0;
        }
        if (call_args) freeAST(call_args);
        setTypeAST(call, TYPE_UNKNOWN);
        return call;
    }
    if (p->current.type == REA_TOKEN_NUMBER) {
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';

        TokenType ttype = TOKEN_INTEGER_CONST;
        VarType vtype = TYPE_INT64;
        bool is_real = false;
        for (size_t i = 0; i < p->current.length; i++) {
            if (lex[i] == '.' || lex[i] == 'e' || lex[i] == 'E') {
                is_real = true;
                break;
            }
        }
        if (is_real) {
            ttype = TOKEN_REAL_CONST;
            vtype = TYPE_DOUBLE;
        }

        Token *tok = newToken(ttype, lex, p->current.line, 0);
        free(lex);
        AST *node = newASTNode(AST_NUMBER, tok);
        setTypeAST(node, vtype);
        reaAdvance(p);
        return node;
    } else if (p->current.type == REA_TOKEN_STRING) {
        size_t len = p->current.length;
        if (len < 2) return NULL;
        char *lex = (char *)malloc(len - 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start + 1, len - 2);
        lex[len - 2] = '\0';
        Token *tok = newToken(TOKEN_STRING_CONST, lex, p->current.line, 0);
        free(lex);
        AST *node = newASTNode(AST_STRING, tok);
        setTypeAST(node, TYPE_STRING);
        reaAdvance(p);
        return node;
    } else if (p->current.type == REA_TOKEN_TRUE || p->current.type == REA_TOKEN_FALSE) {
        TokenType tt = (p->current.type == REA_TOKEN_TRUE) ? TOKEN_TRUE : TOKEN_FALSE;
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *tok = newToken(tt, lex, p->current.line, 0);
        free(lex);
        AST *node = newASTNode(AST_BOOLEAN, tok);
        setTypeAST(node, TYPE_BOOLEAN);
        node->i_val = (tt == TOKEN_TRUE) ? 1 : 0;
        reaAdvance(p);
        return node;
    } else if (p->current.type == REA_TOKEN_IDENTIFIER || p->current.type == REA_TOKEN_THIS) {
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        if (p->current.type == REA_TOKEN_THIS) {
            strcpy(lex, "this");
        } else {
            memcpy(lex, p->current.start, p->current.length);
            lex[p->current.length] = '\0';
        }
        Token *tok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);
        reaAdvance(p); // consume identifier

        AST *call_args = NULL;
        if (p->current.type == REA_TOKEN_LEFT_PAREN) {
            reaAdvance(p); // consume '('
            call_args = newASTNode(AST_COMPOUND, NULL);
            while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
                AST *arg = parseExpression(p);
                if (!arg) break;
                addChild(call_args, arg);
                if (p->current.type == REA_TOKEN_COMMA) {
                    reaAdvance(p);
                } else {
                    break;
                }
            }
            if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
                reaAdvance(p);
            }
            AST *call = newASTNode(AST_PROCEDURE_CALL, tok);
            if (call_args && call_args->child_count > 0) {
                call->children = call_args->children;
                call->child_count = call_args->child_count;
                call->child_capacity = call_args->child_capacity;
                for (int i = 0; i < call->child_count; i++) {
                    if (call->children[i]) call->children[i]->parent = call;
                }
                call_args->children = NULL;
                call_args->child_count = 0;
                call_args->child_capacity = 0;
            }
            if (call_args) freeAST(call_args);
            setTypeAST(call, TYPE_UNKNOWN);
            // Support member access chaining after call
            AST *node = call;
            while (p->current.type == REA_TOKEN_DOT) {
                reaAdvance(p);
                if (p->current.type != REA_TOKEN_IDENTIFIER) break;
                char *f = (char*)malloc(p->current.length + 1);
                if (!f) break;
                memcpy(f, p->current.start, p->current.length);
                f[p->current.length] = '\0';
                Token *fieldTok = newToken(TOKEN_IDENTIFIER, f, p->current.line, 0);
                free(f);
                reaAdvance(p);
                AST *fieldVar = newASTNode(AST_VARIABLE, fieldTok);
                AST *fa = newASTNode(AST_FIELD_ACCESS, fieldTok);
                setLeft(fa, node);
                setRight(fa, fieldVar);
                node = fa;
            }
            return node;
        } else {
            AST *node = newASTNode(AST_VARIABLE, tok);
            setTypeAST(node, TYPE_UNKNOWN);
            while (p->current.type == REA_TOKEN_DOT) {
                reaAdvance(p);
                if (p->current.type != REA_TOKEN_IDENTIFIER) break;
                char *f = (char*)malloc(p->current.length + 1);
                if (!f) break;
                memcpy(f, p->current.start, p->current.length);
                f[p->current.length] = '\0';
                Token *fieldTok = newToken(TOKEN_IDENTIFIER, f, p->current.line, 0);
                free(f);
                reaAdvance(p);
                AST *fieldVar = newASTNode(AST_VARIABLE, fieldTok);
                AST *fa = newASTNode(AST_FIELD_ACCESS, fieldTok);
                setLeft(fa, node);
                setRight(fa, fieldVar);
                node = fa;
            }
            return node;
        }
    } else if (p->current.type == REA_TOKEN_MINUS) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseFactor(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_MINUS, "-", op.line, 0);
        AST *node = newASTNode(AST_UNARY_OP, tok);
        setLeft(node, right);
        setTypeAST(node, right->var_type);
        return node;
    } else if (p->current.type == REA_TOKEN_LEFT_PAREN) {
        reaAdvance(p);
        AST *expr = parseExpression(p);
        if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
            reaAdvance(p);
        }
        return expr;
    }
    return NULL;
}

static TokenType mapOp(ReaTokenType t) {
    switch (t) {
        case REA_TOKEN_PLUS: return TOKEN_PLUS;
        case REA_TOKEN_MINUS: return TOKEN_MINUS;
        case REA_TOKEN_STAR: return TOKEN_MUL;
        case REA_TOKEN_SLASH: return TOKEN_SLASH;
        case REA_TOKEN_EQUAL_EQUAL: return TOKEN_EQUAL;
        case REA_TOKEN_BANG_EQUAL: return TOKEN_NOT_EQUAL;
        case REA_TOKEN_GREATER: return TOKEN_GREATER;
        case REA_TOKEN_GREATER_EQUAL: return TOKEN_GREATER_EQUAL;
        case REA_TOKEN_LESS: return TOKEN_LESS;
        case REA_TOKEN_LESS_EQUAL: return TOKEN_LESS_EQUAL;
        default: return TOKEN_UNKNOWN;
    }
}

static const char *opLexeme(TokenType t) {
    switch (t) {
        case TOKEN_PLUS: return "+";
        case TOKEN_MINUS: return "-";
        case TOKEN_MUL: return "*";
        case TOKEN_SLASH: return "/";
        case TOKEN_EQUAL: return "==";
        case TOKEN_NOT_EQUAL: return "!=";
        case TOKEN_GREATER: return ">";
        case TOKEN_GREATER_EQUAL: return ">=";
        case TOKEN_LESS: return "<";
        case TOKEN_LESS_EQUAL: return "<=";
        default: return "";
    }
}

static AST *parseTerm(ReaParser *p) {
    AST *node = parseFactor(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_STAR || p->current.type == REA_TOKEN_SLASH) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseFactor(p);
        if (!right) return NULL;
        TokenType tt = mapOp(op.type);
        Token *tok = newToken(tt, opLexeme(tt), op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        VarType lt = node->var_type;
        VarType rt = right->var_type;
        VarType res;
        if (tt == TOKEN_SLASH || lt == TYPE_DOUBLE || rt == TYPE_DOUBLE) {
            res = TYPE_DOUBLE;
        } else if (lt == TYPE_INT64 || rt == TYPE_INT64) {
            res = TYPE_INT64;
        } else {
            res = TYPE_INT32;
        }
        setTypeAST(bin, res);
        node = bin;
    }
    return node;
}

static AST *parseAdditive(ReaParser *p) {
    AST *node = parseTerm(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_PLUS || p->current.type == REA_TOKEN_MINUS) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseTerm(p);
        if (!right) return NULL;
        TokenType tt = mapOp(op.type);
        Token *tok = newToken(tt, opLexeme(tt), op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        VarType lt = node->var_type;
        VarType rt = right->var_type;
        VarType res;
        if (lt == TYPE_DOUBLE || rt == TYPE_DOUBLE) {
            res = TYPE_DOUBLE;
        } else if (lt == TYPE_INT64 || rt == TYPE_INT64) {
            res = TYPE_INT64;
        } else {
            res = TYPE_INT32;
        }
        setTypeAST(bin, res);
        node = bin;
    }
    return node;
}

static AST *parseComparison(ReaParser *p) {
    AST *node = parseAdditive(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_GREATER || p->current.type == REA_TOKEN_GREATER_EQUAL ||
           p->current.type == REA_TOKEN_LESS || p->current.type == REA_TOKEN_LESS_EQUAL) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseAdditive(p);
        if (!right) return NULL;
        TokenType tt = mapOp(op.type);
        Token *tok = newToken(tt, opLexeme(tt), op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        setTypeAST(bin, TYPE_BOOLEAN);
        node = bin;
    }
    return node;
}

static AST *parseEquality(ReaParser *p) {
    AST *node = parseComparison(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_EQUAL_EQUAL || p->current.type == REA_TOKEN_BANG_EQUAL) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseComparison(p);
        if (!right) return NULL;
        TokenType tt = mapOp(op.type);
        Token *tok = newToken(tt, opLexeme(tt), op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        setTypeAST(bin, TYPE_BOOLEAN);
        node = bin;
    }
    return node;
}

static AST *parseAssignment(ReaParser *p) {
    AST *left = parseEquality(p);
    if (!left) return NULL;
    if ((left->type == AST_VARIABLE || left->type == AST_FIELD_ACCESS) && p->current.type == REA_TOKEN_EQUAL) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *value = parseAssignment(p);
        if (!value) return NULL;
        Token *tok = newToken(TOKEN_ASSIGN, "=", op.line, 0);
            AST *node = newASTNode(AST_ASSIGN, tok);
            setLeft(node, left);
            setRight(node, value);
            setTypeAST(node, left->var_type);
            return node;
    }
    return left;
}

static AST *parseExpression(ReaParser *p) {
    return parseAssignment(p);
}

static VarType mapType(ReaTokenType t) {
    switch (t) {
        case REA_TOKEN_INT: return TYPE_INT64;
        case REA_TOKEN_FLOAT: return TYPE_DOUBLE;
        case REA_TOKEN_STR: return TYPE_STRING;
        case REA_TOKEN_BOOL: return TYPE_BOOLEAN;
        default: return TYPE_VOID;
    }
}

static const char *typeName(ReaTokenType t) {
    switch (t) {
        case REA_TOKEN_INT: return "int";
        case REA_TOKEN_FLOAT: return "float";
        case REA_TOKEN_STR: return "str";
        case REA_TOKEN_BOOL: return "bool";
        default: return "";
    }
}

static AST *parseVarDecl(ReaParser *p) {
    ReaTokenType typeTok = p->current.type;
    VarType vtype = mapType(typeTok);

    // Build a type identifier node for the declaration's type.
    const char *tname = typeName(typeTok);
    Token *typeToken = newToken(TOKEN_IDENTIFIER, tname, p->current.line, 0);
    AST *typeNode = newASTNode(AST_TYPE_IDENTIFIER, typeToken);
    setTypeAST(typeNode, vtype);

    reaAdvance(p); // consume type keyword

    if (p->current.type != REA_TOKEN_IDENTIFIER) return NULL;

    char *lex = (char *)malloc(p->current.length + 1);
    if (!lex) return NULL;
    memcpy(lex, p->current.start, p->current.length);
    lex[p->current.length] = '\0';
    Token *nameTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
    free(lex);

    reaAdvance(p); // consume identifier

    if (p->current.type == REA_TOKEN_LEFT_PAREN) {
        return parseFunctionDecl(p, nameTok, typeNode, vtype);
    }

    AST *var = newASTNode(AST_VARIABLE, nameTok);
    setTypeAST(var, vtype);

    AST *init = NULL;
    if (p->current.type == REA_TOKEN_EQUAL) {
        reaAdvance(p);
        init = parseExpression(p);
    }

    if (p->current.type == REA_TOKEN_SEMICOLON) {
        reaAdvance(p);
    }

    AST *decl = newASTNode(AST_VAR_DECL, NULL);
    addChild(decl, var);
    setLeft(decl, init);
    setRight(decl, typeNode);
    setTypeAST(decl, vtype);
    return decl;
}

static AST *parseFunctionDecl(ReaParser *p, Token *nameTok, AST *typeNode, VarType vtype) {
    VarType prevType = p->currentFunctionType;
    p->currentFunctionType = vtype;

    // If inside a class, mangle function name to ClassName_method
    if (p->currentClassName && nameTok && nameTok->value) {
        size_t ln = strlen(p->currentClassName) + 1 + strlen(nameTok->value) + 1;
        char *m = (char*)malloc(ln);
        if (m) {
            snprintf(m, ln, "%s_%s", p->currentClassName, nameTok->value);
            free(nameTok->value);
            nameTok->value = m;
        }
    }

    // Parse parameter list
    reaAdvance(p); // consume '('
    AST *params = newASTNode(AST_COMPOUND, NULL);
    // Inject implicit 'this' parameter as first parameter when inside a class
    if (p->currentClassName) {
        Token *ptypeTok = newToken(TOKEN_IDENTIFIER, p->currentClassName, p->current.line, 0);
        AST *ptypeNode = newASTNode(AST_TYPE_IDENTIFIER, ptypeTok);
        setTypeAST(ptypeNode, TYPE_RECORD); // annotate later based on actual type table
        Token *thisTok = newToken(TOKEN_IDENTIFIER, "this", p->current.line, 0);
        AST *thisVar = newASTNode(AST_VARIABLE, thisTok);
        setTypeAST(thisVar, TYPE_RECORD);
        AST *thisDecl = newASTNode(AST_VAR_DECL, NULL);
        addChild(thisDecl, thisVar);
        setRight(thisDecl, ptypeNode);
        setTypeAST(thisDecl, TYPE_RECORD);
        addChild(params, thisDecl);
    }
    while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
        ReaTokenType paramTypeTok = p->current.type;
        VarType pvtype = mapType(paramTypeTok);
        const char *ptname = typeName(paramTypeTok);
        Token *ptypeTok = newToken(TOKEN_IDENTIFIER, ptname, p->current.line, 0);
        AST *ptypeNode = newASTNode(AST_TYPE_IDENTIFIER, ptypeTok);
        setTypeAST(ptypeNode, pvtype);
        reaAdvance(p); // consume param type

        if (p->current.type != REA_TOKEN_IDENTIFIER) break;
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) break;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *paramNameTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);
        AST *paramVar = newASTNode(AST_VARIABLE, paramNameTok);
        setTypeAST(paramVar, pvtype);
        reaAdvance(p); // consume param name

        AST *paramDecl = newASTNode(AST_VAR_DECL, NULL);
        addChild(paramDecl, paramVar);
        setRight(paramDecl, ptypeNode);
        setTypeAST(paramDecl, pvtype);
        addChild(params, paramDecl);

        if (p->current.type == REA_TOKEN_COMMA) {
            reaAdvance(p);
        } else {
            break;
        }
    }
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        reaAdvance(p);
    }

    // Parse function body
    AST *block = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        reaAdvance(p); // consume '{'
        AST *decls = newASTNode(AST_COMPOUND, NULL);
        AST *stmts = newASTNode(AST_COMPOUND, NULL);
        while (p->current.type != REA_TOKEN_RIGHT_BRACE && p->current.type != REA_TOKEN_EOF) {
            AST *stmt = parseStatement(p);
            if (!stmt) break;
            if (stmt->type == AST_VAR_DECL) {
                addChild(decls, stmt);
            } else {
                addChild(stmts, stmt);
            }
        }
        if (p->current.type == REA_TOKEN_RIGHT_BRACE) {
            reaAdvance(p);
        }
        block = newASTNode(AST_BLOCK, NULL);
        addChild(block, decls);
        addChild(block, stmts);
    }

    AST *func = newASTNode(AST_FUNCTION_DECL, nameTok);
    if (params->child_count > 0) {
        func->children = params->children;
        func->child_count = params->child_count;
        func->child_capacity = params->child_capacity;
        for (int i = 0; i < func->child_count; i++) {
            if (func->children[i]) func->children[i]->parent = func;
        }
        params->children = NULL;
        params->child_count = 0;
        params->child_capacity = 0;
    }
    freeAST(params);
    setRight(func, typeNode);
    setExtra(func, block);
    setTypeAST(func, vtype);

    // Register function in procedure table
    Symbol *sym = (Symbol*)malloc(sizeof(Symbol));
    if (sym) {
        memset(sym, 0, sizeof(Symbol));
        sym->name = strdup(nameTok->value);
        if (sym->name) {
            for (int i = 0; sym->name[i]; i++) {
                sym->name[i] = tolower((unsigned char)sym->name[i]);
            }
        }
        sym->type = vtype;
        sym->type_def = copyAST(func);
        hashTableInsert(procedure_table, sym);
    }

    p->currentFunctionType = prevType;
    return func;
}

static AST *parseBlock(ReaParser *p) {
    if (p->current.type != REA_TOKEN_LEFT_BRACE) return NULL;
    reaAdvance(p); // consume '{'
    AST *block = newASTNode(AST_COMPOUND, NULL);
    while (p->current.type != REA_TOKEN_RIGHT_BRACE && p->current.type != REA_TOKEN_EOF) {
        AST *stmt = parseStatement(p);
        if (!stmt) break;
        addChild(block, stmt);
    }
    if (p->current.type == REA_TOKEN_RIGHT_BRACE) {
        reaAdvance(p);
    }
    return block;
}

static AST *parseReturn(ReaParser *p) {
    ReaToken ret = p->current;
    reaAdvance(p); // consume 'return'
    AST *value = NULL;
    if (p->current.type != REA_TOKEN_SEMICOLON) {
        value = parseExpression(p);
    } else if (p->currentFunctionType != TYPE_VOID) {
        fprintf(stderr, "L%d: return requires a value.\n", ret.line);
        p->hadError = true;
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        reaAdvance(p);
    }
    if (p->hadError) return NULL;
    Token *retTok = newToken(TOKEN_RETURN, "return", ret.line, 0);
    AST *node = newASTNode(AST_RETURN, retTok);
    setLeft(node, value);
    if (value) setTypeAST(node, value->var_type); else setTypeAST(node, TYPE_VOID);
    return node;
}

static AST *parseIf(ReaParser *p) {
    reaAdvance(p); // consume 'if'
    if (p->current.type == REA_TOKEN_LEFT_PAREN) {
        reaAdvance(p);
    }
    AST *condition = parseExpression(p);
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        reaAdvance(p);
    }

    AST *thenBranch = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        thenBranch = parseBlock(p);
    } else {
        thenBranch = parseStatement(p);
    }

    AST *elseBranch = NULL;
    if (p->current.type == REA_TOKEN_ELSE) {
        reaAdvance(p);
        if (p->current.type == REA_TOKEN_LEFT_BRACE) {
            elseBranch = parseBlock(p);
        } else {
            elseBranch = parseStatement(p);
        }
    }

    AST *node = newASTNode(AST_IF, NULL);
    setLeft(node, condition);
    setRight(node, thenBranch);
    setExtra(node, elseBranch);
    return node;
}

static AST *parseWhile(ReaParser *p) {
    reaAdvance(p); // consume 'while'
    if (p->current.type == REA_TOKEN_LEFT_PAREN) {
        reaAdvance(p);
    }
    AST *condition = parseExpression(p);
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        reaAdvance(p);
    }
    AST *body = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        body = parseBlock(p);
    } else {
        body = parseStatement(p);
    }
    AST *node = newASTNode(AST_WHILE, NULL);
    setLeft(node, condition);
    setRight(node, body);
    return node;
}

static AST *parseDoWhile(ReaParser *p) {
    reaAdvance(p); // consume 'do'
    AST *body = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        body = parseBlock(p);
    } else {
        body = parseStatement(p);
    }
    if (p->current.type == REA_TOKEN_WHILE) {
        reaAdvance(p);
    }
    if (p->current.type == REA_TOKEN_LEFT_PAREN) {
        reaAdvance(p);
    }
    AST *condition = parseExpression(p);
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        reaAdvance(p);
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        reaAdvance(p);
    }
    AST *node = newASTNode(AST_REPEAT, NULL);
    setLeft(node, body);
    setRight(node, condition);
    return node;
}

static AST *parseFor(ReaParser *p) {
    reaAdvance(p); // consume 'for'
    if (p->current.type != REA_TOKEN_LEFT_PAREN) return NULL;
    reaAdvance(p); // consume '('

    if (p->current.type != REA_TOKEN_IDENTIFIER) return NULL;
    char *lex = (char *)malloc(p->current.length + 1);
    if (!lex) return NULL;
    memcpy(lex, p->current.start, p->current.length);
    lex[p->current.length] = '\0';
    Token *varTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
    free(lex);
    reaAdvance(p); // consume identifier

    if (p->current.type != REA_TOKEN_EQUAL) return NULL;
    reaAdvance(p); // consume '='
    AST *startExpr = parseExpression(p);
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        reaAdvance(p);
    }

    // Condition: skip optional loop variable and comparison operator
    if (p->current.type == REA_TOKEN_IDENTIFIER) {
        reaAdvance(p);
        if (p->current.type == REA_TOKEN_LESS || p->current.type == REA_TOKEN_LESS_EQUAL ||
            p->current.type == REA_TOKEN_GREATER || p->current.type == REA_TOKEN_GREATER_EQUAL) {
            reaAdvance(p);
        }
    }
    AST *endExpr = parseExpression(p);
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        reaAdvance(p);
    }

    // Increment part (ignored, but parsed for correctness)
    AST *inc = parseExpression(p);
    if (inc) freeAST(inc);

    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        reaAdvance(p);
    }

    AST *body = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        body = parseBlock(p);
    } else {
        body = parseStatement(p);
    }

    AST *varNode = newASTNode(AST_VARIABLE, varTok);
    AST *node = newASTNode(AST_FOR_TO, NULL);
    setLeft(node, startExpr);
    setRight(node, endExpr);
    setExtra(node, body);
    addChild(node, varNode);
    return node;
}

static AST *parseSwitch(ReaParser *p) {
    reaAdvance(p); // consume 'switch'
    if (p->current.type == REA_TOKEN_LEFT_PAREN) {
        reaAdvance(p);
    }
    AST *expr = parseExpression(p);
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        reaAdvance(p);
    }
    if (p->current.type != REA_TOKEN_LEFT_BRACE) return NULL;
    reaAdvance(p); // consume '{'

    AST *node = newASTNode(AST_CASE, NULL);
    setLeft(node, expr);

    while (p->current.type != REA_TOKEN_RIGHT_BRACE && p->current.type != REA_TOKEN_EOF) {
        if (p->current.type == REA_TOKEN_CASE) {
            reaAdvance(p);
            AST *label = parseExpression(p);
            if (p->current.type == REA_TOKEN_COLON) {
                reaAdvance(p);
            }
            AST *stmt = NULL;
            if (p->current.type == REA_TOKEN_LEFT_BRACE) {
                stmt = parseBlock(p);
            } else {
                stmt = parseStatement(p);
            }
            AST *branch = newASTNode(AST_CASE_BRANCH, NULL);
            setLeft(branch, label);
            setRight(branch, stmt);
            addChild(node, branch);
            if (p->current.type == REA_TOKEN_SEMICOLON) {
                reaAdvance(p);
            }
        } else if (p->current.type == REA_TOKEN_DEFAULT) {
            reaAdvance(p);
            if (p->current.type == REA_TOKEN_COLON) {
                reaAdvance(p);
            }
            AST *stmt = NULL;
            if (p->current.type == REA_TOKEN_LEFT_BRACE) {
                stmt = parseBlock(p);
            } else {
                stmt = parseStatement(p);
            }
            setExtra(node, stmt);
            if (p->current.type == REA_TOKEN_SEMICOLON) {
                reaAdvance(p);
            }
        } else {
            reaAdvance(p);
        }
    }
    if (p->current.type == REA_TOKEN_RIGHT_BRACE) {
        reaAdvance(p);
    }
    return node;
}

static AST *parseStatement(ReaParser *p) {
    if (p->current.type == REA_TOKEN_CLASS) {
        // class Name { field declarations ; ... }
        reaAdvance(p); // consume 'class'
        if (p->current.type != REA_TOKEN_IDENTIFIER) return NULL;
        // class name token
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *classNameTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);
        reaAdvance(p); // consume class name

        // Optional 'extends' <Identifier> (ignored in phase 1)
        if (p->current.type == REA_TOKEN_EXTENDS) {
            reaAdvance(p); // consume extends
            if (p->current.type == REA_TOKEN_IDENTIFIER) {
                // Skip parent name for now
                reaAdvance(p);
            }
        }

        // Parse class body
        AST *recordAst = newASTNode(AST_RECORD_TYPE, NULL);
        AST *methods = newASTNode(AST_COMPOUND, NULL);
        const char* prevClass = p->currentClassName;
        p->currentClassName = classNameTok->value;
        if (p->current.type == REA_TOKEN_LEFT_BRACE) {
            reaAdvance(p); // consume '{'
            while (p->current.type != REA_TOKEN_RIGHT_BRACE && p->current.type != REA_TOKEN_EOF) {
                // Field or method: both start with a type keyword
                if (p->current.type == REA_TOKEN_INT || p->current.type == REA_TOKEN_FLOAT ||
                    p->current.type == REA_TOKEN_STR || p->current.type == REA_TOKEN_BOOL) {
                    // Peek ahead to decide var vs method based on presence of '('
                    ReaToken save = p->current;
                    // Consume type and identifier into a fake varDecl to reuse existing parser
                    AST *decl = parseVarDecl(p);
                    if (decl) {
                        if (decl->type == AST_FUNCTION_DECL) {
                            addChild(methods, decl);
                        } else {
                            addChild(recordAst, decl);
                        }
                    } else {
                        // Recovery: advance one token to avoid infinite loop
                        reaAdvance(p);
                    }
                } else {
                    // Skip unsupported constructs for now
                    reaAdvance(p);
                }
                // optional semicolons are consumed by parseVarDecl; any extras will be skipped
            }
            if (p->current.type == REA_TOKEN_RIGHT_BRACE) reaAdvance(p);
        }
        p->currentClassName = prevClass;

        // Build TYPE_DECL(Name = <recordAst>) and register type
        AST *typeDecl = newASTNode(AST_TYPE_DECL, classNameTok);
        setLeft(typeDecl, recordAst);

        // Register the type into the global type table
        if (classNameTok && classNameTok->value) {
            insertType(classNameTok->value, recordAst);
        }
        // Return both type and methods in a compound so top-level gets both
        AST *bundle = newASTNode(AST_COMPOUND, NULL);
        addChild(bundle, typeDecl);
        // append methods
        if (methods && methods->child_count > 0) {
            for (int mi = 0; mi < methods->child_count; mi++) {
                addChild(bundle, methods->children[mi]);
                methods->children[mi] = NULL;
            }
            methods->child_count = 0;
        }
        freeAST(methods);
        return bundle;
    }
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        return parseBlock(p);
    }
    if (p->current.type == REA_TOKEN_IF) {
        return parseIf(p);
    }
    if (p->current.type == REA_TOKEN_WHILE) {
        return parseWhile(p);
    }
    if (p->current.type == REA_TOKEN_FOR) {
        return parseFor(p);
    }
    if (p->current.type == REA_TOKEN_DO) {
        return parseDoWhile(p);
    }
    if (p->current.type == REA_TOKEN_SWITCH) {
        return parseSwitch(p);
    }
    if (p->current.type == REA_TOKEN_RETURN) {
        return parseReturn(p);
    }
    if (p->current.type == REA_TOKEN_INT || p->current.type == REA_TOKEN_FLOAT ||
        p->current.type == REA_TOKEN_STR || p->current.type == REA_TOKEN_BOOL) {
        return parseVarDecl(p);
    }
    AST *expr = parseExpression(p);
    if (!expr) return NULL;
    if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);
    if (expr->type == AST_ASSIGN) {
        return expr; // assignments act as statements directly
    }
    AST *stmt = newASTNode(AST_EXPR_STMT, expr->token);
    setLeft(stmt, expr);
    return stmt;
}

AST *parseRea(const char *source) {
    ReaParser p;
    reaInitLexer(&p.lexer, source);
    p.currentFunctionType = TYPE_VOID;
    p.hadError = false;
    reaAdvance(&p);

    AST *program = newASTNode(AST_PROGRAM, NULL);
    AST *block = newASTNode(AST_BLOCK, NULL);
    setRight(program, block);

    AST *decls = newASTNode(AST_COMPOUND, NULL);
    AST *stmts = newASTNode(AST_COMPOUND, NULL);
    addChild(block, decls);
    addChild(block, stmts);

    while (p.current.type != REA_TOKEN_EOF && !p.hadError) {
        AST *stmt = parseStatement(&p);
        if (!stmt) break;
        if (stmt->type == AST_VAR_DECL || stmt->type == AST_FUNCTION_DECL || stmt->type == AST_PROCEDURE_DECL) {
            addChild(decls, stmt);
        } else {
            addChild(stmts, stmt);
        }
    }

    if (p.hadError) {
        freeAST(program);
        return NULL;
    }

    return program;
}
