#include "clike/ast.h"
#include "core/utils.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

ASTNodeClike *newASTNodeClike(ASTNodeTypeClike type, ClikeToken token) {
    ASTNodeClike *node = (ASTNodeClike*)calloc(1, sizeof(ASTNodeClike));
    if (!node) return NULL;
    node->type = type;
    node->token = token;
    node->var_type = TYPE_UNKNOWN;
    node->is_array = 0;
    node->array_size = 0;
    node->array_dims = NULL;
    node->array_dim_exprs = NULL;
    node->dim_count = 0;
    node->element_type = TYPE_UNKNOWN;
    node->is_const = 0;
    node->left = node->right = node->third = NULL;
    node->children = NULL;
    node->child_count = 0;
    node->parent = NULL;
    return node;
}

ASTNodeClike *newThreadSpawnClike(ASTNodeClike *call) {
    ASTNodeClike *node = newASTNodeClike(TCAST_THREAD_SPAWN, (ClikeToken){0});
    setLeftClike(node, call);
    return node;
}

ASTNodeClike *newThreadJoinClike(ASTNodeClike *expr) {
    ASTNodeClike *node = newASTNodeClike(TCAST_THREAD_JOIN, (ClikeToken){0});
    setLeftClike(node, expr);
    return node;
}

void addChildClike(ASTNodeClike *parent, ASTNodeClike *child) {
    if (!parent || !child) return;
    ASTNodeClike **new_children = (ASTNodeClike**)realloc(
        parent->children, sizeof(ASTNodeClike*) * (parent->child_count + 1));
    if (!new_children) return; // allocation failed, leave structure unchanged
    parent->children = new_children;
    parent->children[parent->child_count++] = child;
    child->parent = parent;
}

void setLeftClike(ASTNodeClike *parent, ASTNodeClike *child) {
    if (!parent) return;
    parent->left = child;
    if (child) child->parent = parent;
}

void setRightClike(ASTNodeClike *parent, ASTNodeClike *child) {
    if (!parent) return;
    parent->right = child;
    if (child) child->parent = parent;
}

void setThirdClike(ASTNodeClike *parent, ASTNodeClike *child) {
    if (!parent) return;
    parent->third = child;
    if (child) child->parent = parent;
}

ASTNodeClike *cloneASTClike(ASTNodeClike *node) {
    if (!node) return NULL;
    ASTNodeClike *copy = newASTNodeClike(node->type, node->token);
    if (!copy) return NULL;
    copy->var_type = node->var_type;
    copy->is_array = node->is_array;
    copy->array_size = node->array_size;
    copy->dim_count = node->dim_count;
    copy->element_type = node->element_type;
    copy->is_const = node->is_const;
    if (node->dim_count > 0 && node->array_dims) {
        copy->array_dims = (int*)malloc(sizeof(int) * node->dim_count);
        memcpy(copy->array_dims, node->array_dims, sizeof(int) * node->dim_count);
    }
    if (node->dim_count > 0 && node->array_dim_exprs) {
        copy->array_dim_exprs = (ASTNodeClike**)malloc(sizeof(ASTNodeClike*) * node->dim_count);
        for (int i = 0; i < node->dim_count; ++i) {
            copy->array_dim_exprs[i] = cloneASTClike(node->array_dim_exprs[i]);
            if (copy->array_dim_exprs[i]) copy->array_dim_exprs[i]->parent = copy;
        }
    }
    setLeftClike(copy, cloneASTClike(node->left));
    setRightClike(copy, cloneASTClike(node->right));
    setThirdClike(copy, cloneASTClike(node->third));
    for (int i = 0; i < node->child_count; ++i) {
        addChildClike(copy, cloneASTClike(node->children[i]));
    }
    return copy;
}

void freeASTClike(ASTNodeClike *node) {
    if (!node) return;
    for (int i = 0; i < node->child_count; ++i) freeASTClike(node->children[i]);
    if (node->left) freeASTClike(node->left);
    if (node->right) freeASTClike(node->right);
    if (node->third) freeASTClike(node->third);
    free(node->children);
    free(node->array_dims);
    if (node->array_dim_exprs) {
        for (int i = 0; i < node->dim_count; ++i) {
            freeASTClike(node->array_dim_exprs[i]);
        }
        free(node->array_dim_exprs);
    }
    free(node);
}

