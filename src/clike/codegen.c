#include "clike/codegen.h"
#include "clike/builtins.h"
#include "backend_ast/builtin.h"
#include "clike/parser.h"
#include "clike/semantics.h"
#include "Pascal/ast.h"
#include "core/types.h"
#include "core/utils.h"
#include "symbol/symbol.h"
#include "globals.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct {
    char *name;
    int index;
    VarType type;
    int depth;
    int isArray;
    int *arrayDims;
    int dimCount;
    VarType elemType;
} LocalVar;

typedef struct {
    int breakAddrs[256];
    int breakCount;
    int continueAddrs[256];
    int continueCount;
} LoopInfo;

typedef struct {
    LocalVar locals[256];
    int localCount;
    int maxLocalCount;
    int paramCount;
    int scopeDepth;
    LoopInfo loops[256];
    int loopDepth;
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

// Convert a string token's lexeme into a C string while interpreting common
// escape sequences (currently \n, \r, \t, \\, and \" ).
static char* tokenStringToCString(ClikeToken t) {
    char* s = (char*)malloc(t.length + 1); // escaped string can't be longer
    int j = 0;
    for (int i = 0; i < t.length; i++) {
        char c = t.lexeme[i];
        if (c == '\\' && i + 1 < t.length) {
            char next = t.lexeme[++i];
            switch (next) {
                case 'n': s[j++] = '\n'; break;
                case 'r': s[j++] = '\r'; break;
                case 't': s[j++] = '\t'; break;
                case '\\': s[j++] = '\\'; break;
                case '"': s[j++] = '"'; break;
                default:   s[j++] = next; break;
            }
        } else {
            s[j++] = c;
        }
    }
    s[j] = '\0';
    return s;
}

// Local helper to construct identifier tokens for core AST nodes
static Token* makeIdentTokenLocal(const char* s) {
    Token* t = (Token*)malloc(sizeof(Token));
    t->type = TOKEN_IDENTIFIER;
    t->value = strdup(s);
    t->line = 0;
    t->column = 0;
    return t;
}

// Create a core AST node representing a builtin type token
static AST* makeBuiltinTypeASTFromToken(ClikeToken t) {
    const char* name = NULL;
    VarType vt = TYPE_UNKNOWN;
    switch (t.type) {
        case CLIKE_TOKEN_INT:   name = "integer"; vt = TYPE_INTEGER; break;
        case CLIKE_TOKEN_FLOAT: name = "real";    vt = TYPE_REAL;    break;
        case CLIKE_TOKEN_STR:   name = "string";  vt = TYPE_STRING;  break;
        case CLIKE_TOKEN_TEXT:  name = "text";    vt = TYPE_FILE;    break;
        case CLIKE_TOKEN_CHAR:  name = "char";    vt = TYPE_CHAR;    break;
        default: return NULL;
    }
    Token* tok = makeIdentTokenLocal(name);
    AST* node = newASTNode(AST_VARIABLE, tok);
    setTypeAST(node, vt);
    return node;
}

static void beginScope(FuncContext* ctx) { ctx->scopeDepth++; }

static void endScope(FuncContext* ctx) {
    while (ctx->localCount > ctx->paramCount &&
           ctx->locals[ctx->localCount - 1].depth >= ctx->scopeDepth) {
        free(ctx->locals[ctx->localCount - 1].name);
        free(ctx->locals[ctx->localCount - 1].arrayDims);
        ctx->localCount--;
    }
    ctx->scopeDepth--;
}

static int addLocal(FuncContext* ctx, const char* name, VarType type, int isArray, int dimCount, int* arrayDims, VarType elemType) {
    ctx->locals[ctx->localCount].name = strdup(name);
    ctx->locals[ctx->localCount].index = ctx->localCount;
    ctx->locals[ctx->localCount].type = type;
    ctx->locals[ctx->localCount].depth = ctx->scopeDepth;
    ctx->locals[ctx->localCount].isArray = isArray;
    ctx->locals[ctx->localCount].dimCount = dimCount;
    ctx->locals[ctx->localCount].arrayDims = NULL;
    if (isArray && dimCount > 0 && arrayDims) {
        ctx->locals[ctx->localCount].arrayDims = (int*)malloc(sizeof(int) * dimCount);
        memcpy(ctx->locals[ctx->localCount].arrayDims, arrayDims, sizeof(int) * dimCount);
    }
    ctx->locals[ctx->localCount].elemType = elemType;
    ctx->localCount++;
    if (ctx->localCount > ctx->maxLocalCount) ctx->maxLocalCount = ctx->localCount;
    return ctx->localCount - 1;
}

static int resolveLocal(FuncContext* ctx, const char* name) {
    for (int i = ctx->localCount - 1; i >= 0; i--) {
        if (strcmp(ctx->locals[i].name, name) == 0) return ctx->locals[i].index;
    }
    return -1;
}

static void compileStatement(ASTNodeClike *node, BytecodeChunk *chunk, FuncContext* ctx);
static void compileExpression(ASTNodeClike *node, BytecodeChunk *chunk, FuncContext* ctx);
static void collectLocals(ASTNodeClike *node, FuncContext* ctx);
static int countLocalDecls(ASTNodeClike *node);

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
    } else if (node->type == TCAST_ARRAY_ACCESS) {
        for (int i = 0; i < node->child_count; ++i) {
            compileExpression(node->children[i], chunk, ctx);
        }
        if (node->left && node->left->type == TCAST_IDENTIFIER) {
            char* name = tokenToCString(node->left->token);
            int idx = resolveLocal(ctx, name);
            free(name);
            writeBytecodeChunk(chunk, OP_GET_LOCAL_ADDRESS, node->left->token.line);
            writeBytecodeChunk(chunk, (uint8_t)idx, node->left->token.line);
        } else {
            compileExpression(node->left, chunk, ctx);
        }
        writeBytecodeChunk(chunk, OP_GET_ELEMENT_ADDRESS, node->token.line);
        writeBytecodeChunk(chunk, (uint8_t)node->child_count, node->token.line);
    } else if (node->type == TCAST_MEMBER) {
        compileExpression(node->left, chunk, ctx);
        if (node->right && node->right->type == TCAST_IDENTIFIER) {
            char *fname = tokenToCString(node->right->token);
            int idx = addStringConstant(chunk, fname);
            if (idx < 256) {
                writeBytecodeChunk(chunk, OP_GET_FIELD_ADDRESS, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
            } else {
                writeBytecodeChunk(chunk, OP_GET_FIELD_ADDRESS16, node->token.line);
                emitShort(chunk, (uint16_t)idx, node->token.line);
            }
            free(fname);
        }
    }
}

