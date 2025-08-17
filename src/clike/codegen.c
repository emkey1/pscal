#include "clike/codegen.h"
#include "clike/builtins.h"
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

static char* tokenToCString(ClikeToken t) {
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

static void collectLocals(ASTNodeClike* node, FuncContext* ctx) {
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

static void compileStatement(ASTNodeClike *node, BytecodeChunk *chunk, FuncContext* ctx);
static void compileExpression(ASTNodeClike *node, BytecodeChunk *chunk, FuncContext* ctx);

// Helper to compile an l-value (currently only local identifiers) and push its
// address onto the stack.
static void compileLValue(ASTNodeClike *node, BytecodeChunk *chunk, FuncContext* ctx) {
    if (!node) return;
    if (node->type == TCAST_IDENTIFIER) {
        char* name = tokenToCString(node->token);
        int idx = resolveLocal(ctx, name);
        free(name);
        writeBytecodeChunk(chunk, OP_GET_LOCAL_ADDRESS, node->token.line);
        writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
    }
}

static void compileStatement(ASTNodeClike *node, BytecodeChunk *chunk, FuncContext* ctx) {
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
                ASTNodeClike* child = node->children[i];
                if (child->type == TCAST_VAR_DECL) continue;
                compileStatement(child, chunk, ctx);
            }
            break;
        }
        default:
            break;
    }
}

static void compileExpression(ASTNodeClike *node, BytecodeChunk *chunk, FuncContext* ctx) {
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
                case CLIKE_TOKEN_PLUS: writeBytecodeChunk(chunk, OP_ADD, node->token.line); break;
                case CLIKE_TOKEN_MINUS: writeBytecodeChunk(chunk, OP_SUBTRACT, node->token.line); break;
                case CLIKE_TOKEN_STAR: writeBytecodeChunk(chunk, OP_MULTIPLY, node->token.line); break;
                case CLIKE_TOKEN_SLASH: writeBytecodeChunk(chunk, OP_DIVIDE, node->token.line); break;
                case CLIKE_TOKEN_GREATER: writeBytecodeChunk(chunk, OP_GREATER, node->token.line); break;
                case CLIKE_TOKEN_GREATER_EQUAL: writeBytecodeChunk(chunk, OP_GREATER_EQUAL, node->token.line); break;
                case CLIKE_TOKEN_LESS: writeBytecodeChunk(chunk, OP_LESS, node->token.line); break;
                case CLIKE_TOKEN_LESS_EQUAL: writeBytecodeChunk(chunk, OP_LESS_EQUAL, node->token.line); break;
                case CLIKE_TOKEN_EQUAL_EQUAL: writeBytecodeChunk(chunk, OP_EQUAL, node->token.line); break;
                case CLIKE_TOKEN_BANG_EQUAL: writeBytecodeChunk(chunk, OP_NOT_EQUAL, node->token.line); break;
                case CLIKE_TOKEN_AND_AND: writeBytecodeChunk(chunk, OP_AND, node->token.line); break;
                case CLIKE_TOKEN_OR_OR: writeBytecodeChunk(chunk, OP_OR, node->token.line); break;
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
            char *name = tokenToCString(node->token);
            if (strcmp(name, "printf") == 0) {
                for (int i = 0; i < node->child_count; ++i) {
                    compileExpression(node->children[i], chunk, ctx);
                }
                // Directly map printf to the WriteLn opcode. printf in clike is
                // treated as a procedure that always succeeds and returns 0.
                writeBytecodeChunk(chunk, OP_WRITE_LN, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)node->child_count, node->token.line);

                // Push a dummy return value (0) so expression statements remain
                // balanced on the stack.
                Value zero = makeInt(0);
                int idx = addConstantToChunk(chunk, &zero);
                freeValue(&zero);
                writeBytecodeChunk(chunk, OP_CONSTANT, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
            } else if (strcmp(name, "scanf") == 0) {
                // Compile arguments as l-values (addresses) and call VM builtin
                // readln. scanf in clike returns 0 for simplicity.
                for (int i = 0; i < node->child_count; ++i) {
                    compileLValue(node->children[i], chunk, ctx);
                }
                int rlIndex = addStringConstant(chunk, "readln");
                writeBytecodeChunk(chunk, OP_CALL_BUILTIN, node->token.line);
                emitShort(chunk, (uint16_t)rlIndex, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)node->child_count, node->token.line);

                Value zero = makeInt(0);
                int idx = addConstantToChunk(chunk, &zero);
                freeValue(&zero);
                writeBytecodeChunk(chunk, OP_CONSTANT, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
            } else {
                for (int i = 0; i < node->child_count; ++i) {
                    compileExpression(node->children[i], chunk, ctx);
                }
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
            }
            free(name);
            break;
        }
        default:
            break;
    }
}

