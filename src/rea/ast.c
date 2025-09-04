#include "rea/ast.h"
#include <stdlib.h>
#include <string.h>

ReaAST *reaNewASTNode(ReaASTNodeType type) {
    ReaAST *node = (ReaAST *)calloc(1, sizeof(ReaAST));
    node->type = type;
    return node;
}

void reaAddChild(ReaAST *parent, ReaAST *child) {
    if (!parent || !child) return;
    if (parent->child_capacity <= parent->child_count) {
        int newcap = parent->child_capacity ? parent->child_capacity * 2 : 4;
        parent->children = (ReaAST **)realloc(parent->children, newcap * sizeof(ReaAST *));
        parent->child_capacity = newcap;
    }
    parent->children[parent->child_count++] = child;
}

static void reaFreeToken(ReaToken *t) {
    if (!t) return;
    if (t->start) {
        free((void *)t->start);
        t->start = NULL;
    }
}

void reaFreeAST(ReaAST *node) {
    if (!node) return;
    for (int i = 0; i < node->child_count; i++) {
        reaFreeAST(node->children[i]);
    }
    free(node->children);
    reaFreeToken(&node->token);
    free(node);
}

static void printIndent(FILE *out, int indent) {
    for (int i = 0; i < indent; ++i) fputs("  ", out);
}

const char *reaTokenTypeToString(ReaTokenType type) {
    switch (type) {
#define CASE(t) case t: return #t;
        CASE(REA_TOKEN_EOF)
        CASE(REA_TOKEN_UNKNOWN)
        CASE(REA_TOKEN_IDENTIFIER)
        CASE(REA_TOKEN_NUMBER)
        CASE(REA_TOKEN_STRING)
        CASE(REA_TOKEN_LEFT_PAREN)
        CASE(REA_TOKEN_RIGHT_PAREN)
        CASE(REA_TOKEN_LEFT_BRACE)
        CASE(REA_TOKEN_RIGHT_BRACE)
        CASE(REA_TOKEN_COMMA)
        CASE(REA_TOKEN_DOT)
        CASE(REA_TOKEN_SEMICOLON)
        CASE(REA_TOKEN_COLON)
        CASE(REA_TOKEN_PLUS)
        CASE(REA_TOKEN_MINUS)
        CASE(REA_TOKEN_STAR)
        CASE(REA_TOKEN_SLASH)
        CASE(REA_TOKEN_EQUAL)
        CASE(REA_TOKEN_EQUAL_EQUAL)
        CASE(REA_TOKEN_BANG)
        CASE(REA_TOKEN_BANG_EQUAL)
        CASE(REA_TOKEN_GREATER)
        CASE(REA_TOKEN_GREATER_EQUAL)
        CASE(REA_TOKEN_LESS)
        CASE(REA_TOKEN_LESS_EQUAL)
        CASE(REA_TOKEN_CLASS)
        CASE(REA_TOKEN_EXTENDS)
        CASE(REA_TOKEN_NEW)
        CASE(REA_TOKEN_THIS)
        CASE(REA_TOKEN_SUPER)
        CASE(REA_TOKEN_IF)
        CASE(REA_TOKEN_ELSE)
        CASE(REA_TOKEN_WHILE)
        CASE(REA_TOKEN_FOR)
        CASE(REA_TOKEN_DO)
        CASE(REA_TOKEN_SWITCH)
        CASE(REA_TOKEN_CASE)
        CASE(REA_TOKEN_DEFAULT)
        CASE(REA_TOKEN_BREAK)
        CASE(REA_TOKEN_CONTINUE)
        CASE(REA_TOKEN_RETURN)
        CASE(REA_TOKEN_TRUE)
        CASE(REA_TOKEN_FALSE)
        CASE(REA_TOKEN_CONST)
        CASE(REA_TOKEN_IMPORT)
        CASE(REA_TOKEN_INT)
        CASE(REA_TOKEN_INT64)
        CASE(REA_TOKEN_INT32)
        CASE(REA_TOKEN_INT16)
        CASE(REA_TOKEN_INT8)
        CASE(REA_TOKEN_FLOAT)
        CASE(REA_TOKEN_FLOAT32)
        CASE(REA_TOKEN_LONG_DOUBLE)
        CASE(REA_TOKEN_CHAR)
        CASE(REA_TOKEN_BYTE)
        CASE(REA_TOKEN_STR)
        CASE(REA_TOKEN_TEXT)
        CASE(REA_TOKEN_MSTREAM)
        CASE(REA_TOKEN_VOID)
        CASE(REA_TOKEN_BOOL)
#undef CASE
    }
    return "UNKNOWN";
}

const char *reaASTNodeTypeToString(ReaASTNodeType type) {
    switch (type) {
        case REA_AST_PROGRAM: return "PROGRAM";
        case REA_AST_NUMBER: return "NUMBER";
        case REA_AST_BINARY: return "BINARY";
    }
    return "UNKNOWN";
}

static void escapeJSONString(FILE *out, const char *str) {
    fputc('"', out);
    while (*str) {
        unsigned char c = (unsigned char)*str;
        switch (c) {
            case '\\': fputs("\\\\", out); break;
            case '"': fputs("\\\"", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:
                if (c < 32 || c == 127) {
                    fprintf(out, "\\u%04x", c);
                } else {
                    fputc(c, out);
                }
        }
        str++;
    }
    fputc('"', out);
}

static void dumpJSON(ReaAST *node, FILE *out, int indent) {
    if (!node) {
        printIndent(out, indent);
        fputs("null", out);
        return;
    }
    printIndent(out, indent);
    fputs("{\n", out);
    indent++;
    printIndent(out, indent);
    fprintf(out, "\"node_type\": \"%s\"", reaASTNodeTypeToString(node->type));
    if (node->token.start) {
        fputs(",\n", out);
        printIndent(out, indent);
        fprintf(out, "\"token_type\": \"%s\",\n", reaTokenTypeToString(node->token.type));
        printIndent(out, indent);
        fprintf(out, "\"lexeme\": ");
        escapeJSONString(out, node->token.start ? node->token.start : "");
        fprintf(out, ",\n");
        printIndent(out, indent);
        fprintf(out, "\"line\": %d", node->token.line);
    }
    if (node->child_count > 0) {
        fputs(",\n", out);
        printIndent(out, indent);
        fputs("\"children\": [\n", out);
        for (int i = 0; i < node->child_count; i++) {
            dumpJSON(node->children[i], out, indent + 1);
            if (i < node->child_count - 1) fputs(",\n", out);
            else fputc('\n', out);
        }
        printIndent(out, indent);
        fputs("]", out);
    }
    fputc('\n', out);
    indent--;
    printIndent(out, indent);
    fputc('}', out);
}

void reaDumpASTJSON(ReaAST *node, FILE *out) {
    dumpJSON(node, out, 0);
    fputc('\n', out);
}
