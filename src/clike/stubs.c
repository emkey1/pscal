#include "ast/ast.h"
#include "symbol/symbol.h"
#include "core/types.h"
#include "core/utils.h"
#include "clike/parser.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static AST* ensureBuiltinType(VarType type, AST** cache) {
    if (!*cache) {
        AST* node = newASTNode(AST_VARIABLE, NULL);
        if (!node) {
            return NULL;
        }
        setTypeAST(node, type);
        *cache = node;
    }
    return *cache;
}

AST* lookupType(const char* name) {
    // First, see if this name refers to a previously-declared struct.
    AST* type = clikeLookupStruct(name);
    if (type) return type;

    static AST* int32Type = NULL;
    static AST* doubleType = NULL;
    static AST* floatType = NULL;
    static AST* charType = NULL;
    static AST* stringType = NULL;
    static AST* booleanType = NULL;
    static AST* byteType = NULL;
    static AST* wordType = NULL;
    static AST* int64Type = NULL;
    static AST* voidType = NULL;

    struct BuiltinEntry {
        const char* name;
        AST** cache;
        VarType type;
    };

    const struct BuiltinEntry builtinTypes[] = {
        {"integer", &int32Type, TYPE_INT32},
        {"int", &int32Type, TYPE_INT32},
        {"real", &doubleType, TYPE_DOUBLE},
        {"double", &doubleType, TYPE_DOUBLE},
        {"single", &floatType, TYPE_FLOAT},
        {"float", &floatType, TYPE_FLOAT},
        {"char", &charType, TYPE_CHAR},
        {"string", &stringType, TYPE_STRING},
        {"boolean", &booleanType, TYPE_BOOLEAN},
        {"bool", &booleanType, TYPE_BOOLEAN},
        {"byte", &byteType, TYPE_BYTE},
        {"word", &wordType, TYPE_WORD},
        {"int64", &int64Type, TYPE_INT64},
        {"longint", &int64Type, TYPE_INT64},
        {"void", &voidType, TYPE_VOID},
    };

    for (size_t i = 0; i < sizeof(builtinTypes) / sizeof(builtinTypes[0]); ++i) {
        if (strcasecmp(name, builtinTypes[i].name) == 0) {
            return ensureBuiltinType(builtinTypes[i].type, builtinTypes[i].cache);
        }
    }

    return NULL;
}

Value evaluateCompileTimeValue(AST* node) { (void)node; return makeNil(); }
void insertType(const char* name, AST* typeDef) { (void)name; (void)typeDef; }

AST* newASTNode(ASTNodeType type, Token* token) {
    AST* node = (AST*)calloc(1, sizeof(AST));
    node->type = type;
    if (token) node->token = copyToken(token);
    return node;
}

void setTypeAST(AST* node, VarType type) { if (node) node->var_type = type; }

void setRight(AST* parent, AST* child) {
    if (parent) {
        parent->right = child;
        if (child) child->parent = parent;
    }
}

void addChild(AST* parent, AST* child) {
    if (!parent || !child) return;
    if (parent->child_capacity <= parent->child_count) {
        int newcap = parent->child_capacity ? parent->child_capacity * 2 : 4;
        parent->children = (AST**)realloc(parent->children, sizeof(AST*) * newcap);
        parent->child_capacity = newcap;
    }
    parent->children[parent->child_count++] = child;
    child->parent = parent;
}

void freeAST(AST* node) { (void)node; }
void dumpAST(AST* node, int indent) { (void)node; (void)indent; }
AST* copyAST(AST* node) { (void)node; return NULL; }
