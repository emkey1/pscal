#include "tinyc/codegen.h"
#include "tinyc/builtins.h"
#include "core/types.h"
#include "core/utils.h"
#include "symbol/symbol.h"
#include "globals.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    char *name;
    int index;
} LocalVar;

typedef struct {
    LocalVar locals[256];
    int localCount;
    int paramCount;
} FuncContext;

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

static int addLocal(FuncContext* ctx, const char* name) {
    ctx->locals[ctx->localCount].name = strdup(name);
    ctx->locals[ctx->localCount].index = ctx->localCount;
    return ctx->localCount++;
}

static int resolveLocal(FuncContext* ctx, const char* name) {
    for (int i = 0; i < ctx->localCount; i++) {
        if (strcmp(ctx->locals[i].name, name) == 0) return ctx->locals[i].index;
    }
    return -1;
}

static void collectLocals(ASTNodeTinyC* node, FuncContext* ctx) {
    if (!node) return;
    if (node->type == TCAST_VAR_DECL) {
        char* name = tokenToCString(node->token);
        addLocal(ctx, name);
        free(name);
        return;
    }
    if (node->left) collectLocals(node->left, ctx);
    if (node->right) collectLocals(node->right, ctx);
    if (node->third) collectLocals(node->third, ctx);
    for (int i = 0; i < node->child_count; i++) {
        collectLocals(node->children[i], ctx);
    }
}

static void compileStatement(ASTNodeTinyC *node, BytecodeChunk *chunk, FuncContext* ctx);
static void compileExpression(ASTNodeTinyC *node, BytecodeChunk *chunk, FuncContext* ctx);

static void compileStatement(ASTNodeTinyC *node, BytecodeChunk *chunk, FuncContext* ctx) {
    if (!node) return;
    switch (node->type) {
        case TCAST_RETURN:
            if (node->left) compileExpression(node->left, chunk, ctx);
            writeBytecodeChunk(chunk, OP_RETURN, node->token.line);
            break;
        case TCAST_EXPR_STMT:
            if (node->left) {
                compileExpression(node->left, chunk, ctx);
                writeBytecodeChunk(chunk, OP_POP, node->token.line);
            }
            break;
        case TCAST_IF: {
            compileExpression(node->left, chunk, ctx);
            writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, node->token.line);
            int elseJump = chunk->count;
            emitShort(chunk, 0xFFFF, node->token.line);
            compileStatement(node->right, chunk, ctx);
            if (node->third) {
                writeBytecodeChunk(chunk, OP_JUMP, node->token.line);
                int endJump = chunk->count;
                emitShort(chunk, 0xFFFF, node->token.line);
                uint16_t offset = (uint16_t)(chunk->count - (elseJump + 2));
                patchShort(chunk, elseJump, offset);
                compileStatement(node->third, chunk, ctx);
                uint16_t endOffset = (uint16_t)(chunk->count - (endJump + 2));
                patchShort(chunk, endJump, endOffset);
            } else {
                uint16_t offset = (uint16_t)(chunk->count - (elseJump + 2));
                patchShort(chunk, elseJump, offset);
            }
            break;
        }
        case TCAST_WHILE: {
            int loopStart = chunk->count;
            compileExpression(node->left, chunk, ctx);
            writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, node->token.line);
            int exitJump = chunk->count;
            emitShort(chunk, 0xFFFF, node->token.line);
            compileStatement(node->right, chunk, ctx);
            writeBytecodeChunk(chunk, OP_JUMP, node->token.line);
            int backOffset = loopStart - (chunk->count + 2);
            emitShort(chunk, (uint16_t)backOffset, node->token.line);
            uint16_t exitOffset = (uint16_t)(chunk->count - (exitJump + 2));
            patchShort(chunk, exitJump, exitOffset);
            break;
        }
        case TCAST_COMPOUND: {
            // Var declarations already collected; skip them
            for (int i = 0; i < node->child_count; i++) {
                ASTNodeTinyC* child = node->children[i];
                if (child->type == TCAST_VAR_DECL) continue;
                compileStatement(child, chunk, ctx);
            }
            break;
        }
        default:
            break;
    }
}