static int countLocalDecls(ASTNodeClike *node) {
    if (!node) return 0;
    int count = (node->type == TCAST_VAR_DECL) ? 1 : 0;
    count += countLocalDecls(node->left);
    count += countLocalDecls(node->right);
    count += countLocalDecls(node->third);
    for (int i = 0; i < node->child_count; i++) {
        count += countLocalDecls(node->children[i]);
    }
    return count;
}

static void collectLocals(ASTNodeClike *node, FuncContext* ctx) {
    if (!node) return;
    for (int i = 0; i < node->child_count; i++) {
        ASTNodeClike* child = node->children[i];
        if (child->type == TCAST_VAR_DECL) {
            char* name = tokenToCString(child->token);
            addLocal(ctx, name, child->var_type, child->is_array, child->dim_count, child->array_dims, child->element_type);
            free(name);
        }
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
                bool needPop = true;
                if (node->left->type == TCAST_CALL) {
                    char* name = tokenToCString(node->left->token);
                    Symbol* sym = procedure_table ? hashTableLookup(procedure_table, name) : NULL;
                    BuiltinRoutineType btype = getBuiltinType(name);
                    if ((sym && sym->type == TYPE_VOID) || btype == BUILTIN_TYPE_PROCEDURE) {
                        needPop = false;
                    }
                    free(name);
                }
                if (needPop) {
                    writeBytecodeChunk(chunk, OP_POP, node->token.line);
                }
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
            LoopInfo* loop = &ctx->loops[ctx->loopDepth++];
            loop->breakCount = loop->continueCount = 0;
            int loopStart = chunk->count;
            compileExpression(node->left, chunk, ctx);
            writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, node->token.line);
            int exitJump = chunk->count;
            emitShort(chunk, 0xFFFF, node->token.line);
            compileStatement(node->right, chunk, ctx);
            for (int i = 0; i < loop->continueCount; i++) {
                patchShort(chunk, loop->continueAddrs[i], (uint16_t)(loopStart - (loop->continueAddrs[i] + 2)));
            }
            writeBytecodeChunk(chunk, OP_JUMP, node->token.line);
            int backOffset = loopStart - (chunk->count + 2);
            emitShort(chunk, (uint16_t)backOffset, node->token.line);
            int loopEnd = chunk->count;
            uint16_t exitOffset = (uint16_t)(loopEnd - (exitJump + 2));
            patchShort(chunk, exitJump, exitOffset);
            for (int i = 0; i < loop->breakCount; i++) {
                patchShort(chunk, loop->breakAddrs[i], (uint16_t)(loopEnd - (loop->breakAddrs[i] + 2)));
            }
            ctx->loopDepth--;
            break;
        }
        case TCAST_FOR: {
            LoopInfo* loop = &ctx->loops[ctx->loopDepth++];
            loop->breakCount = loop->continueCount = 0;
            beginScope(ctx);
            if (node->left) {
                if (node->left->type == TCAST_VAR_DECL) {
                    char* name = tokenToCString(node->left->token);
                    addLocal(ctx, name, node->left->var_type, node->left->is_array,
                             node->left->dim_count, node->left->array_dims,
                             node->left->element_type);
                    free(name);
                    compileStatement(node->left, chunk, ctx);
                } else {
                    compileExpression(node->left, chunk, ctx);
                    writeBytecodeChunk(chunk, OP_POP, node->token.line);
                }
            }
            int loopStart = chunk->count;
            int exitJump = -1;
            if (node->right) {
                compileExpression(node->right, chunk, ctx);
                writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, node->token.line);
                exitJump = chunk->count;
                emitShort(chunk, 0xFFFF, node->token.line);
            }
            ASTNodeClike* body = node->child_count > 0 ? node->children[0] : NULL;
            compileStatement(body, chunk, ctx);
            int postStart = chunk->count;
            for (int i = 0; i < loop->continueCount; i++) {
                patchShort(chunk, loop->continueAddrs[i], (uint16_t)(postStart - (loop->continueAddrs[i] + 2)));
            }
            if (node->third) {
                compileExpression(node->third, chunk, ctx);
                writeBytecodeChunk(chunk, OP_POP, node->token.line);
            }
            writeBytecodeChunk(chunk, OP_JUMP, node->token.line);
            int backOffset = loopStart - (chunk->count + 2);
            emitShort(chunk, (uint16_t)backOffset, node->token.line);
            int loopEnd = chunk->count;
            if (exitJump != -1) {
                uint16_t exitOffset = (uint16_t)(loopEnd - (exitJump + 2));
                patchShort(chunk, exitJump, exitOffset);
            }
            for (int i = 0; i < loop->breakCount; i++) {
                patchShort(chunk, loop->breakAddrs[i], (uint16_t)(loopEnd - (loop->breakAddrs[i] + 2)));
            }
            endScope(ctx);
            ctx->loopDepth--;
            break;
        }
        case TCAST_DO_WHILE: {
            LoopInfo* loop = &ctx->loops[ctx->loopDepth++];
            loop->breakCount = loop->continueCount = 0;
            int loopStart = chunk->count;
            compileStatement(node->right, chunk, ctx);
            int continueTarget = chunk->count;
            for (int i = 0; i < loop->continueCount; i++) {
                patchShort(chunk, loop->continueAddrs[i], (uint16_t)(continueTarget - (loop->continueAddrs[i] + 2)));
            }
            compileExpression(node->left, chunk, ctx);
            writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, node->token.line);
            int exitJump = chunk->count;
            emitShort(chunk, 0xFFFF, node->token.line);
            writeBytecodeChunk(chunk, OP_JUMP, node->token.line);
            int backOffset = loopStart - (chunk->count + 2);
            emitShort(chunk, (uint16_t)backOffset, node->token.line);
            int loopEnd = chunk->count;
            uint16_t exitOffset = (uint16_t)(loopEnd - (exitJump + 2));
            patchShort(chunk, exitJump, exitOffset);
            for (int i = 0; i < loop->breakCount; i++) {
                patchShort(chunk, loop->breakAddrs[i], (uint16_t)(loopEnd - (loop->breakAddrs[i] + 2)));
            }
            ctx->loopDepth--;
            break;
        }
        case TCAST_SWITCH: {
            LoopInfo* loop = &ctx->loops[ctx->loopDepth++];
            loop->breakCount = loop->continueCount = 0;
            compileExpression(node->left, chunk, ctx);
            int *endJumps = NULL; int endCount = 0;
            for (int i = 0; i < node->child_count; ++i) {
                ASTNodeClike *br = node->children[i];
                writeBytecodeChunk(chunk, OP_DUP, node->token.line);
                compileExpression(br->left, chunk, ctx);
                writeBytecodeChunk(chunk, OP_EQUAL, node->token.line);
                writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, node->token.line);
                int skip = chunk->count; emitShort(chunk, 0xFFFF, node->token.line);
                writeBytecodeChunk(chunk, OP_POP, node->token.line);
                for (int j = 0; j < br->child_count; ++j) {
                    compileStatement(br->children[j], chunk, ctx);
                }
                writeBytecodeChunk(chunk, OP_JUMP, node->token.line);
                int endJump = chunk->count; emitShort(chunk, 0xFFFF, node->token.line);
                endJumps = realloc(endJumps, sizeof(int)*(endCount+1)); endJumps[endCount++] = endJump;
                patchShort(chunk, skip, (uint16_t)(chunk->count - (skip + 2)));
            }
            if (node->right) {
                writeBytecodeChunk(chunk, OP_POP, node->token.line);
                compileStatement(node->right, chunk, ctx);
            } else {
                writeBytecodeChunk(chunk, OP_POP, node->token.line);
            }
            int end = chunk->count;
            for (int i = 0; i < endCount; ++i) {
                patchShort(chunk, endJumps[i], (uint16_t)(end - (endJumps[i] + 2)));
            }
            free(endJumps);
            for (int i = 0; i < loop->breakCount; i++) {
                patchShort(chunk, loop->breakAddrs[i], (uint16_t)(end - (loop->breakAddrs[i] + 2)));
            }
            ctx->loopDepth--;
            break;
        }
        case TCAST_BREAK: {
            writeBytecodeChunk(chunk, OP_JUMP, node->token.line);
            int patch = chunk->count;
            emitShort(chunk, 0xFFFF, node->token.line);
            if (ctx->loopDepth > 0) {
                LoopInfo* loop = &ctx->loops[ctx->loopDepth - 1];
                loop->breakAddrs[loop->breakCount++] = patch;
            }
            break;
        }
        case TCAST_CONTINUE: {
            writeBytecodeChunk(chunk, OP_JUMP, node->token.line);
            int patch = chunk->count;
            emitShort(chunk, 0xFFFF, node->token.line);
            if (ctx->loopDepth > 0) {
                LoopInfo* loop = &ctx->loops[ctx->loopDepth - 1];
                loop->continueAddrs[loop->continueCount++] = patch;
            }
            break;
        }
        case TCAST_VAR_DECL: {
            char* name = tokenToCString(node->token);
            int idx = resolveLocal(ctx, name);
            free(name);
            if (node->var_type == TYPE_POINTER) {
                if (node->left) {
                    compileExpression(node->left, chunk, ctx);
                } else {
                    AST *base = NULL;
                    if (node->right && node->right->type == TCAST_IDENTIFIER) {
                        if (node->right->token.type == CLIKE_TOKEN_IDENTIFIER) {
                            char *tname = tokenToCString(node->right->token);
                            base = clike_lookup_struct(tname);
                            if (!base) base = lookupType(tname);
                            free(tname);
                        } else {
                            base = makeBuiltinTypeASTFromToken(node->right->token);
                        }
                    }
                    Value init;
                    if (base) {
                        init = makePointer(NULL, base);
                    } else {
                        init = makeValueForType(TYPE_POINTER, NULL, NULL);
                    }
                    int cidx = addConstantToChunk(chunk, &init);
                    freeValue(&init);
                    writeBytecodeChunk(chunk, OP_CONSTANT, node->token.line);
                    writeBytecodeChunk(chunk, (uint8_t)cidx, node->token.line);
                }
                writeBytecodeChunk(chunk, OP_SET_LOCAL, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
            } else if (node->is_array) {
                int elemNameIdx = addStringConstant(chunk, "");
                writeBytecodeChunk(chunk, OP_INIT_LOCAL_ARRAY, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)node->dim_count, node->token.line);
                for (int d = 0; d < node->dim_count; ++d) {
                    Value lower = makeInt(0);
                    Value upper = makeInt(node->array_dims[d] - 1);
                    int lidx = addConstantToChunk(chunk, &lower);
                    int uidx = addConstantToChunk(chunk, &upper);
                    freeValue(&lower);
                    freeValue(&upper);
                    emitShort(chunk, (uint16_t)lidx, node->token.line);
                    emitShort(chunk, (uint16_t)uidx, node->token.line);
                }
                writeBytecodeChunk(chunk, (uint8_t)node->element_type, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)elemNameIdx, node->token.line);
            } else {
                if (node->left) {
                    compileExpression(node->left, chunk, ctx);
                } else {
                    Value init;
                    switch (node->var_type) {
                        case TYPE_REAL:
                            init = makeReal(0.0);
                            break;
                        case TYPE_STRING:
                            init = makeNil();
                            break;
                        case TYPE_FILE:
                            init = makeValueForType(TYPE_FILE, NULL, NULL);
                            break;
                        default:
                            init = makeInt(0);
                            break;
                    }
                    int cidx = addConstantToChunk(chunk, &init);
                    freeValue(&init);
                    writeBytecodeChunk(chunk, OP_CONSTANT, node->token.line);
                    writeBytecodeChunk(chunk, (uint8_t)cidx, node->token.line);
                }
                writeBytecodeChunk(chunk, OP_SET_LOCAL, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
            }
            break;
        }
        case TCAST_COMPOUND:
            beginScope(ctx);
            collectLocals(node, ctx);
            for (int i = 0; i < node->child_count; i++) {
                compileStatement(node->children[i], chunk, ctx);
            }
            endScope(ctx);
            break;
        default:
            break;
    }
}

