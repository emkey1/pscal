#include "tinyc/codegen.h"
#include "tinyc/builtins.h"
#include "core/types.h"
#include "core/utils.h"
#include <string.h>
#include <stdlib.h>

static int addStringConstant(BytecodeChunk* chunk, const char* str) {
    Value val = makeString(str);
    int index = addConstantToChunk(chunk, &val);
    freeValue(&val);
    return index;
}

static char* tokenToCString(TinyCToken t) {
    char* s = (char*)malloc(t.length + 1);
    memcpy(s, t.lexeme, t.length);
    s[t.length] = '\0';
    return s;
}

static void compileExpression(ASTNodeTinyC *node, BytecodeChunk *chunk);
static void compileStatement(ASTNodeTinyC *node, BytecodeChunk *chunk);
static void compileFunction(ASTNodeTinyC *func, BytecodeChunk *chunk);

static void compileFunction(ASTNodeTinyC *func, BytecodeChunk *chunk) {
    if (!func || !func->right) return;
    ASTNodeTinyC *body = func->right;
    for (int i = 0; i < body->child_count; ++i) {
        ASTNodeTinyC *child = body->children[i];
        if (child->type != TCAST_VAR_DECL) {
            compileStatement(child, chunk);
        }
    }
    writeBytecodeChunk(chunk, OP_HALT, func->token.line);
}

static void compileStatement(ASTNodeTinyC *node, BytecodeChunk *chunk) {
    if (!node) return;
    switch (node->type) {
        case TCAST_RETURN:
            if (node->left) compileExpression(node->left, chunk);
            writeBytecodeChunk(chunk, OP_RETURN, node->token.line);
            break;
        case TCAST_EXPR_STMT:
            if (node->left) {
                compileExpression(node->left, chunk);
                writeBytecodeChunk(chunk, OP_POP, node->token.line);
            }
            break;
        default:
            // Other statements not implemented
            break;
    }
}

static void compileExpression(ASTNodeTinyC *node, BytecodeChunk *chunk) {
    if (!node) return;
    switch (node->type) {
        case TCAST_NUMBER: {
            Value v = makeInt(node->token.int_val);
            int idx = addConstantToChunk(chunk, &v);
            writeBytecodeChunk(chunk, OP_CONSTANT, node->token.line);
            writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
            break;
        }
        case TCAST_BINOP:
            compileExpression(node->left, chunk);
            compileExpression(node->right, chunk);
            switch (node->token.type) {
                case TINYCTOKEN_PLUS: writeBytecodeChunk(chunk, OP_ADD, node->token.line); break;
                case TINYCTOKEN_MINUS: writeBytecodeChunk(chunk, OP_SUBTRACT, node->token.line); break;
                case TINYCTOKEN_STAR: writeBytecodeChunk(chunk, OP_MULTIPLY, node->token.line); break;
                case TINYCTOKEN_SLASH: writeBytecodeChunk(chunk, OP_DIVIDE, node->token.line); break;
                default: break;
            }
            break;
        case TCAST_CALL: {
            for (int i = 0; i < node->child_count; ++i) {
                compileExpression(node->children[i], chunk);
            }
            char *name = tokenToCString(node->token);
            int nameIndex = addStringConstant(chunk, name);
            free(name);
            writeBytecodeChunk(chunk, OP_CALL_BUILTIN, node->token.line);
            emitShort(chunk, (uint16_t)nameIndex, node->token.line);
            writeBytecodeChunk(chunk, (uint8_t)node->child_count, node->token.line);
            break;
        }
        case TCAST_IDENTIFIER:
            // Variables not implemented
            writeBytecodeChunk(chunk, OP_CONSTANT, node->token.line);
            writeBytecodeChunk(chunk, 0, node->token.line);
            break;
        default:
            break;
    }
}

void tinyc_compile(ASTNodeTinyC *program, BytecodeChunk *chunk) {
    initBytecodeChunk(chunk);
    if (!program) return;
    for (int i = 0; i < program->child_count; ++i) {
        ASTNodeTinyC *decl = program->children[i];
        if (decl->type == TCAST_FUN_DECL) {
            char *name = tokenToCString(decl->token);
            if (strcmp(name, "main") == 0) {
                compileFunction(decl, chunk);
                free(name);
                return;
            }
            free(name);
        }
    }
}