static void compileFunction(ASTNodeClike *func, BytecodeChunk *chunk) {
    if (!func || !func->right) return;

    FuncContext ctx = {0};
    // Parameters
    if (func->left) {
        for (int i = 0; i < func->left->child_count; i++) {
            ASTNodeClike* p = func->left->children[i];
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

    // Initialize local variables (excluding parameters) to 0 so that builtins
    // like `readln` treat them as integers instead of defaulting to strings.
    if (ctx.localCount > ctx.paramCount) {
        Value zero = makeInt(0);
        int zeroIdx = addConstantToChunk(chunk, &zero);
        freeValue(&zero);
        for (int i = ctx.paramCount; i < ctx.localCount; i++) {
            writeBytecodeChunk(chunk, OP_CONSTANT, func->token.line);
            writeBytecodeChunk(chunk, (uint8_t)zeroIdx, func->token.line);
            writeBytecodeChunk(chunk, OP_SET_LOCAL, func->token.line);
            writeBytecodeChunk(chunk, (uint8_t)i, func->token.line);
        }
    }

    compileStatement(func->right, chunk, &ctx);
    writeBytecodeChunk(chunk, OP_RETURN, func->token.line);

    for (int i = 0; i < ctx.localCount; i++) {
        free(ctx.locals[i].name);
    }
    free(fname);
}

void clike_compile(ASTNodeClike *program, BytecodeChunk *chunk) {
    initBytecodeChunk(chunk);
    if (!program) return;

    // Emit a call to main at the start of the program. This ensures a call
    // frame is created so that local variables in main have storage on the
    // stack.  Previously a simple jump was used, which executed `main`
    // without a frame, leading to crashes when builtins expected VAR
    // parameters (e.g. `scanf`).
    writeBytecodeChunk(chunk, OP_CALL, 0);
    int mainNameIdx = addStringConstant(chunk, "main");
    emitShort(chunk, (uint16_t)mainNameIdx, 0);
    // Placeholder for the target address of main; patch later once functions
    // are compiled and we know its bytecode location.
    int mainAddrPatch = chunk->count;
    emitShort(chunk, 0, 0);
    // Placeholder for main's arity (argument count).
    int mainArityPatch = chunk->count;
    writeBytecodeChunk(chunk, 0, 0);
    // After main returns halt the VM.
    writeBytecodeChunk(chunk, OP_HALT, 0);

    int mainAddress = -1;
    uint8_t mainArity = 0;
    for (int i = 0; i < program->child_count; ++i) {
        ASTNodeClike *decl = program->children[i];
        if (decl->type != TCAST_FUN_DECL) continue;
        char *name = tokenToCString(decl->token);
        compileFunction(decl, chunk);
        if (strcmp(name, "main") == 0) {
            Symbol* sym = (Symbol*)hashTableLookup(procedure_table, name);
            mainAddress = sym->bytecode_address;
            mainArity = sym->arity;
        }
        free(name);
    }

    if (mainAddress >= 0) {
        patchShort(chunk, mainAddrPatch, (uint16_t)mainAddress);
        chunk->code[mainArityPatch] = mainArity;
    }
}

