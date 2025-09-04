#include "rea/parser.h"
#include <stdlib.h>
#include <string.h>

ReaAST *parseRea(const char *source) {
    ReaLexer lexer;
    reaInitLexer(&lexer, source);

    ReaAST *root = reaNewASTNode(REA_AST_PROGRAM);

    ReaToken t;
    do {
        t = reaNextToken(&lexer);
        ReaAST *child = reaNewASTNode(REA_AST_TOKEN);
        child->token.type = t.type;
        child->token.line = t.line;
        child->token.length = t.length;
        char *lex = (char *)malloc(t.length + 1);
        if (lex) {
            memcpy(lex, t.start, t.length);
            lex[t.length] = '\0';
            child->token.start = lex;
        } else {
            child->token.start = NULL;
        }
        reaAddChild(root, child);
    } while (t.type != REA_TOKEN_EOF);

    return root;
}

