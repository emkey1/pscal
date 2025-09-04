#include "rea/compiler.h"
#include "core/utils.h"
#include "core/types.h"
#include <string.h>

// Helper to create the minimal block/declaration structure expected by
// the existing compiler.  The block contains an empty declaration list
// and a single statement compound passed in as `statements`.
static AST *makeProgramWithStatements(AST *statements) {
    AST *program = newASTNode(AST_PROGRAM, NULL);

    AST *block = newASTNode(AST_BLOCK, NULL);
    block->is_global_scope = true;

    AST *decls = newASTNode(AST_COMPOUND, NULL);
    addChild(block, decls);
    addChild(block, statements);

    setRight(program, block);
    return program;
}

static AST *convertExpr(ReaAST *node) {
    if (!node) return NULL;
    switch (node->type) {
        case REA_AST_NUMBER: {
            const char *lex = node->token.start ? node->token.start : "";
            TokenType ttype = TOKEN_INTEGER_CONST;
            VarType vtype = TYPE_INTEGER;

            if (strpbrk(lex, ".eE")) {
                ttype = TOKEN_REAL_CONST;
                vtype = TYPE_REAL;
            }

            Token *num_tok = newToken(ttype,
                                     lex,
                                     node->token.line,
                                     0);
            AST *num = newASTNode(AST_NUMBER, num_tok);
            setTypeAST(num, vtype);
            return num;
        }
        case REA_AST_BINARY: {
            AST *left = convertExpr(node->children[0]);
            AST *right = convertExpr(node->children[1]);
            TokenType optype = TOKEN_UNKNOWN;
            switch (node->token.type) {
                case REA_TOKEN_PLUS: optype = TOKEN_PLUS; break;
                case REA_TOKEN_MINUS: optype = TOKEN_MINUS; break;
                case REA_TOKEN_STAR: optype = TOKEN_MUL; break;
                case REA_TOKEN_SLASH: optype = TOKEN_SLASH; break;
                default: break;
            }
            Token *op_tok = newToken(optype,
                                     node->token.start ? node->token.start : "",
                                     node->token.line,
                                     0);
            AST *bin = newASTNode(AST_BINARY_OP, op_tok);
            setLeft(bin, left);
            setRight(bin, right);
            return bin;
        }
        default:
            return NULL;
    }
}

AST *reaConvertToAST(ReaAST *root) {
    if (!root || root->child_count == 0) return NULL;

    AST *stmts = newASTNode(AST_COMPOUND, NULL);
    for (int i = 0; i < root->child_count; ++i) {
        AST *expr = convertExpr(root->children[i]);
        if (!expr) continue;
        AST *writeln = newASTNode(AST_WRITELN, NULL);
        addChild(writeln, expr);
        addChild(stmts, writeln);
    }
    if (stmts->child_count == 0) {
        freeAST(stmts);
        return NULL;
    }
    return makeProgramWithStatements(stmts);
}