static void indent(FILE *out, int level) {
    for (int i = 0; i < level; ++i) {
        fputc(' ', out);
    }
}

static void escapeString(FILE *out, const char *str, int len) {
    for (int i = 0; i < len; ++i) {
        char c = str[i];
        switch (c) {
            case '\\': fputs("\\\\", out); break;
            case '"': fputs("\\\"", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default: fputc(c, out); break;
        }
    }
}

/*
 * Map C-like AST node types to Pascal AST node strings understood by
 * tools/ast_json_loader.c.  The loader relies on astTypeFromString(), which
 * expects the strings produced by astTypeToString() for Pascal AST enums.  If
 * we emit the raw C-like names (e.g. "BINOP" or "IDENTIFIER") the loader would
 * treat every node as AST_NOOP.  This translation keeps the structural
 * information intact when deserialising.
 */
static const char* nodeTypeToPascalString(ASTNodeTypeClike type) {
    switch (type) {
        case TCAST_PROGRAM:       return "PROGRAM";
        case TCAST_VAR_DECL:      return "VAR_DECL";
        case TCAST_FUN_DECL:      return "FUNCTION_DECL";
        case TCAST_PARAM:         return "VAR_DECL";      /* best approximation */
        case TCAST_COMPOUND:      return "COMPOUND";
        case TCAST_IF:            return "IF";
        case TCAST_WHILE:         return "WHILE";
        case TCAST_FOR:           return "FOR_TO";       /* generic for-loop */
        case TCAST_DO_WHILE:      return "REPEAT";
        case TCAST_SWITCH:        return "CASE";
        case TCAST_CASE:          return "CASE_BRANCH";
        case TCAST_BREAK:         return "BREAK";
        case TCAST_CONTINUE:      return "CONTINUE";
        case TCAST_RETURN:        return "RETURN";
        case TCAST_EXPR_STMT:     return "EXPR_STMT";
        case TCAST_ASSIGN:        return "ASSIGN";
        case TCAST_BINOP:         return "BINARY_OP";
        case TCAST_UNOP:          return "UNARY_OP";
        case TCAST_TERNARY:       return "IF";           /* represented via IF */
        case TCAST_NUMBER:        return "NUMBER";
        case TCAST_STRING:        return "STRING";
        case TCAST_IDENTIFIER:    return "VARIABLE";
        case TCAST_ARRAY_ACCESS:  return "ARRAY_ACCESS";
        case TCAST_MEMBER:        return "FIELD_ACCESS";
        case TCAST_ADDR:          return "ADDR_OF";
        case TCAST_DEREF:         return "DEREFERENCE";
        case TCAST_SIZEOF:        return "UNARY_OP";
        case TCAST_CALL:          return "PROCEDURE_CALL";
        case TCAST_STRUCT_DECL:   return "RECORD_TYPE";
        case TCAST_THREAD_SPAWN:  return "THREAD_SPAWN";
        case TCAST_THREAD_JOIN:   return "THREAD_JOIN";
        default:                  return "UNKNOWN_AST_TYPE";
    }
}

static const char* clikeTokenTypeToPascalString(ClikeTokenType type) {
    switch (type) {
        case CLIKE_TOKEN_IF:            return "IF";
        case CLIKE_TOKEN_ELSE:          return "ELSE";
        case CLIKE_TOKEN_WHILE:         return "WHILE";
        case CLIKE_TOKEN_FOR:           return "FOR";
        case CLIKE_TOKEN_DO:            return "DO";
        case CLIKE_TOKEN_SWITCH:       return "CASE";
        case CLIKE_TOKEN_CASE:         return "CASE";
        case CLIKE_TOKEN_DEFAULT:      return "ELSE";
        case CLIKE_TOKEN_STRUCT:       return "RECORD";
        case CLIKE_TOKEN_ENUM:         return "ENUM";
        case CLIKE_TOKEN_CONST:        return "CONST";
        case CLIKE_TOKEN_BREAK:        return "BREAK";
        case CLIKE_TOKEN_CONTINUE:     return "CONTINUE";
        case CLIKE_TOKEN_RETURN:       return "RETURN";
        case CLIKE_TOKEN_IMPORT:       return "USES";
        case CLIKE_TOKEN_SPAWN:        return "SPAWN";
        case CLIKE_TOKEN_JOIN:         return "JOIN";
        case CLIKE_TOKEN_IDENTIFIER:   return "IDENTIFIER";
        case CLIKE_TOKEN_NUMBER:       return "INTEGER_CONST";
        case CLIKE_TOKEN_FLOAT_LITERAL:return "REAL_CONST";
        case CLIKE_TOKEN_CHAR_LITERAL: return "STRING_CONST";
        case CLIKE_TOKEN_STRING:       return "STRING_CONST";
        case CLIKE_TOKEN_PLUS:         return "PLUS";
        case CLIKE_TOKEN_PLUS_EQUAL:   return "PLUS";
        case CLIKE_TOKEN_MINUS:        return "MINUS";
        case CLIKE_TOKEN_MINUS_EQUAL:  return "MINUS";
        case CLIKE_TOKEN_PLUS_PLUS:    return "PLUS";
        case CLIKE_TOKEN_MINUS_MINUS:  return "MINUS";
        case CLIKE_TOKEN_STAR:         return "MUL";
        case CLIKE_TOKEN_STAR_EQUAL:   return "MUL";
        case CLIKE_TOKEN_SLASH:        return "SLASH";
        case CLIKE_TOKEN_SLASH_EQUAL:  return "SLASH";
        case CLIKE_TOKEN_PERCENT:      return "MOD";
        case CLIKE_TOKEN_PERCENT_EQUAL:return "MOD";
        case CLIKE_TOKEN_TILDE:        return "NOT";
        case CLIKE_TOKEN_BIT_AND:      return "AND";
        case CLIKE_TOKEN_BIT_AND_EQUAL:return "AND";
        case CLIKE_TOKEN_BIT_OR:       return "OR";
        case CLIKE_TOKEN_BIT_OR_EQUAL: return "OR";
        case CLIKE_TOKEN_BIT_XOR:      return "XOR";
        case CLIKE_TOKEN_BIT_XOR_EQUAL:return "XOR";
        case CLIKE_TOKEN_SHL:          return "SHL";
        case CLIKE_TOKEN_SHL_EQUAL:    return "SHL";
        case CLIKE_TOKEN_SHR:          return "SHR";
        case CLIKE_TOKEN_SHR_EQUAL:    return "SHR";
        case CLIKE_TOKEN_BANG:         return "NOT";
        case CLIKE_TOKEN_BANG_EQUAL:   return "NOT_EQUAL";
        case CLIKE_TOKEN_EQUAL:        return "ASSIGN";
        case CLIKE_TOKEN_EQUAL_EQUAL:  return "EQUAL";
        case CLIKE_TOKEN_LESS:         return "LESS";
        case CLIKE_TOKEN_LESS_EQUAL:   return "LESS_EQUAL";
        case CLIKE_TOKEN_GREATER:      return "GREATER";
        case CLIKE_TOKEN_GREATER_EQUAL:return "GREATER_EQUAL";
        case CLIKE_TOKEN_AND_AND:      return "AND";
        case CLIKE_TOKEN_OR_OR:        return "OR";
        case CLIKE_TOKEN_QUESTION:     return "UNKNOWN";
        case CLIKE_TOKEN_COLON:        return "COLON";
        case CLIKE_TOKEN_DOT:          return "PERIOD";
        case CLIKE_TOKEN_ARROW:        return "UNKNOWN";
        case CLIKE_TOKEN_SEMICOLON:    return "SEMICOLON";
        case CLIKE_TOKEN_COMMA:        return "COMMA";
        case CLIKE_TOKEN_LPAREN:       return "LPAREN";
        case CLIKE_TOKEN_RPAREN:       return "RPAREN";
        case CLIKE_TOKEN_LBRACE:       return "LBRACKET";
        case CLIKE_TOKEN_RBRACE:       return "RBRACKET";
        case CLIKE_TOKEN_LBRACKET:     return "LBRACKET";
        case CLIKE_TOKEN_RBRACKET:     return "RBRACKET";
        case CLIKE_TOKEN_EOF:          return "EOF";
        case CLIKE_TOKEN_UNKNOWN:      return "UNKNOWN";
        default:                       return "IDENTIFIER";
    }
}

static void dumpASTClikeJSONRecursive(ASTNodeClike *node, FILE *out, int level) {
    if (!node) {
        indent(out, level);
        fputs("null", out);
        return;
    }

    indent(out, level);
    fputs("{\n", out);

    indent(out, level + 2);
    fprintf(out, "\"node_type\": \"%s\"", nodeTypeToPascalString(node->type));

    /* Emit token in the format expected by tools/ast_json_loader.c */
    if (node->token.type != CLIKE_TOKEN_UNKNOWN ||
        (node->token.lexeme && node->token.length > 0)) {
        fputs(",\n", out);
        indent(out, level + 2);
        fputs("\"token\": {\n", out);
        indent(out, level + 4);
        fprintf(out, "\"type\": \"%s\"", clikeTokenTypeToPascalString(node->token.type));
        if (node->token.lexeme && node->token.length > 0) {
            fputs(",\n", out);
            indent(out, level + 4);
            fputs("\"value\": \"", out);
            escapeString(out, node->token.lexeme, node->token.length);
            fputc('"', out);
        }
        fputc('\n', out);
        indent(out, level + 2);
        fputc('}', out);
    }

    /*
     * Emit an annotated type only when semantic analysis produced a concrete
     * value.  The loader treats the field as optional and assumes
     * TYPE_UNKNOWN when it is absent.
     */
    if (node->var_type != TYPE_UNKNOWN) {
        fputs(",\n", out);
        indent(out, level + 2);
        fprintf(out, "\"var_type_annotated\": \"%s\"", varTypeToString(node->var_type));
    }

    if (node->left) {
        fputs(",\n", out);
        indent(out, level + 2);
        fputs("\"left\": \n", out);
        dumpASTClikeJSONRecursive(node->left, out, level + 4);
    }
    if (node->right) {
        fputs(",\n", out);
        indent(out, level + 2);
        fputs("\"right\": \n", out);
        dumpASTClikeJSONRecursive(node->right, out, level + 4);
    }
    if (node->third) {
        fputs(",\n", out);
        indent(out, level + 2);
        fputs("\"third\": \n", out);
        dumpASTClikeJSONRecursive(node->third, out, level + 4);
    }
    if (node->child_count > 0) {
        fputs(",\n", out);
        indent(out, level + 2);
        fputs("\"children\": [\n", out);
        for (int i = 0; i < node->child_count; ++i) {
            dumpASTClikeJSONRecursive(node->children[i], out, level + 4);
            if (i < node->child_count - 1) {
                fputs(",\n", out);
            } else {
                fputc('\n', out);
            }
        }
        indent(out, level + 2);
        fputs("]", out);
    }

    fputc('\n', out);
    indent(out, level);
    fputc('}', out);
}

void dumpASTClikeJSON(ASTNodeClike *node, FILE *out) {
    dumpASTClikeJSONRecursive(node, out, 0);
}

bool verifyASTClikeLinks(ASTNodeClike *node, ASTNodeClike *expectedParent) {
    if (!node) return true;
    bool links_ok = true;
    if (node->parent != expectedParent) {
        fprintf(stderr,
                "[VERIFY] Node %p has parent %p but expected %p\n",
                (void*)node, (void*)node->parent, (void*)expectedParent);
        links_ok = false;
    }
    if (!verifyASTClikeLinks(node->left, node)) links_ok = false;
    if (!verifyASTClikeLinks(node->right, node)) links_ok = false;
    if (!verifyASTClikeLinks(node->third, node)) links_ok = false;
    for (int i = 0; i < node->child_count; ++i) {
        if (!verifyASTClikeLinks(node->children[i], node)) links_ok = false;
    }
    if (node->array_dim_exprs) {
        for (int i = 0; i < node->dim_count; ++i) {
            if (!verifyASTClikeLinks(node->array_dim_exprs[i], node)) links_ok = false;
        }
    }
    return links_ok;
}