static void compileExpression(ASTNodeTinyC *node, BytecodeChunk *chunk, FuncContext* ctx) {
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
            compileExpression(node->left, chunk, ctx);
            compileExpression(node->right, chunk, ctx);
            switch (node->token.type) {
                case TINYCTOKEN_PLUS: writeBytecodeChunk(chunk, OP_ADD, node->token.line); break;
                case TINYCTOKEN_MINUS: writeBytecodeChunk(chunk, OP_SUBTRACT, node->token.line); break;
                case TINYCTOKEN_STAR: writeBytecodeChunk(chunk, OP_MULTIPLY, node->token.line); break;
                case TINYCTOKEN_SLASH: writeBytecodeChunk(chunk, OP_DIVIDE, node->token.line); break;
                case TINYCTOKEN_GREATER: writeBytecodeChunk(chunk, OP_GREATER, node->token.line); break;
                case TINYCTOKEN_GREATER_EQUAL: writeBytecodeChunk(chunk, OP_GREATER_EQUAL, node->token.line); break;
                case TINYCTOKEN_LESS: writeBytecodeChunk(chunk, OP_LESS, node->token.line); break;
                case TINYCTOKEN_LESS_EQUAL: writeBytecodeChunk(chunk, OP_LESS_EQUAL, node->token.line); break;
                case TINYCTOKEN_EQUAL_EQUAL: writeBytecodeChunk(chunk, OP_EQUAL, node->token.line); break;
                case TINYCTOKEN_BANG_EQUAL: writeBytecodeChunk(chunk, OP_NOT_EQUAL, node->token.line); break;
                case TINYCTOKEN_AND_AND: writeBytecodeChunk(chunk, OP_AND, node->token.line); break;
                case TINYCTOKEN_OR_OR: writeBytecodeChunk(chunk, OP_OR, node->token.line); break;
                default: break;
            }
            break;
        case TCAST_ASSIGN: {
            if (node->left && node->left->type == TCAST_IDENTIFIER) {
                char* name = tokenToCString(node->left->token);
                int idx = resolveLocal(ctx, name);
                free(name);
                compileExpression(node->right, chunk, ctx);
                writeBytecodeChunk(chunk, OP_DUP, node->token.line);
                writeBytecodeChunk(chunk, OP_SET_LOCAL, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
            }
            break;
        }
        case TCAST_IDENTIFIER: {
            char* name = tokenToCString(node->token);
            int idx = resolveLocal(ctx, name);
            free(name);
            writeBytecodeChunk(chunk, OP_GET_LOCAL, node->token.line);
            writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
            break;
        }
        case TCAST_CALL: {
            for (int i = 0; i < node->child_count; ++i) {
                compileExpression(node->children[i], chunk, ctx);
            }
            char *name = tokenToCString(node->token);
            Symbol* sym = procedure_table ? hashTableLookup(procedure_table, name) : NULL;
            int nameIndex = addStringConstant(chunk, name);
            if (sym) {
                writeBytecodeChunk(chunk, OP_CALL, node->token.line);
                emitShort(chunk, (uint16_t)nameIndex, node->token.line);
                emitShort(chunk, (uint16_t)sym->bytecode_address, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)node->child_count, node->token.line);
            } else {
                writeBytecodeChunk(chunk, OP_CALL_BUILTIN, node->token.line);
                emitShort(chunk, (uint16_t)nameIndex, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)node->child_count, node->token.line);
            }
            free(name);
            break;
        }
        default:
            break;
    }
}

static void compileFunction(ASTNodeTinyC *func, BytecodeChunk *chunk) {
    if (!func || !func->right) return;

    FuncContext ctx = {0};
    // Parameters
    if (func->left) {
        for (int i = 0; i < func->left->child_count; i++) {
            ASTNodeTinyC* p = func->left->children[i];
            char* name = tokenToCString(p->token);
            addLocal(&ctx, name);
            free(name);
            ctx.paramCount++;
        }
    }

    // Collect local variable declarations recursively
    collectLocals(func->right, &ctx);

    int address = chunk->count;
    char* fname = tokenToCString(func->token);
    Symbol* sym = malloc(sizeof(Symbol));
    memset(sym, 0, sizeof(Symbol));
    sym->name = strdup(fname);
    sym->bytecode_address = address;
    sym->arity = (uint8_t)ctx.paramCount;
    sym->locals_count = (uint8_t)(ctx.localCount - ctx.paramCount);
    sym->type = TYPE_INTEGER;
    sym->is_defined = true;
    hashTableInsert(procedure_table, sym);

    compileStatement(func->right, chunk, &ctx);
    writeBytecodeChunk(chunk, OP_RETURN, func->token.line);

    for (int i = 0; i < ctx.localCount; i++) {
        free(ctx.locals[i].name);
    }
    free(fname);
}

void tinyc_compile(ASTNodeTinyC *program, BytecodeChunk *chunk) {
    initBytecodeChunk(chunk);
    if (!program) return;

    // Placeholder jump to main
    writeBytecodeChunk(chunk, OP_JUMP, 0);
    int mainJump = chunk->count;
    emitShort(chunk, 0, 0);

    int mainAddress = -1;
    for (int i = 0; i < program->child_count; ++i) {
        ASTNodeTinyC *decl = program->children[i];
        if (decl->type != TCAST_FUN_DECL) continue;
        char *name = tokenToCString(decl->token);
        compileFunction(decl, chunk);
        if (strcmp(name, "main") == 0) {
            mainAddress = ((Symbol*)hashTableLookup(procedure_table, name))->bytecode_address;
        }
        free(name);
    }

    if (mainAddress >= 0) {
        uint16_t offset = (uint16_t)(mainAddress - (mainJump + 2));
        patchShort(chunk, mainJump, offset);
    }
}