static void compileExpression(ASTNodeClike *node, BytecodeChunk *chunk, FuncContext* ctx) {
    if (!node) return;
    switch (node->type) {
        case TCAST_NUMBER: {
            Value v;
            if (node->token.type == CLIKE_TOKEN_FLOAT_LITERAL) {
                v = makeReal(node->token.float_val);
            } else {
                v = makeInt(node->token.int_val);
            }
            int idx = addConstantToChunk(chunk, &v);
            writeBytecodeChunk(chunk, OP_CONSTANT, node->token.line);
            writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
            break;
        }
        case TCAST_STRING: {
            char* s = tokenStringToCString(node->token);
            Value v = makeString(s);
            free(s);
            int idx = addConstantToChunk(chunk, &v);
            freeValue(&v);
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
                case CLIKE_TOKEN_SLASH:
                    if (node->var_type == TYPE_INTEGER &&
                        node->left && node->right &&
                        node->left->var_type == TYPE_INTEGER &&
                        node->right->var_type == TYPE_INTEGER) {
                        /*
                         * In C, dividing two integers performs integer division
                         * (truncating toward zero).  The VM has a dedicated
                         * opcode for this behaviour (OP_INT_DIV), whereas
                         * OP_DIVIDE always produces a real result.  Without this
                         * check, expressions like `w / 4` would yield a real
                         * value, which breaks APIs expecting integer arguments
                         * (e.g. drawrect in the graphics tests).
                         */
                        writeBytecodeChunk(chunk, OP_INT_DIV, node->token.line);
                    } else {
                        writeBytecodeChunk(chunk, OP_DIVIDE, node->token.line);
                    }
                    break;
                case CLIKE_TOKEN_GREATER: writeBytecodeChunk(chunk, OP_GREATER, node->token.line); break;
                case CLIKE_TOKEN_GREATER_EQUAL: writeBytecodeChunk(chunk, OP_GREATER_EQUAL, node->token.line); break;
                case CLIKE_TOKEN_LESS: writeBytecodeChunk(chunk, OP_LESS, node->token.line); break;
                case CLIKE_TOKEN_LESS_EQUAL: writeBytecodeChunk(chunk, OP_LESS_EQUAL, node->token.line); break;
                case CLIKE_TOKEN_EQUAL_EQUAL: writeBytecodeChunk(chunk, OP_EQUAL, node->token.line); break;
                case CLIKE_TOKEN_BANG_EQUAL: writeBytecodeChunk(chunk, OP_NOT_EQUAL, node->token.line); break;
                case CLIKE_TOKEN_AND_AND: writeBytecodeChunk(chunk, OP_AND, node->token.line); break;
                case CLIKE_TOKEN_OR_OR: writeBytecodeChunk(chunk, OP_OR, node->token.line); break;
                case CLIKE_TOKEN_BIT_AND: writeBytecodeChunk(chunk, OP_AND, node->token.line); break;
                case CLIKE_TOKEN_BIT_OR: writeBytecodeChunk(chunk, OP_OR, node->token.line); break;
                case CLIKE_TOKEN_SHL: writeBytecodeChunk(chunk, OP_SHL, node->token.line); break;
                case CLIKE_TOKEN_SHR: writeBytecodeChunk(chunk, OP_SHR, node->token.line); break;
                default: break;
            }
            break;
        case TCAST_UNOP:
            compileExpression(node->left, chunk, ctx);
            switch (node->token.type) {
                case CLIKE_TOKEN_MINUS: writeBytecodeChunk(chunk, OP_NEGATE, node->token.line); break;
                case CLIKE_TOKEN_BANG: writeBytecodeChunk(chunk, OP_NOT, node->token.line); break;
                case CLIKE_TOKEN_TILDE: writeBytecodeChunk(chunk, OP_NOT, node->token.line); break;
                default: break;
            }
            break;
        case TCAST_ADDR:
            compileLValue(node->left, chunk, ctx);
            break;
        case TCAST_DEREF:
            compileExpression(node->left, chunk, ctx);
            writeBytecodeChunk(chunk, OP_GET_INDIRECT, node->token.line);
            break;
        case TCAST_ASSIGN: {
            if (node->left) {
                if (node->left->type == TCAST_IDENTIFIER) {
                    char* name = tokenToCString(node->left->token);
                    int idx = resolveLocal(ctx, name);
                    free(name);
                    compileExpression(node->right, chunk, ctx);
                    writeBytecodeChunk(chunk, OP_DUP, node->token.line);
                    writeBytecodeChunk(chunk, OP_SET_LOCAL, node->token.line);
                    writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
                } else if (node->left->type == TCAST_ARRAY_ACCESS) {
                    /*
                     * In C, an assignment expression evaluates to the value
                     * being assigned.  Our VM's OP_SET_INDIRECT no longer
                     * leaves a copy of the value on the stack (Pascal
                     * semantics), so we must explicitly preserve it for the
                     * expression result.
                     *
                     * Evaluate the right-hand side first and duplicate it so
                     * one copy remains after the store.  Then compute the
                     * l-value address, swap to place the address beneath the
                     * value and perform the indirect assignment.  The
                     * duplicated value is left on the stack as the expression
                     * result, allowing surrounding expression statements to
                     * pop it without disturbing the stack.
                     */
                    compileExpression(node->right, chunk, ctx);      // [..., value]
                    writeBytecodeChunk(chunk, OP_DUP, node->token.line); // [..., value, value]
                    compileLValue(node->left, chunk, ctx);            // [..., value, value, ptr]
                    writeBytecodeChunk(chunk, OP_SWAP, node->token.line); // [..., value, ptr, value]
                    writeBytecodeChunk(chunk, OP_SET_INDIRECT, node->token.line); // [..., value]
                } else if (node->left->type == TCAST_DEREF) {
                    compileExpression(node->right, chunk, ctx);      // [..., value]
                    writeBytecodeChunk(chunk, OP_DUP, node->token.line); // [..., value, value]
                    compileExpression(node->left->left, chunk, ctx); // [..., value, value, ptr]
                    writeBytecodeChunk(chunk, OP_SWAP, node->token.line); // [..., value, ptr, value]
                    writeBytecodeChunk(chunk, OP_SET_INDIRECT, node->token.line); // [..., value]
                } else if (node->left->type == TCAST_MEMBER) {
                    compileExpression(node->right, chunk, ctx);      // [..., value]
                    writeBytecodeChunk(chunk, OP_DUP, node->token.line); // [..., value, value]
                    compileLValue(node->left, chunk, ctx);           // [..., value, value, ptr]
                    writeBytecodeChunk(chunk, OP_SWAP, node->token.line); // [..., value, ptr, value]
                    writeBytecodeChunk(chunk, OP_SET_INDIRECT, node->token.line); // [..., value]
                }
            }
            break;
        }
        case TCAST_IDENTIFIER: {
            char* name = tokenToCString(node->token);
            if (strcmp(name, "NULL") == 0) {
                Value v; memset(&v, 0, sizeof(Value));
                v.type = TYPE_POINTER;
                int cidx = addConstantToChunk(chunk, &v);
                writeBytecodeChunk(chunk, OP_CONSTANT, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)cidx, node->token.line);
                free(name);
                break;
            }
            int idx = resolveLocal(ctx, name);
            free(name);
            writeBytecodeChunk(chunk, OP_GET_LOCAL, node->token.line);
            writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
            break;
        }
        case TCAST_ARRAY_ACCESS:
            compileLValue(node, chunk, ctx);
            writeBytecodeChunk(chunk, OP_GET_INDIRECT, node->token.line);
            break;
        case TCAST_MEMBER:
            compileExpression(node->left, chunk, ctx);
            if (node->right && node->right->type == TCAST_IDENTIFIER) {
                char *fname = tokenToCString(node->right->token);
                int idx = addStringConstant(chunk, fname);
                if (idx < 256) {
                    writeBytecodeChunk(chunk, OP_GET_FIELD_ADDRESS, node->token.line);
                    writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
                } else {
                    writeBytecodeChunk(chunk, OP_GET_FIELD_ADDRESS16, node->token.line);
                    emitShort(chunk, (uint16_t)idx, node->token.line);
                }
                free(fname);
            }
            writeBytecodeChunk(chunk, OP_GET_INDIRECT, node->token.line);
            break;
        case TCAST_CALL: {
            char *name = tokenToCString(node->token);
            if (strcmp(name, "printf") == 0) {
                for (int i = 0; i < node->child_count; ++i) {
                    compileExpression(node->children[i], chunk, ctx);
                }
                // Map printf to the Write opcode so no implicit newline is
                // emitted. printf in clike is treated as a procedure that
                // always succeeds and returns 0.
                writeBytecodeChunk(chunk, OP_WRITE, node->token.line);
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
            } else if (strcmp(name, "assign") == 0 ||
                       strcmp(name, "reset") == 0 ||
                       strcmp(name, "eof") == 0 ||
                       strcmp(name, "close") == 0) {
                // File builtins take the file variable as a VAR parameter.
                if (node->child_count > 0) {
                    compileLValue(node->children[0], chunk, ctx);
                    for (int i = 1; i < node->child_count; ++i) {
                        compileExpression(node->children[i], chunk, ctx);
                    }
                }
                int fnIndex = addStringConstant(chunk, name);
                writeBytecodeChunk(chunk, OP_CALL_BUILTIN, node->token.line);
                emitShort(chunk, (uint16_t)fnIndex, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)node->child_count, node->token.line);
            } else if (strcmp(name, "readln") == 0) {
                // readln requires all arguments by reference (file and buffer).
                for (int i = 0; i < node->child_count; ++i) {
                    compileLValue(node->children[i], chunk, ctx);
                }
                int rlIndex = addStringConstant(chunk, "readln");
                writeBytecodeChunk(chunk, OP_CALL_BUILTIN, node->token.line);
                emitShort(chunk, (uint16_t)rlIndex, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)node->child_count, node->token.line);
            } else if (strcmp(name, "random") == 0) {
                // Direct wrapper around the VM's random builtin.
                for (int i = 0; i < node->child_count; ++i) {
                    compileExpression(node->children[i], chunk, ctx);
                }
                int rIndex = addStringConstant(chunk, "random");
                writeBytecodeChunk(chunk, OP_CALL_BUILTIN, node->token.line);
                emitShort(chunk, (uint16_t)rIndex, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)node->child_count, node->token.line);
            } else if (strcmp(name, "strlen") == 0) {
                // Map C's strlen to the Pascal-style length builtin.
                for (int i = 0; i < node->child_count; ++i) {
                    compileExpression(node->children[i], chunk, ctx);
                }
                int lenIndex = addStringConstant(chunk, "length");
                writeBytecodeChunk(chunk, OP_CALL_BUILTIN, node->token.line);
                emitShort(chunk, (uint16_t)lenIndex, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)node->child_count, node->token.line);
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
            addLocal(&ctx, name, p->var_type, 0, 0, NULL, TYPE_UNKNOWN);
            free(name);
            ctx.paramCount++;
        }
    }
    /*
     * Track the maximum number of locals that exist at any point during
     * compilation.  The call frame must allocate space for this many locals
     * (minus the parameters) when the function is executed.  We also perform a
     * recursive pre-pass to count every local declaration in the function. If
     * the pre-pass reports more locals than were ever simultaneously live, we
     * still allocate the larger amount to be safe.
     */
    int declaredLocals = countLocalDecls(func->right);
    ctx.maxLocalCount = ctx.localCount;

    int address = chunk->count;
    char* fname = tokenToCString(func->token);
    Symbol* sym = malloc(sizeof(Symbol));
    memset(sym, 0, sizeof(Symbol));
    sym->name = strdup(fname);
    sym->bytecode_address = address;
    sym->arity = (uint8_t)ctx.paramCount;
    sym->type = func->var_type;
    sym->is_defined = true;
    hashTableInsert(procedure_table, sym);

    compileStatement(func->right, chunk, &ctx);
    writeBytecodeChunk(chunk, OP_RETURN, func->token.line);

    /* The total locals required are whichever is larger: the maximum locals
     * seen during compilation (minus parameters) or the number of declarations
     * discovered by the pre-pass. */
    int needed = ctx.maxLocalCount - ctx.paramCount;
    if (declaredLocals > needed) needed = declaredLocals;
    sym->locals_count = (uint8_t)needed;

    for (int i = 0; i < ctx.paramCount; i++) {
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

    // Compile imported modules before the main program
    for (int i = 0; i < clike_import_count; ++i) {
        const char *orig_path = clike_imports[i];
        const char *path = orig_path;
        char *allocated_path = NULL;
        FILE *f = fopen(path, "rb");
        if (!f) {
            const char *lib_dir = getenv("CLIKE_LIB_DIR");
            if (lib_dir && *lib_dir) {
                size_t len = strlen(lib_dir) + 1 + strlen(orig_path) + 1;
                allocated_path = (char*)malloc(len);
                snprintf(allocated_path, len, "%s/%s", lib_dir, orig_path);
                f = fopen(allocated_path, "rb");
                if (f) path = allocated_path; else { free(allocated_path); allocated_path = NULL; }
            }
        }
        if (!f) {
            const char *default_dir = "/usr/local/pscal/clike/lib";
            size_t len = strlen(default_dir) + 1 + strlen(orig_path) + 1;
            allocated_path = (char*)malloc(len);
            snprintf(allocated_path, len, "%s/%s", default_dir, orig_path);
            f = fopen(allocated_path, "rb");
            if (f) path = allocated_path; else { free(allocated_path); allocated_path = NULL; }
        }
        if (!f) {
            fprintf(stderr, "Could not open import '%s'\n", orig_path);
            continue;
        }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        rewind(f);
        char *src = (char*)malloc(len + 1);
        fread(src, 1, len, f);
        src[len] = '\0';
        fclose(f);

        ParserClike p; initParserClike(&p, src);
        ASTNodeClike *modProg = parseProgramClike(&p);

        if (!verifyASTClikeLinks(modProg, NULL)) {
            fprintf(stderr, "AST verification failed for module '%s' after parsing.\n", path);
            freeASTClike(modProg);
            free(src);
            exit(1);
        }

        analyzeSemanticsClike(modProg);

        if (!verifyASTClikeLinks(modProg, NULL)) {
            fprintf(stderr, "AST verification failed for module '%s' after semantic analysis.\n", path);
            freeASTClike(modProg);
            free(src);
            exit(1);
        }
        for (int j = 0; j < modProg->child_count; ++j) {
            ASTNodeClike *decl = modProg->children[j];
            if (decl->type == TCAST_FUN_DECL) {
                compileFunction(decl, chunk);
            }
        }
        freeASTClike(modProg);
        free(src);
        if (allocated_path) free(allocated_path);
    }

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

    for (int i = 0; i < clike_import_count; ++i) {
        free(clike_imports[i]);
    }
    free(clike_imports);
    clike_imports = NULL;
    clike_import_count = 0;
}

