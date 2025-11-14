#include "clike/codegen.h"
#include "clike/builtins.h"
#include "backend_ast/builtin.h"
#include "clike/parser.h"
#include "clike/semantics.h"
#include "ast/ast.h"
#include "core/types.h"
#include "core/utils.h"
#include "symbol/symbol.h"
#include "Pascal/globals.h"
#include "compiler/compiler.h"
#include "vm/string_sentinels.h"
#include "vm/vm.h"
#include "pscal_paths.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>

typedef struct {
    char *name;
    int name_idx;   // constant pool index for the variable name
    VarType type;
    VarType elemType;
} GlobalVar;

static GlobalVar globalVars[256];
static int globalVarCount = 0;

typedef struct {
    char *name;
    int index;
    VarType type;
    int depth;
    int isArray;
    int *arrayDims;
    int dimCount;
    VarType elemType;
    int isActive;
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

static int addBuiltinNameConstant(BytecodeChunk* chunk,
                                  const char* encoded_name,
                                  const char* canonical_hint) {
    if (!chunk) {
        return -1;
    }
    if (!encoded_name) {
        encoded_name = "";
    }
    int name_index = addStringConstant(chunk, encoded_name);
    if (name_index < 0) {
        return name_index;
    }

    if (getBuiltinLowercaseIndex(chunk, name_index) >= 0) {
        return name_index;
    }

    const char* lower_source = canonical_hint && *canonical_hint ? canonical_hint : encoded_name;
    char lowered[MAX_SYMBOL_LENGTH];
    strncpy(lowered, lower_source, sizeof(lowered) - 1);
    lowered[sizeof(lowered) - 1] = '\0';
    toLowerString(lowered);

    Value lower_val = makeString(lowered);
    int lower_index = addConstantToChunk(chunk, &lower_val);
    freeValue(&lower_val);
    setBuiltinLowercaseIndex(chunk, name_index, lower_index);
    return name_index;
}

static void emitConstantOperand(BytecodeChunk* chunk, int constant_index, int line) {
    if (!chunk) {
        return;
    }
    if (constant_index < 0) {
        fprintf(stderr, "L%d: Compiler Error: negative constant index.\n", line);
        return;
    }
    if (constant_index <= 0xFF) {
        writeBytecodeChunk(chunk, CONSTANT, line);
        writeBytecodeChunk(chunk, (uint8_t)constant_index, line);
        return;
    }
    if (constant_index <= 0xFFFF) {
        writeBytecodeChunk(chunk, CONSTANT16, line);
        emitShort(chunk, (uint16_t)constant_index, line);
        return;
    }

    fprintf(stderr,
            "L%d: Compiler Error: too many constants (%d). Limit is 65535.\n",
            line,
            constant_index);
}

static void emitBuiltinProcedureCall(BytecodeChunk* chunk, const char* vmName,
                                    uint8_t arg_count, int line) {
    if (!vmName) vmName = "";

    const char* dispatch_name = clikeCanonicalBuiltinName(vmName);
    char normalized_name[MAX_SYMBOL_LENGTH];
    strncpy(normalized_name, dispatch_name, sizeof(normalized_name) - 1);
    normalized_name[sizeof(normalized_name) - 1] = '\0';
    toLowerString(normalized_name);

    int nameIndex = addBuiltinNameConstant(chunk, normalized_name, dispatch_name);
    int builtin_id = clikeGetBuiltinID(vmName);
    if (builtin_id < 0) {
        fprintf(stderr,
                "L%d: Compiler Error: Unknown built-in procedure '%s'.\n",
                line, vmName);
        writeBytecodeChunk(chunk, CALL_BUILTIN, line);
        emitShort(chunk, (uint16_t)nameIndex, line);
        writeBytecodeChunk(chunk, arg_count, line);
        return;
    }

    writeBytecodeChunk(chunk, CALL_BUILTIN_PROC, line);
    emitShort(chunk, (uint16_t)builtin_id, line);
    emitShort(chunk, (uint16_t)nameIndex, line);
    writeBytecodeChunk(chunk, arg_count, line);
}

static void emitBuiltinFunctionCall(BytecodeChunk* chunk, const char* vmName,
                                   uint8_t arg_count, int line) {
    if (!vmName) vmName = "";

    const char* dispatch_name = clikeCanonicalBuiltinName(vmName);
    char normalized_name[MAX_SYMBOL_LENGTH];
    strncpy(normalized_name, dispatch_name, sizeof(normalized_name) - 1);
    normalized_name[sizeof(normalized_name) - 1] = '\0';
    toLowerString(normalized_name);

    int nameIndex = addBuiltinNameConstant(chunk, normalized_name, dispatch_name);
    if (clikeGetBuiltinID(vmName) < 0) {
        fprintf(stderr,
                "L%d: Compiler Error: Unknown built-in function '%s'.\n",
                line, vmName);
    }

    writeBytecodeChunk(chunk, CALL_BUILTIN, line);
    emitShort(chunk, (uint16_t)nameIndex, line);
    writeBytecodeChunk(chunk, arg_count, line);
}

static bool isNumericPrintfSpec(char spec) {
    switch (spec) {
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            return true;
        default:
            return false;
    }
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
    ctx->locals[ctx->localCount].isActive = 0;
    ctx->localCount++;
    if (ctx->localCount > ctx->maxLocalCount) ctx->maxLocalCount = ctx->localCount;
    return ctx->localCount - 1;
}

static GlobalVar* findGlobalEntry(const char* name) {
    for (int i = 0; i < globalVarCount; ++i) {
        if (strcmp(globalVars[i].name, name) == 0) {
            return &globalVars[i];
        }
    }
    return NULL;
}

static void registerGlobal(const char* name, VarType type, VarType elemType, int name_idx) {
    if (globalVarCount >= (int)(sizeof(globalVars)/sizeof(globalVars[0]))) {
        fprintf(stderr, "CLike codegen error: too many globals (limit %zu)\n",
                sizeof(globalVars)/sizeof(globalVars[0]));
        return;
    }
    GlobalVar* entry = &globalVars[globalVarCount++];
    entry->name = strdup(name);
    entry->type = type;
    entry->elemType = elemType;
    entry->name_idx = name_idx;
}

static int resolveGlobal(const char* name) {
    GlobalVar* entry = findGlobalEntry(name);
    return entry ? entry->name_idx : -1;
}

// Return the constant-pool index of the global's name string. If the global
// was previously registered during compilation we reuse its name index;
// otherwise we add the name as a new string constant so the VM can resolve it
// at runtime when the definition is encountered.
static int getGlobalNameConstIndex(BytecodeChunk* chunk, const char* name) {
    int gidx = resolveGlobal(name);
    if (gidx >= 0) return gidx; // already present in globals registry
    return addStringConstant(chunk, name);
}

static int resolveLocal(FuncContext* ctx, const char* name); // forward declaration for helpers

static int isCharPointerLocal(FuncContext* ctx, int idx) {
    if (!ctx) return 0;
    if (idx < 0 || idx >= ctx->localCount) return 0;
    LocalVar* local = &ctx->locals[idx];
    return local->type == TYPE_POINTER && local->elemType == TYPE_CHAR;
}

static int identifierIsCharPointer(ASTNodeClike* ident, FuncContext* ctx) {
    if (!ident || ident->type != TCAST_IDENTIFIER) return 0;
    char* name = tokenToCString(ident->token);
    int isCharPtr = 0;
    if (ctx) {
        int idx = resolveLocal(ctx, name);
        if (idx >= 0) {
            isCharPtr = isCharPointerLocal(ctx, idx);
        } else {
            GlobalVar* entry = findGlobalEntry(name);
            if (entry && entry->type == TYPE_POINTER && entry->elemType == TYPE_CHAR) {
                isCharPtr = 1;
            }
        }
    } else {
        GlobalVar* entry = findGlobalEntry(name);
        if (entry && entry->type == TYPE_POINTER && entry->elemType == TYPE_CHAR) {
            isCharPtr = 1;
        }
    }
    free(name);
    return isCharPtr;
}

static int shouldEmitStringAsCharPointer(ASTNodeClike* node, FuncContext* ctx) {
    if (!node || node->type != TCAST_STRING) return 0;
    ASTNodeClike* parent = node->parent;
    if (!parent) return 0;

    if (parent->type == TCAST_VAR_DECL && parent->left == node) {
        return parent->var_type == TYPE_POINTER && parent->element_type == TYPE_CHAR;
    }

    if (parent->type == TCAST_ASSIGN && parent->right == node) {
        ASTNodeClike* lhs = parent->left;
        if (!lhs) return 0;
        if (lhs->type == TCAST_IDENTIFIER) {
            return identifierIsCharPointer(lhs, ctx);
        }
        if (lhs->var_type == TYPE_POINTER && lhs->element_type == TYPE_CHAR) {
            return 1;
        }
    }

    return 0;
}

static void emitCharPointerConstant(ASTNodeClike* node, BytecodeChunk* chunk) {
    if (!node || !chunk) return;
    char* raw = tokenStringToCString(node->token);
    Value strVal = makeString(raw);
    free(raw);
    int strIdx = addConstantToChunk(chunk, &strVal);
    freeValue(&strVal);
    Value ptrVal = makePointer(chunk->constants[strIdx].s_val, STRING_CHAR_PTR_SENTINEL);
    int ptrIdx = addConstantToChunk(chunk, &ptrVal);
    freeValue(&ptrVal);
    emitConstantOperand(chunk, ptrIdx, node->token.line);
}

static LocalVar* findLocalEntry(FuncContext* ctx, const char* name) {
    if (!ctx) return NULL;
    for (int i = ctx->localCount - 1; i >= 0; i--) {
        if (strcmp(ctx->locals[i].name, name) == 0) {
            return &ctx->locals[i];
        }
    }
    return NULL;
}

static int resolveLocal(FuncContext* ctx, const char* name) {
    LocalVar* entry = findLocalEntry(ctx, name);
    if (!entry || !entry->isActive) return -1;
    return entry->index;
}

static void compileStatement(ASTNodeClike *node, BytecodeChunk *chunk, FuncContext* ctx);
static void compileExpressionWithResult(ASTNodeClike *node, BytecodeChunk *chunk, FuncContext* ctx, bool resultUsed);
static void compileExpression(ASTNodeClike *node, BytecodeChunk *chunk, FuncContext* ctx);
static void collectLocals(ASTNodeClike *node, FuncContext* ctx);
static int countLocalDecls(ASTNodeClike *node);
static void compileGlobalVar(ASTNodeClike *node, BytecodeChunk *chunk);

// Helper to compile an l-value (currently only local identifiers) and push its
// address onto the stack.
static void compileLValue(ASTNodeClike *node, BytecodeChunk *chunk, FuncContext* ctx) {
    if (!node) return;
    if (node->type == TCAST_IDENTIFIER) {
        char* name = tokenToCString(node->token);
        int idx = resolveLocal(ctx, name);
        if (idx >= 0) {
            writeBytecodeChunk(chunk, GET_LOCAL_ADDRESS, node->token.line);
            writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
        } else {
            int nameIdx = getGlobalNameConstIndex(chunk, name);
            if (nameIdx < 256) {
                writeBytecodeChunk(chunk, GET_GLOBAL_ADDRESS, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)nameIdx, node->token.line);
            } else {
                writeBytecodeChunk(chunk, GET_GLOBAL_ADDRESS16, node->token.line);
                emitShort(chunk, (uint16_t)nameIdx, node->token.line);
            }
        }
        free(name);
    } else if (node->type == TCAST_ARRAY_ACCESS) {
        for (int i = 0; i < node->child_count; ++i) {
            compileExpression(node->children[i], chunk, ctx);
        }
        if (node->left && node->left->type == TCAST_IDENTIFIER) {
            char* name = tokenToCString(node->left->token);
            int idx = resolveLocal(ctx, name);
            if (idx >= 0) {
                writeBytecodeChunk(chunk, GET_LOCAL_ADDRESS, node->left->token.line);
                writeBytecodeChunk(chunk, (uint8_t)idx, node->left->token.line);
            } else {
                int nameIdx = getGlobalNameConstIndex(chunk, name);
                if (nameIdx < 256) {
                    writeBytecodeChunk(chunk, GET_GLOBAL_ADDRESS, node->left->token.line);
                    writeBytecodeChunk(chunk, (uint8_t)nameIdx, node->left->token.line);
                } else {
                    writeBytecodeChunk(chunk, GET_GLOBAL_ADDRESS16, node->left->token.line);
                    emitShort(chunk, (uint16_t)nameIdx, node->left->token.line);
                }
            }
            free(name);
        } else {
            compileExpression(node->left, chunk, ctx);
        }
        writeBytecodeChunk(chunk, GET_ELEMENT_ADDRESS, node->token.line);
        writeBytecodeChunk(chunk, (uint8_t)node->child_count, node->token.line);
    } else if (node->type == TCAST_MEMBER) {
        int needsAddress = node->token.type != CLIKE_TOKEN_ARROW;
        ASTNodeClike *base = node->left;
        if (needsAddress && base &&
            (base->type == TCAST_IDENTIFIER || base->type == TCAST_ARRAY_ACCESS ||
             base->type == TCAST_MEMBER)) {
            compileLValue(base, chunk, ctx);
        } else {
            compileExpression(base, chunk, ctx);
        }
        if (node->right && node->right->type == TCAST_IDENTIFIER) {
            char *fname = tokenToCString(node->right->token);
            int idx = addStringConstant(chunk, fname);
            if (idx < 256) {
                writeBytecodeChunk(chunk, GET_FIELD_ADDRESS, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
            } else {
                writeBytecodeChunk(chunk, GET_FIELD_ADDRESS16, node->token.line);
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
            writeBytecodeChunk(chunk, RETURN, node->token.line);
            break;
        case TCAST_THREAD_JOIN:
            if (node->left) compileExpression(node->left, chunk, ctx);
            writeBytecodeChunk(chunk, THREAD_JOIN, node->token.line);
            break;
        case TCAST_EXPR_STMT: {
            ASTNodeClike* expr = node->left;
            if (!expr) break;
            if (expr->type == TCAST_ASSIGN) {
                ASTNodeClike* lhs = expr->left;
                if (lhs && lhs->type == TCAST_IDENTIFIER) {
                    compileExpressionWithResult(expr, chunk, ctx, false);
                    break;
                }
            }
            compileExpression(expr, chunk, ctx);
            bool needPop = true;
            if (expr->type == TCAST_CALL) {
                char* name = tokenToCString(expr->token);
                Symbol* sym = procedure_table ? hashTableLookup(procedure_table, name) : NULL;
                sym = resolveSymbolAlias(sym);
                BuiltinRoutineType btype = getBuiltinType(name);
                if ((sym && sym->type == TYPE_VOID) || btype == BUILTIN_TYPE_PROCEDURE) {
                    needPop = false;
                }
                free(name);
            }
            if (needPop) {
                writeBytecodeChunk(chunk, POP, node->token.line);
            }
            break;
        }
        case TCAST_IF: {
            compileExpression(node->left, chunk, ctx);
            writeBytecodeChunk(chunk, JUMP_IF_FALSE, node->token.line);
            int elseJump = chunk->count;
            emitShort(chunk, 0xFFFF, node->token.line);
            compileStatement(node->right, chunk, ctx);
            if (node->third) {
                writeBytecodeChunk(chunk, JUMP, node->token.line);
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
            writeBytecodeChunk(chunk, JUMP_IF_FALSE, node->token.line);
            int exitJump = chunk->count;
            emitShort(chunk, 0xFFFF, node->token.line);
            compileStatement(node->right, chunk, ctx);
            for (int i = 0; i < loop->continueCount; i++) {
                patchShort(chunk, loop->continueAddrs[i], (uint16_t)(loopStart - (loop->continueAddrs[i] + 2)));
            }
            writeBytecodeChunk(chunk, JUMP, node->token.line);
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
                } else if (node->left->type == TCAST_COMPOUND) {
                    for (int i = 0; i < node->left->child_count; ++i) {
                        ASTNodeClike *child = node->left->children[i];
                        char* name = tokenToCString(child->token);
                        addLocal(ctx, name, child->var_type, child->is_array,
                                 child->dim_count, child->array_dims,
                                 child->element_type);
                        free(name);
                        compileStatement(child, chunk, ctx);
                    }
                } else {
                    compileExpression(node->left, chunk, ctx);
                    writeBytecodeChunk(chunk, POP, node->token.line);
                }
            }
            int loopStart = chunk->count;
            int exitJump = -1;
            if (node->right) {
                compileExpression(node->right, chunk, ctx);
                writeBytecodeChunk(chunk, JUMP_IF_FALSE, node->token.line);
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
                writeBytecodeChunk(chunk, POP, node->token.line);
            }
            writeBytecodeChunk(chunk, JUMP, node->token.line);
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
            writeBytecodeChunk(chunk, JUMP_IF_FALSE, node->token.line);
            int exitJump = chunk->count;
            emitShort(chunk, 0xFFFF, node->token.line);
            writeBytecodeChunk(chunk, JUMP, node->token.line);
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
                writeBytecodeChunk(chunk, DUP, node->token.line);
                compileExpression(br->left, chunk, ctx);
                writeBytecodeChunk(chunk, EQUAL, node->token.line);
                writeBytecodeChunk(chunk, JUMP_IF_FALSE, node->token.line);
                int skip = chunk->count; emitShort(chunk, 0xFFFF, node->token.line);
                writeBytecodeChunk(chunk, POP, node->token.line);
                for (int j = 0; j < br->child_count; ++j) {
                    compileStatement(br->children[j], chunk, ctx);
                }
                writeBytecodeChunk(chunk, JUMP, node->token.line);
                int endJump = chunk->count; emitShort(chunk, 0xFFFF, node->token.line);
                endJumps = realloc(endJumps, sizeof(int)*(endCount+1)); endJumps[endCount++] = endJump;
                patchShort(chunk, skip, (uint16_t)(chunk->count - (skip + 2)));
            }
            if (node->right) {
                writeBytecodeChunk(chunk, POP, node->token.line);
                compileStatement(node->right, chunk, ctx);
            } else {
                writeBytecodeChunk(chunk, POP, node->token.line);
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
            writeBytecodeChunk(chunk, JUMP, node->token.line);
            int patch = chunk->count;
            emitShort(chunk, 0xFFFF, node->token.line);
            if (ctx->loopDepth > 0) {
                LoopInfo* loop = &ctx->loops[ctx->loopDepth - 1];
                loop->breakAddrs[loop->breakCount++] = patch;
            }
            break;
        }
        case TCAST_CONTINUE: {
            writeBytecodeChunk(chunk, JUMP, node->token.line);
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
            LocalVar* local = findLocalEntry(ctx, name);
            int idx = local ? local->index : -1;
            free(name);
            AST *recordDef = NULL;
            if (node->right && node->right->type == TCAST_IDENTIFIER) {
                char *typeName = tokenToCString(node->right->token);
                recordDef = clikeLookupStruct(typeName);
                if (!recordDef) recordDef = lookupType(typeName);
                free(typeName);
            }
            if (node->var_type == TYPE_POINTER) {
                int typeNameIdx = -1;
                if (node->right && node->right->type == TCAST_IDENTIFIER) {
                    char *typeName = tokenToCString(node->right->token);
                    if (typeName) {
                        typeNameIdx = addStringConstant(chunk, typeName);
                        free(typeName);
                    }
                }
                if (typeNameIdx < 0) {
                    typeNameIdx = addStringConstant(chunk, "");
                }
                writeBytecodeChunk(chunk, INIT_LOCAL_POINTER, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
                emitShort(chunk, (uint16_t)typeNameIdx, node->token.line);
                if (node->left) {
                    compileExpression(node->left, chunk, ctx);
                    writeBytecodeChunk(chunk, SET_LOCAL, node->token.line);
                    writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
                }
            } else if (node->is_array) {
                int elemNameIdx = addStringConstant(chunk, "");
                // Compile dynamic dimension sizes before emitting the init opcode
                if (node->array_dim_exprs) {
                    for (int d = 0; d < node->dim_count; ++d) {
                        if (node->array_dims[d] == 0 && node->array_dim_exprs[d]) {
                            compileExpression(node->array_dim_exprs[d], chunk, ctx);
                        }
                    }
                }
                writeBytecodeChunk(chunk, INIT_LOCAL_ARRAY, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)node->dim_count, node->token.line);
                for (int d = 0; d < node->dim_count; ++d) {
                    if (node->array_dims[d] == 0 && node->array_dim_exprs && node->array_dim_exprs[d]) {
                        emitShort(chunk, 0xFFFF, node->token.line);
                        emitShort(chunk, 0xFFFF, node->token.line);
                    } else {
                        Value lower = makeInt(0);
                        Value upper = makeInt(node->array_dims[d] - 1);
                        int lidx = addConstantToChunk(chunk, &lower);
                        int uidx = addConstantToChunk(chunk, &upper);
                        freeValue(&lower);
                        freeValue(&upper);
                        emitShort(chunk, (uint16_t)lidx, node->token.line);
                        emitShort(chunk, (uint16_t)uidx, node->token.line);
                    }
                }
                writeBytecodeChunk(chunk, (uint8_t)node->element_type, node->token.line);
                emitShort(chunk, (uint16_t)elemNameIdx, node->token.line);
                if (node->left && node->left->type == TCAST_STRING &&
                    node->element_type == TYPE_CHAR && node->dim_count == 1) {
                    char* str = tokenStringToCString(node->left->token);
                    size_t slen = strlen(str);
                    for (size_t i = 0; i <= slen; ++i) {
                        char ch = (i < slen) ? str[i] : '\0';
                        Value idxVal = makeInt((long long)i);
                        int idxConst = addConstantToChunk(chunk, &idxVal);
                        freeValue(&idxVal);
                        emitConstantOperand(chunk, idxConst, node->token.line);
                        writeBytecodeChunk(chunk, GET_LOCAL_ADDRESS, node->token.line);
                        writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
                        writeBytecodeChunk(chunk, GET_ELEMENT_ADDRESS, node->token.line);
                        writeBytecodeChunk(chunk, 1, node->token.line);
                        Value chVal = makeChar(ch);
                        int chConst = addConstantToChunk(chunk, &chVal);
                        freeValue(&chVal);
                        emitConstantOperand(chunk, chConst, node->token.line);
                        writeBytecodeChunk(chunk, SET_INDIRECT, node->token.line);
                    }
                    free(str);
                }
            } else if (node->var_type == TYPE_STRING) {
                writeBytecodeChunk(chunk, INIT_LOCAL_STRING, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
                writeBytecodeChunk(chunk, 0, node->token.line);
                if (node->left) {
                    compileExpression(node->left, chunk, ctx);
                    writeBytecodeChunk(chunk, SET_LOCAL, node->token.line);
                    writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
                }
            } else if (node->var_type == TYPE_FILE) {
                writeBytecodeChunk(chunk, INIT_LOCAL_FILE, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);

                VarType file_element_type = TYPE_VOID;
                const char* element_type_name = "";

                if (node->right && node->right->token.type != CLIKE_TOKEN_TEXT) {
                    if (node->element_type != TYPE_UNKNOWN && node->element_type != TYPE_VOID) {
                        file_element_type = node->element_type;
                        element_type_name = clikeTokenTypeToTypeName(node->right->token.type);
                        if (!element_type_name || element_type_name[0] == '\0') {
                            element_type_name = varTypeToString(file_element_type);
                        }
                    }
                }

                writeBytecodeChunk(chunk, (uint8_t)file_element_type, node->token.line);
                if (file_element_type != TYPE_VOID && element_type_name && element_type_name[0] != '\0') {
                    int type_name_index = addStringConstant(chunk, element_type_name);
                    emitShort(chunk, (uint16_t)type_name_index, node->token.line);
                } else {
                    emitShort(chunk, 0xFFFF, node->token.line);
                }
                if (node->left) {
                    compileExpression(node->left, chunk, ctx);
                    writeBytecodeChunk(chunk, SET_LOCAL, node->token.line);
                    writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
                }
            } else {
                if (node->left) {
                    compileExpression(node->left, chunk, ctx);
                } else {
                    Value init;
                    if (isRealType(node->var_type)) {
                        init = makeReal(0.0);
                        init.type = node->var_type;
                    } else if (node->var_type == TYPE_RECORD) {
                        init = makeValueForType(TYPE_RECORD, recordDef, NULL);
                    } else {
                        switch (node->var_type) {
                            case TYPE_STRING:
                                init = makeNil();
                                break;
                            case TYPE_FILE:
                                init = makeValueForType(TYPE_FILE, NULL, NULL);
                                break;
                            case TYPE_MEMORYSTREAM:
                                init = makeValueForType(TYPE_MEMORYSTREAM, NULL, NULL);
                                break;
                            default:
                                init = makeInt(0);
                                init.type = node->var_type;
                                if (isIntlikeType(init.type)) init.u_val = 0;
                                break;
                        }
                    }
                    int cidx = addConstantToChunk(chunk, &init);
                    freeValue(&init);
                    emitConstantOperand(chunk, cidx, node->token.line);
                }
                writeBytecodeChunk(chunk, SET_LOCAL, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
            }
            if (local) local->isActive = 1;
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

static void compileGlobalVar(ASTNodeClike *node, BytecodeChunk *chunk) {
    if (!node) return;
    char* name = tokenToCString(node->token);
    int name_idx = addStringConstant(chunk, name);
    registerGlobal(name, node->var_type, node->element_type, name_idx);
    if (name_idx < 256) {
        writeBytecodeChunk(chunk, DEFINE_GLOBAL, node->token.line);
        writeBytecodeChunk(chunk, (uint8_t)name_idx, node->token.line);
    } else {
        writeBytecodeChunk(chunk, DEFINE_GLOBAL16, node->token.line);
        emitShort(chunk, (uint16_t)name_idx, node->token.line);
    }
    writeBytecodeChunk(chunk, (uint8_t)node->var_type, node->token.line);
    if (node->var_type == TYPE_ARRAY && node->is_array) {
        /* Emit array dimension metadata similar to INIT_LOCAL_ARRAY */
        writeBytecodeChunk(chunk, (uint8_t)node->dim_count, node->token.line);
        for (int d = 0; d < node->dim_count; ++d) {
            if (node->array_dims[d] > 0) {
                Value lower = makeInt(0);
                Value upper = makeInt(node->array_dims[d] - 1);
                emitShort(chunk, (uint16_t)addConstantToChunk(chunk, &lower), node->token.line);
                emitShort(chunk, (uint16_t)addConstantToChunk(chunk, &upper), node->token.line);
                freeValue(&lower);
                freeValue(&upper);
            } else {
                /* Undefined dimension size; emit zero bounds */
                emitShort(chunk, 0, node->token.line);
                emitShort(chunk, 0, node->token.line);
            }
        }
        int elemNameIdx = addStringConstant(chunk, "");
        writeBytecodeChunk(chunk, (uint8_t)node->element_type, node->token.line);
        emitShort(chunk, (uint16_t)elemNameIdx, node->token.line);
    } else if (node->var_type == TYPE_FILE) {
        const char* type_name = "";
        char* owned_type_name = NULL;
        if (node->right) {
            const char* mapped = clikeTokenTypeToTypeName(node->right->token.type);
            if (mapped && mapped[0] != '\0') {
                type_name = mapped;
            } else if (node->right->token.length > 0 && node->right->token.lexeme) {
                owned_type_name = tokenToCString(node->right->token);
                type_name = owned_type_name;
            }
        }
        if (type_name[0] == '\0') {
            type_name = varTypeToString(node->var_type);
        }
        int type_name_index = addStringConstant(chunk, type_name);
        if (owned_type_name) {
            free(owned_type_name);
        }
        emitShort(chunk, (uint16_t)type_name_index, node->token.line);

        VarType file_element_type = TYPE_VOID;
        const char* element_type_name = "";

        if (node->right && node->right->token.type != CLIKE_TOKEN_TEXT) {
            if (node->element_type != TYPE_UNKNOWN && node->element_type != TYPE_VOID) {
                file_element_type = node->element_type;
                element_type_name = clikeTokenTypeToTypeName(node->right->token.type);
                if (!element_type_name || element_type_name[0] == '\0') {
                    element_type_name = varTypeToString(file_element_type);
                }
            }
        }

        writeBytecodeChunk(chunk, (uint8_t)file_element_type, node->token.line);
        if (file_element_type != TYPE_VOID && element_type_name && element_type_name[0] != '\0') {
            int element_type_index = addStringConstant(chunk, element_type_name);
            emitShort(chunk, (uint16_t)element_type_index, node->token.line);
        } else {
            emitShort(chunk, 0xFFFF, node->token.line);
        }
    } else {
        const char* type_name = varTypeToString(node->var_type);
        int type_idx = addStringConstant(chunk, type_name);
        emitShort(chunk, (uint16_t)type_idx, node->token.line);
        if (node->var_type == TYPE_STRING) {
            Value zero = makeInt(0);
            int len_idx = addConstantToChunk(chunk, &zero);
            freeValue(&zero);
            emitShort(chunk, (uint16_t)len_idx, node->token.line);
        }
    }
    if (node->left) {
        FuncContext dummy = {0};
        compileExpression(node->left, chunk, &dummy);
        if (name_idx < 256) {
            writeBytecodeChunk(chunk, SET_GLOBAL, node->token.line);
            writeBytecodeChunk(chunk, (uint8_t)name_idx, node->token.line);
            writeInlineCacheSlot(chunk, node->token.line);
        } else {
            writeBytecodeChunk(chunk, SET_GLOBAL16, node->token.line);
            emitShort(chunk, (uint16_t)name_idx, node->token.line);
            writeInlineCacheSlot(chunk, node->token.line);
        }
    }
    free(name);
}
static void compileExpression(ASTNodeClike *node, BytecodeChunk *chunk, FuncContext* ctx) {
    compileExpressionWithResult(node, chunk, ctx, true);
}
static void compileExpressionWithResult(ASTNodeClike *node, BytecodeChunk *chunk, FuncContext* ctx, bool resultUsed) {
    if (!node) return;
    switch (node->type) {
        case TCAST_NUMBER: {
            Value v;
            if (node->token.type == CLIKE_TOKEN_FLOAT_LITERAL) {
                v = makeReal(node->token.float_val);
            } else if (node->token.type == CLIKE_TOKEN_CHAR_LITERAL) {
                // Emit character literals distinctly
                unsigned char char_code = (unsigned char)node->token.int_val;
                v = makeChar(char_code);
            } else {
                // Default to 64-bit integer regardless of inferred var_type
                v = makeInt(node->token.int_val);
            }
            int idx = addConstantToChunk(chunk, &v);
            emitConstantOperand(chunk, idx, node->token.line);
            break;
        }
        case TCAST_SIZEOF: {
            Value v = makeInt(node->token.int_val);
            v.type = TYPE_INT64;
            int idx = addConstantToChunk(chunk, &v);
            emitConstantOperand(chunk, idx, node->token.line);
            break;
        }
        case TCAST_STRING: {
            if (shouldEmitStringAsCharPointer(node, ctx)) {
                emitCharPointerConstant(node, chunk);
            } else {
                char* s = tokenStringToCString(node->token);
                Value v = makeString(s);
                free(s);
                int idx = addConstantToChunk(chunk, &v);
                freeValue(&v);
                emitConstantOperand(chunk, idx, node->token.line);
            }
            break;
        }
        case TCAST_BINOP: {
            // Short-circuit semantics for logical AND/OR
            if (node->token.type == CLIKE_TOKEN_AND_AND) {
                // if (!left) goto pushFalse; result = !!right; goto end; pushFalse: result=false; end:
                compileExpression(node->left, chunk, ctx);
                writeBytecodeChunk(chunk, JUMP_IF_FALSE, node->token.line);
                int jFalse = chunk->count; emitShort(chunk, 0xFFFF, node->token.line);
                compileExpression(node->right, chunk, ctx);
                // coerce to boolean
                writeBytecodeChunk(chunk, TO_BOOL, node->token.line);
                writeBytecodeChunk(chunk, JUMP, node->token.line);
                int jEnd = chunk->count; emitShort(chunk, 0xFFFF, node->token.line);
                // false path target
                uint16_t offFalse = (uint16_t)(chunk->count - (jFalse + 2));
                patchShort(chunk, jFalse, offFalse);
                Value fv = makeBoolean(0);
                int cFalse = addConstantToChunk(chunk, &fv); freeValue(&fv);
                emitConstantOperand(chunk, cFalse, node->token.line);
                uint16_t offEnd = (uint16_t)(chunk->count - (jEnd + 2));
                patchShort(chunk, jEnd, offEnd);
                break;
            } else if (node->token.type == CLIKE_TOKEN_OR_OR) {
                // if (!left) eval right else pushTrue; result at end
                compileExpression(node->left, chunk, ctx);
                writeBytecodeChunk(chunk, JUMP_IF_FALSE, node->token.line);
                int jEvalRight = chunk->count; emitShort(chunk, 0xFFFF, node->token.line);
                // left was true: push true and jump end
                Value tv = makeBoolean(1);
                int cTrue = addConstantToChunk(chunk, &tv); freeValue(&tv);
                emitConstantOperand(chunk, cTrue, node->token.line);
                writeBytecodeChunk(chunk, JUMP, node->token.line);
                int jEnd2 = chunk->count; emitShort(chunk, 0xFFFF, node->token.line);
                // evalRight target
                uint16_t offEval = (uint16_t)(chunk->count - (jEvalRight + 2));
                patchShort(chunk, jEvalRight, offEval);
                compileExpression(node->right, chunk, ctx);
                // coerce to boolean
                writeBytecodeChunk(chunk, TO_BOOL, node->token.line);
                uint16_t offEnd2 = (uint16_t)(chunk->count - (jEnd2 + 2));
                patchShort(chunk, jEnd2, offEnd2);
                break;
            }

            // Default binary operators
            compileExpression(node->left, chunk, ctx);
            compileExpression(node->right, chunk, ctx);
            switch (node->token.type) {
                case CLIKE_TOKEN_PLUS: writeBytecodeChunk(chunk, ADD, node->token.line); break;
                case CLIKE_TOKEN_MINUS: writeBytecodeChunk(chunk, SUBTRACT, node->token.line); break;
                case CLIKE_TOKEN_STAR: writeBytecodeChunk(chunk, MULTIPLY, node->token.line); break;
                case CLIKE_TOKEN_SLASH:
                    if (isIntlikeType(node->var_type) &&
                        node->left && node->right &&
                        isIntlikeType(node->left->var_type) &&
                        isIntlikeType(node->right->var_type)) {
                        /*
                         * In C, dividing two integers performs integer division
                         * (truncating toward zero). The VM has a dedicated
                         * opcode for this behaviour (INT_DIV), whereas
                         * DIVIDE always produces a real result. Without this
                         * check, expressions like `w / 4` would yield a real
                         * value, which breaks APIs expecting integer arguments
                         * (e.g. drawrect in the graphics tests).
                         */
                        writeBytecodeChunk(chunk, INT_DIV, node->token.line);
                    } else {
                        writeBytecodeChunk(chunk, DIVIDE, node->token.line);
                    }
                    break;
                case CLIKE_TOKEN_PERCENT: writeBytecodeChunk(chunk, MOD, node->token.line); break;
                case CLIKE_TOKEN_GREATER: writeBytecodeChunk(chunk, GREATER, node->token.line); break;
                case CLIKE_TOKEN_GREATER_EQUAL: writeBytecodeChunk(chunk, GREATER_EQUAL, node->token.line); break;
                case CLIKE_TOKEN_LESS: writeBytecodeChunk(chunk, LESS, node->token.line); break;
                case CLIKE_TOKEN_LESS_EQUAL: writeBytecodeChunk(chunk, LESS_EQUAL, node->token.line); break;
                case CLIKE_TOKEN_EQUAL_EQUAL: writeBytecodeChunk(chunk, EQUAL, node->token.line); break;
                case CLIKE_TOKEN_BANG_EQUAL: writeBytecodeChunk(chunk, NOT_EQUAL, node->token.line); break;
                case CLIKE_TOKEN_AND_AND: writeBytecodeChunk(chunk, AND, node->token.line); break;
                case CLIKE_TOKEN_OR_OR: writeBytecodeChunk(chunk, OR, node->token.line); break;
                case CLIKE_TOKEN_BIT_AND: writeBytecodeChunk(chunk, AND, node->token.line); break;
                case CLIKE_TOKEN_BIT_OR: writeBytecodeChunk(chunk, OR, node->token.line); break;
                case CLIKE_TOKEN_BIT_XOR: writeBytecodeChunk(chunk, XOR, node->token.line); break;
                case CLIKE_TOKEN_SHL: writeBytecodeChunk(chunk, SHL, node->token.line); break;
                case CLIKE_TOKEN_SHR: writeBytecodeChunk(chunk, SHR, node->token.line); break;
                default: break;
            }
            break;
        }
        case TCAST_TERNARY: {
            compileExpression(node->left, chunk, ctx);
            writeBytecodeChunk(chunk, JUMP_IF_FALSE, node->token.line);
            int elseJump = chunk->count;
            emitShort(chunk, 0xFFFF, node->token.line);
            compileExpression(node->right, chunk, ctx);
            writeBytecodeChunk(chunk, JUMP, node->token.line);
            int endJump = chunk->count;
            emitShort(chunk, 0xFFFF, node->token.line);
            uint16_t elseOffset = (uint16_t)(chunk->count - (elseJump + 2));
            patchShort(chunk, elseJump, elseOffset);
            compileExpression(node->third, chunk, ctx);
            uint16_t endOffset = (uint16_t)(chunk->count - (endJump + 2));
            patchShort(chunk, endJump, endOffset);
            break;
        }
        case TCAST_UNOP:
            compileExpression(node->left, chunk, ctx);
            switch (node->token.type) {
                case CLIKE_TOKEN_MINUS:
                    writeBytecodeChunk(chunk, NEGATE, node->token.line); break;
                case CLIKE_TOKEN_BANG:
                    writeBytecodeChunk(chunk, NOT, node->token.line); break;
                case CLIKE_TOKEN_TILDE:
                    // If int-like, emulate bitwise NOT as (-x - 1); otherwise fall back to logical NOT
                    if (node->left && isIntlikeType(node->left->var_type)) {
                        writeBytecodeChunk(chunk, NEGATE, node->token.line);
                        Value one = makeInt(1);
                        int c1 = addConstantToChunk(chunk, &one); freeValue(&one);
                        emitConstantOperand(chunk, c1, node->token.line);
                        writeBytecodeChunk(chunk, SUBTRACT, node->token.line);
                    } else {
                        writeBytecodeChunk(chunk, NOT, node->token.line);
                    }
                    break;
                default:
                    break;
            }
            break;
        case TCAST_ADDR: {
            // Support &var (address-of variable) and &func (address-of routine)
            if (node->left && node->left->type == TCAST_IDENTIFIER) {
                char* name = tokenToCString(node->left->token);
                // If it's a known function, emit its bytecode address as an integer constant.
                Symbol* sym = procedure_table ? hashTableLookup(procedure_table, name) : NULL;
                sym = resolveSymbolAlias(sym);
                if (sym) {
                    Value addr; memset(&addr, 0, sizeof(Value));
                    addr.type = TYPE_INT32;
                    SET_INT_VALUE(&addr, (long long)sym->bytecode_address);
                    int cidx = addConstantToChunk(chunk, &addr);
                    freeValue(&addr);
                    emitConstantOperand(chunk, cidx, node->token.line);
                    free(name);
                    break;
                }
                free(name);
            }
            // Fallback: address of variable/field/element
            compileLValue(node->left, chunk, ctx);
            break;
        }
        case TCAST_DEREF:
            compileExpression(node->left, chunk, ctx);
            writeBytecodeChunk(chunk, GET_INDIRECT, node->token.line);
            break;
        case TCAST_ASSIGN: {
            if (node->left) {
                if (node->left->type == TCAST_IDENTIFIER) {
                    char* name = tokenToCString(node->left->token);
                    int idx = resolveLocal(ctx, name);
                    compileExpression(node->right, chunk, ctx);
                    if (resultUsed) {
                        writeBytecodeChunk(chunk, DUP, node->token.line);
                    }
                    if (idx >= 0) {
                        writeBytecodeChunk(chunk, SET_LOCAL, node->token.line);
                        writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
                    } else {
                        int nameIdx = getGlobalNameConstIndex(chunk, name);
                        if (nameIdx < 256) {
                            writeBytecodeChunk(chunk, SET_GLOBAL, node->token.line);
                            writeBytecodeChunk(chunk, (uint8_t)nameIdx, node->token.line);
                            writeInlineCacheSlot(chunk, node->token.line);
                        } else {
                            writeBytecodeChunk(chunk, SET_GLOBAL16, node->token.line);
                            emitShort(chunk, (uint16_t)nameIdx, node->token.line);
                            writeInlineCacheSlot(chunk, node->token.line);
                        }
                    }
                    free(name);
                } else if (node->left->type == TCAST_ARRAY_ACCESS) {
                    /*
                     * In C, an assignment expression evaluates to the value
                     * being assigned.  Our VM's SET_INDIRECT no longer
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
                    writeBytecodeChunk(chunk, DUP, node->token.line); // [..., value, value]
                    compileLValue(node->left, chunk, ctx);            // [..., value, value, ptr]
                    writeBytecodeChunk(chunk, SWAP, node->token.line); // [..., value, ptr, value]
                    writeBytecodeChunk(chunk, SET_INDIRECT, node->token.line); // [..., value]
                } else if (node->left->type == TCAST_DEREF) {
                    compileExpression(node->right, chunk, ctx);      // [..., value]
                    writeBytecodeChunk(chunk, DUP, node->token.line); // [..., value, value]
                    compileExpression(node->left->left, chunk, ctx); // [..., value, value, ptr]
                    writeBytecodeChunk(chunk, SWAP, node->token.line); // [..., value, ptr, value]
                    writeBytecodeChunk(chunk, SET_INDIRECT, node->token.line); // [..., value]
                } else if (node->left->type == TCAST_MEMBER) {
                    compileExpression(node->right, chunk, ctx);      // [..., value]
                    writeBytecodeChunk(chunk, DUP, node->token.line); // [..., value, value]
                    compileLValue(node->left, chunk, ctx);           // [..., value, value, ptr]
                    writeBytecodeChunk(chunk, SWAP, node->token.line); // [..., value, ptr, value]
                    writeBytecodeChunk(chunk, SET_INDIRECT, node->token.line); // [..., value]
                }
            }
            break;
        }
        case TCAST_IDENTIFIER: {
            char* name = tokenToCString(node->token);
            if (strcasecmp(name, "NULL") == 0) {
                Value v; memset(&v, 0, sizeof(Value));
                // Emit a NIL constant instead of a zeroed POINTER to preserve
                // the base type of pointer variables when assigning NULL.
                // Using TYPE_POINTER here would clear the base type and later
                // operations (e.g. dereferencing) would fail.
                v.type = TYPE_NIL;
                int cidx = addConstantToChunk(chunk, &v);
                emitConstantOperand(chunk, cidx, node->token.line);
                free(name);
                break;
            }
            int idx = resolveLocal(ctx, name);
            if (idx >= 0) {
                writeBytecodeChunk(chunk, GET_LOCAL, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
            } else {
                int nameIdx = getGlobalNameConstIndex(chunk, name);
                if (nameIdx < 256) {
                    writeBytecodeChunk(chunk, GET_GLOBAL, node->token.line);
                    writeBytecodeChunk(chunk, (uint8_t)nameIdx, node->token.line);
                    writeInlineCacheSlot(chunk, node->token.line);
                } else {
                    writeBytecodeChunk(chunk, GET_GLOBAL16, node->token.line);
                    emitShort(chunk, (uint16_t)nameIdx, node->token.line);
                    writeInlineCacheSlot(chunk, node->token.line);
                }
            }
            free(name);
            break;
        }
        case TCAST_ARRAY_ACCESS:
            for (int i = 0; i < node->child_count; ++i) {
                compileExpression(node->children[i], chunk, ctx);
            }
            if (node->left && node->left->type == TCAST_IDENTIFIER) {
                char* name = tokenToCString(node->left->token);
                int idx = resolveLocal(ctx, name);
                if (idx >= 0) {
                    writeBytecodeChunk(chunk, GET_LOCAL_ADDRESS, node->left->token.line);
                    writeBytecodeChunk(chunk, (uint8_t)idx, node->left->token.line);
                } else {
                    int nameIdx = getGlobalNameConstIndex(chunk, name);
                    if (nameIdx < 256) {
                        writeBytecodeChunk(chunk, GET_GLOBAL_ADDRESS, node->left->token.line);
                        writeBytecodeChunk(chunk, (uint8_t)nameIdx, node->left->token.line);
                    } else {
                        writeBytecodeChunk(chunk, GET_GLOBAL_ADDRESS16, node->left->token.line);
                        emitShort(chunk, (uint16_t)nameIdx, node->left->token.line);
                    }
                }
                free(name);
            } else {
                compileExpression(node->left, chunk, ctx);
            }
            writeBytecodeChunk(chunk, LOAD_ELEMENT_VALUE, node->token.line);
            writeBytecodeChunk(chunk, (uint8_t)node->child_count, node->token.line);
            break;
        case TCAST_MEMBER:
            compileExpression(node->left, chunk, ctx);
            if (node->right && node->right->type == TCAST_IDENTIFIER) {
                char *fname = tokenToCString(node->right->token);
                int idx = addStringConstant(chunk, fname);
                if (idx < 256) {
                    writeBytecodeChunk(chunk, LOAD_FIELD_VALUE_BY_NAME, node->token.line);
                    writeBytecodeChunk(chunk, (uint8_t)idx, node->token.line);
                } else {
                    writeBytecodeChunk(chunk, LOAD_FIELD_VALUE_BY_NAME16, node->token.line);
                    emitShort(chunk, (uint16_t)idx, node->token.line);
                }
                free(fname);
            }
            break;
        case TCAST_THREAD_SPAWN: {
            ASTNodeClike *call = node->left;
            if (call && call->type == TCAST_CALL) {
                char *name = tokenToCString(call->token);
                Symbol* sym = procedure_table ? hashTableLookup(procedure_table, name) : NULL;
                sym = resolveSymbolAlias(sym);
                if (sym) {
                    /*
                     * When a function with local variables is spawned directly,
                     * the new thread begins execution without a call frame,
                     * so its locals overlap the stack base.  Any POP used to
                     * discard the assignment result then wipes out the just-
                     * stored local, leaving it NIL (e.g. feeding NIL to
                     * built-ins like toupper).  Generate a wrapper that
                     * performs a CALL_USER_PROC so the callee runs with a
                     * proper stack frame and separate local slot storage.
                     */
                    writeBytecodeChunk(chunk, THREAD_CREATE, call->token.line);
                    int patch = chunk->count;
                    emitShort(chunk, 0xFFFF, call->token.line); // placeholder for wrapper addr

                    // Jump over inlined wrapper so main thread continues execution.
                    writeBytecodeChunk(chunk, JUMP, call->token.line);
                    int jumpPatch = chunk->count;
                    emitShort(chunk, 0xFFFF, call->token.line);

                    int wrapper_addr = chunk->count;
                    int nameIdx = addStringConstant(chunk, name);
                    writeBytecodeChunk(chunk, CALL_USER_PROC, call->token.line);
                    emitShort(chunk, (uint16_t)nameIdx, call->token.line);
                    writeBytecodeChunk(chunk, (uint8_t)sym->arity, call->token.line);
                    writeBytecodeChunk(chunk, RETURN, call->token.line);

                    // Patch addresses: spawn target and jump over wrapper.
                    patchShort(chunk, patch, (uint16_t)wrapper_addr);
                    patchShort(chunk, jumpPatch, (uint16_t)(chunk->count - (jumpPatch + 2)));
                }
                free(name);
            }
            break;
        }
        case TCAST_CALL: {
            char *name = tokenToCString(node->token);
            if (strcasecmp(name, "mutex") == 0) {
                free(name);
                if (node->child_count != 0) {
                    fprintf(stderr, "Compile error: mutex expects no arguments.\n");
                }
                writeBytecodeChunk(chunk, MUTEX_CREATE, node->token.line);
                break;
            }
            if (strcasecmp(name, "rcmutex") == 0) {
                free(name);
                if (node->child_count != 0) {
                    fprintf(stderr, "Compile error: rcmutex expects no arguments.\n");
                }
                writeBytecodeChunk(chunk, RCMUTEX_CREATE, node->token.line);
                break;
            }
            if (strcasecmp(name, "lock") == 0) {
                if (node->child_count != 1) {
                    fprintf(stderr, "Compile error: lock expects 1 argument.\n");
                } else {
                    compileExpression(node->children[0], chunk, ctx);
                }
                free(name);
                writeBytecodeChunk(chunk, MUTEX_LOCK, node->token.line);
                break;
            }
            if (strcasecmp(name, "unlock") == 0) {
                if (node->child_count != 1) {
                    fprintf(stderr, "Compile error: unlock expects 1 argument.\n");
                } else {
                    compileExpression(node->children[0], chunk, ctx);
                }
                free(name);
                writeBytecodeChunk(chunk, MUTEX_UNLOCK, node->token.line);
                break;
            }
            if (strcasecmp(name, "destroy") == 0) {
                if (node->child_count != 1) {
                    fprintf(stderr, "Compile error: destroy expects 1 argument.\n");

                } else {
                    compileExpression(node->children[0], chunk, ctx);
                }
                free(name);
                writeBytecodeChunk(chunk, MUTEX_DESTROY, node->token.line);
                break;
            }
            if (strcasecmp(name, "printf") == 0) {
                int arg_index = 0;
                int write_arg_count = 0;
                Value nl = makeInt(VM_WRITE_FLAG_SUPPRESS_SPACING);
                int nlidx = addConstantToChunk(chunk, &nl);
                freeValue(&nl);
                emitConstantOperand(chunk, nlidx, node->token.line);
                write_arg_count++;
                if (node->child_count > 0 && node->children[0]->type == TCAST_STRING) {
                    arg_index = 1;
                    char* fmt = tokenStringToCString(node->children[0]->token);
                    size_t flen = strlen(fmt);
                    char* seg = malloc(flen + 1);
                    size_t seglen = 0;
                    for (size_t i = 0; i < flen; ++i) {
                        if (fmt[i] == '%' && i + 1 < flen) {
                            if (fmt[i + 1] == '%') {
                                seg[seglen++] = '%';
                                i++; // skip second %
                            } else {
                                size_t j = i + 1;
                                int width = 0;
                                int precision = -1;
                                while (j < flen && isdigit((unsigned char)fmt[j])) {
                                    width = width * 10 + (fmt[j] - '0');
                                    j++;
                                }
                                if (j < flen && fmt[j] == '.') {
                                    j++;
                                    precision = 0;
                                    while (j < flen && isdigit((unsigned char)fmt[j])) {
                                        precision = precision * 10 + (fmt[j] - '0');
                                        j++;
                                    }
                                }
                                const char *length_mods = "hlLjzt";
                                while (j < flen && strchr(length_mods, fmt[j]) != NULL) {
                                    j++;
                                }
                                const char *specifiers = "cdiuoxXfFeEgGaAspn";
                                if (j < flen && strchr(specifiers, fmt[j]) != NULL && arg_index < node->child_count) {
                                    if (seglen > 0) {
                                        seg[seglen] = '\0';
                                        Value strv = makeString(seg);
                                        int cidx = addConstantToChunk(chunk, &strv);
                                        freeValue(&strv);
                                        emitConstantOperand(chunk, cidx, node->token.line);
                                        write_arg_count++;
                                        seglen = 0;
                                    }
                                    ASTNodeClike* argNode = node->children[arg_index];
                                    compileExpression(argNode, chunk, ctx);
                                    if (isNumericPrintfSpec(fmt[j]) && argNode &&
                                        (argNode->var_type == TYPE_BOOLEAN || argNode->var_type == TYPE_CHAR)) {
                                        emitBuiltinFunctionCall(chunk, "toint", 1, node->token.line);
                                    }
                                    arg_index++;
                                    if (width > 0 || precision >= 0) {
                                        if (precision < 0) precision = PASCAL_DEFAULT_FLOAT_PRECISION;
                                        writeBytecodeChunk(chunk, FORMAT_VALUE, node->token.line);
                                        writeBytecodeChunk(chunk, (uint8_t)width, node->token.line);
                                        writeBytecodeChunk(chunk, (uint8_t)precision, node->token.line);
                                    }
                                    write_arg_count++;
                                    i = j; // skip full format specifier
                                } else {
                                    seg[seglen++] = '%';
                                }
                            }
                        } else {
                            seg[seglen++] = fmt[i];
                        }
                    }
                    if (seglen > 0) {
                        seg[seglen] = '\0';
                        Value strv = makeString(seg);
                        int cidx = addConstantToChunk(chunk, &strv);
                        freeValue(&strv);
                        emitConstantOperand(chunk, cidx, node->token.line);
                        write_arg_count++;
                    }
                    free(seg);
                    free(fmt);
                }
                for (; arg_index < node->child_count; ++arg_index) {
                    compileExpression(node->children[arg_index], chunk, ctx);
                    write_arg_count++;
                }
                emitBuiltinProcedureCall(chunk, "write",
                                         (uint8_t)write_arg_count,
                                         node->token.line);
                Value zero = makeInt(0);
                int zidx = addConstantToChunk(chunk, &zero);
                freeValue(&zero);
                emitConstantOperand(chunk, zidx, node->token.line);
            } else if (strcasecmp(name, "scanf") == 0 || strcasecmp(name, "readln") == 0) {
                // Compile arguments as l-values (addresses) and call the VM's
                // `readln` builtin. `scanf` returns 0 to mimic C semantics,
                // while `readln` is treated as a procedure.
                for (int i = 0; i < node->child_count; ++i) {
                    compileLValue(node->children[i], chunk, ctx);
                }
                emitBuiltinProcedureCall(chunk, "readln",
                                         (uint8_t)node->child_count,
                                         node->token.line);

                if (strcasecmp(name, "scanf") == 0) {
                    Value zero = makeInt(0);
                    int idx = addConstantToChunk(chunk, &zero);
                    freeValue(&zero);
                    emitConstantOperand(chunk, idx, node->token.line);
                }
            } else if (strcasecmp(name, "assign") == 0 ||
                       strcasecmp(name, "reset") == 0 ||
                       strcasecmp(name, "rewrite") == 0 ||
                       strcasecmp(name, "append") == 0 ||
                       strcasecmp(name, "eof") == 0 ||
                       strcasecmp(name, "close") == 0 ||
                       strcasecmp(name, "rename") == 0 ||
                       strcasecmp(name, "remove") == 0) {
                // File builtins take the file variable as a VAR parameter.
                if (node->child_count > 0) {
                    compileLValue(node->children[0], chunk, ctx);
                    for (int i = 1; i < node->child_count; ++i) {
                        compileExpression(node->children[i], chunk, ctx);
                    }
                }
                const char* vmName = name;
                if (strcasecmp(name, "remove") == 0) vmName = "erase";
                BuiltinRoutineType builtinKind = getBuiltinType(vmName);
                if (builtinKind == BUILTIN_TYPE_NONE && strcasecmp(vmName, name) != 0) {
                    builtinKind = getBuiltinType(name);
                }
                if (builtinKind == BUILTIN_TYPE_PROCEDURE) {
                    emitBuiltinProcedureCall(chunk, vmName,
                                             (uint8_t)node->child_count,
                                             node->token.line);
                } else {
                    int fnIndex = addBuiltinNameConstant(chunk, vmName, vmName);
                    writeBytecodeChunk(chunk, CALL_BUILTIN, node->token.line);
                    emitShort(chunk, (uint16_t)fnIndex, node->token.line);
                    writeBytecodeChunk(chunk, (uint8_t)node->child_count,
                                       node->token.line);
                }
            } else if (strcasecmp(name, "random") == 0) {
                // Direct wrapper around the VM's random builtin.
                for (int i = 0; i < node->child_count; ++i) {
                    compileExpression(node->children[i], chunk, ctx);
                }
                int rIndex = addBuiltinNameConstant(chunk, "random", "random");
                writeBytecodeChunk(chunk, CALL_BUILTIN, node->token.line);
                emitShort(chunk, (uint16_t)rIndex, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)node->child_count, node->token.line);
            } else if (strcasecmp(name, "itoa") == 0) {
                // Wrap Pascal's Str builtin: first argument is a value,
                // second is a VAR string parameter.
                if (node->child_count == 2) {
                    compileExpression(node->children[0], chunk, ctx);
                    compileLValue(node->children[1], chunk, ctx);
                } else {
                    for (int i = 0; i < node->child_count; ++i) {
                        compileExpression(node->children[i], chunk, ctx);
                    }
                }
                emitBuiltinProcedureCall(chunk, "str",
                                         (uint8_t)node->child_count,
                                         node->token.line);
            } else if (strcasecmp(name, "strlen") == 0) {
                // Map C's strlen to the Pascal-style length builtin.
                for (int i = 0; i < node->child_count; ++i) {
                    compileExpression(node->children[i], chunk, ctx);
                }
                int lenIndex = addBuiltinNameConstant(chunk, "length", "length");
                writeBytecodeChunk(chunk, CALL_BUILTIN, node->token.line);
                emitShort(chunk, (uint16_t)lenIndex, node->token.line);
                writeBytecodeChunk(chunk, (uint8_t)node->child_count, node->token.line);
            } else if (strcasecmp(name, "exit") == 0) {
                // Map C-like exit to the VM's halt builtin to allow an optional exit code.
                for (int i = 0; i < node->child_count; ++i) {
                    compileExpression(node->children[i], chunk, ctx);
                }
                emitBuiltinProcedureCall(chunk, "halt",
                                         (uint8_t)node->child_count,
                                         node->token.line);
            } else {
                for (int i = 0; i < node->child_count; ++i) {
                    compileExpression(node->children[i], chunk, ctx);
                }
                Symbol* sym = procedure_table ? hashTableLookup(procedure_table, name) : NULL;
                sym = resolveSymbolAlias(sym);
                int nameIndex = addStringConstant(chunk, name);
                if (sym) {
                    writeBytecodeChunk(chunk, CALL_USER_PROC, node->token.line);
                    emitShort(chunk, (uint16_t)nameIndex, node->token.line);
                    writeBytecodeChunk(chunk, (uint8_t)node->child_count, node->token.line);
                } else {
                    // If a local variable with this name exists, treat it as an indirect call through a function pointer.
                    int localIdx = resolveLocal(ctx, name);
                    if (localIdx >= 0) {
                        // Push the callee pointer value and emit indirect call
                        ASTNodeClike idNode = {0};
                        idNode.type = TCAST_IDENTIFIER;
                        idNode.token = node->token; // reuse token for name
                        compileExpression(&idNode, chunk, ctx);
                        writeBytecodeChunk(chunk, CALL_INDIRECT, node->token.line);
                        writeBytecodeChunk(chunk, (uint8_t)node->child_count, node->token.line);
                    } else {
                        // Fallback to builtin call by name (existing behavior)
                        writeBytecodeChunk(chunk, CALL_BUILTIN, node->token.line);
                        emitShort(chunk, (uint16_t)nameIndex, node->token.line);
                        writeBytecodeChunk(chunk, (uint8_t)node->child_count, node->token.line);
                    }
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
            int paramIdx = addLocal(&ctx, name, p->var_type, 0, 0, NULL, p->element_type);
            if (paramIdx >= 0 && paramIdx < ctx.localCount) {
                ctx.locals[paramIdx].isActive = 1;
            }
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
    /*
     * Symbol lookups are case-insensitive.  The symbol table stores all
     * procedure names in lowercase, but the token text preserves the
     * original casing.  This meant that procedures with mixed-case names
     * (e.g. "computeRowsThread0") were inserted using their original case
     * and later lookups using the lowercase form failed, causing instructions
     * such as THREAD_CREATE to be omitted.
     *
     * Normalize the function name to lowercase before inserting it into the
     * procedure table so lookups succeed regardless of the original casing.
     */
    toLowerString(fname);
    Symbol* sym = hashTableLookup(procedure_table, fname);
    sym = resolveSymbolAlias(sym);
    if (!sym) {
        sym = malloc(sizeof(Symbol));
        memset(sym, 0, sizeof(Symbol));
        sym->name = strdup(fname);
        hashTableInsert(procedure_table, sym);
    }
    sym->bytecode_address = address;
    sym->arity = (uint8_t)ctx.paramCount;
    sym->type = func->var_type;
    sym->is_defined = true;

    compileStatement(func->right, chunk, &ctx);
    writeBytecodeChunk(chunk, RETURN, func->token.line);

    /* The total locals required are whichever is larger: the maximum locals
     * seen during compilation (minus parameters) or the number of declarations
     * discovered by the pre-pass. */
    int needed = ctx.maxLocalCount - ctx.paramCount;
    if (declaredLocals > needed) needed = declaredLocals;
    if (needed < 0) {
        needed = 0;
    }
    sym->locals_count = (uint16_t)needed;

    // Free any remaining local metadata (params are at [0..paramCount-1]).
    for (int i = 0; i < ctx.localCount; i++) {
        free(ctx.locals[i].name);
        free(ctx.locals[i].arrayDims);
    }
    free(fname);
}

static void predeclareFunctions(ASTNodeClike *program) {
    if (!program) return;
    for (int i = 0; i < program->child_count; ++i) {
        ASTNodeClike *decl = program->children[i];
        if (decl->type != TCAST_FUN_DECL) continue;
        char *name = tokenToCString(decl->token);
        toLowerString(name);
        if (!hashTableLookup(procedure_table, name)) {
            Symbol *sym = malloc(sizeof(Symbol));
            memset(sym, 0, sizeof(Symbol));
            sym->name = strdup(name);
            sym->arity = decl->left ? decl->left->child_count : 0;
            sym->type = decl->var_type;
            sym->is_defined = false;
            hashTableInsert(procedure_table, sym);
        }
        free(name);
    }
}

static void patchForwardCalls(BytecodeChunk *chunk) {
    if (!procedure_table || !chunk || !chunk->code) return;
    for (int offset = 0; offset < chunk->count; ) {
        uint8_t opcode = chunk->code[offset];
        if (opcode == CALL_USER_PROC) {
            if (offset + 3 >= chunk->count) break;
            uint16_t name_index = (uint16_t)((chunk->code[offset + 1] << 8) |
                                             chunk->code[offset + 2]);
            if (name_index < chunk->constants_count &&
                chunk->constants[name_index].type == TYPE_STRING) {
                const char *proc_name = AS_STRING(chunk->constants[name_index]);
                if (proc_name && *proc_name) {
                    char lookup[MAX_SYMBOL_LENGTH];
                    strncpy(lookup, proc_name, sizeof(lookup) - 1);
                    lookup[sizeof(lookup) - 1] = '\0';
                    toLowerString(lookup);
                    Symbol *sym = hashTableLookup(procedure_table, lookup);
                    sym = resolveSymbolAlias(sym);
                    if (!sym || !sym->is_defined) {
                        fprintf(stderr,
                                "Compiler Error: Procedure '%s' was called but never defined.\n",
                                proc_name);
                    }
                }
            }
            offset += 4;
        } else {
            offset += getInstructionLength(chunk, offset);
        }
    }
}

void clikeCompile(ASTNodeClike *program, BytecodeChunk *chunk) {
    initBytecodeChunk(chunk);
    if (!program) return;

    globalVarCount = 0;

    typedef struct {
        ASTNodeClike *prog;
        char *source;
        char *allocated_path;
    } LoadedModule;

    LoadedModule *modules = NULL;
    size_t moduleCapacity = (size_t)clike_import_count;
    if (moduleCapacity > 0) {
        modules = (LoadedModule*)calloc(moduleCapacity, sizeof(LoadedModule));
        if (!modules) {
            fprintf(stderr, "CLike codegen error: failed to allocate module cache.\n");
            EXIT_FAILURE_HANDLER();
            return;
        }
    }

    // Compile global variable declarations first so they are initialized
    // before main is invoked.
    for (int i = 0; i < program->child_count; ++i) {
        ASTNodeClike *decl = program->children[i];
        if (decl->type == TCAST_VAR_DECL) {
            compileGlobalVar(decl, chunk);
        }
    }

    // Load imported modules so their globals can be defined before main runs.
    for (int i = 0; i < clike_import_count; ++i) {
        if ((size_t)i >= moduleCapacity) {
            size_t newCapacity = (size_t)clike_import_count;
            LoadedModule *resized = (LoadedModule*)realloc(modules, newCapacity * sizeof(LoadedModule));
            if (!resized) {
                fprintf(stderr, "CLike codegen error: failed to expand module cache.\n");
                EXIT_FAILURE_HANDLER();
                return;
            }
            modules = resized;
            memset(modules + moduleCapacity, 0, (newCapacity - moduleCapacity) * sizeof(LoadedModule));
            moduleCapacity = newCapacity;
        }
        LoadedModule *mod = &modules[i];
        const char *orig_path = clike_imports[i];
        const char *path = orig_path;
        char *allocated_path = NULL;
        FILE *f = fopen(path, "rb");
        if (!f) {
            const char *lib_dir = getenv("CLIKE_LIB_DIR");
            if (lib_dir && *lib_dir) {
                size_t len = strlen(lib_dir) + 1 + strlen(orig_path) + 1;
                allocated_path = (char*)malloc(len);
                if (allocated_path) {
                    snprintf(allocated_path, len, "%s/%s", lib_dir, orig_path);
                    f = fopen(allocated_path, "rb");
                    if (f) path = allocated_path; else { free(allocated_path); allocated_path = NULL; }
                }
            }
        }
        if (!f) {
            const char *default_dir = PSCAL_CLIKE_LIB_DIR;
            size_t len = strlen(default_dir) + 1 + strlen(orig_path) + 1;
            allocated_path = (char*)malloc(len);
            if (allocated_path) {
                snprintf(allocated_path, len, "%s/%s", default_dir, orig_path);
                f = fopen(allocated_path, "rb");
                if (f) path = allocated_path; else { free(allocated_path); allocated_path = NULL; }
            }
        }
        if (!f) {
            fprintf(stderr, "Could not open import '%s'\n", orig_path);
            if (allocated_path) free(allocated_path);
            continue;
        }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        rewind(f);
        char *src = (char*)malloc(len + 1);
        if (!src) { fclose(f); if (allocated_path) free(allocated_path); continue; }
        size_t bytes_read = fread(src, 1, len, f);
        if (bytes_read != (size_t)len) {
            fprintf(stderr, "Error reading import '%s'\n", orig_path);
            free(src);
            fclose(f);
            if (allocated_path) free(allocated_path);
            continue;
        }
        src[len] = '\0';
        fclose(f);

        ParserClike p; initParserClike(&p, src);
        ASTNodeClike *modProg = parseProgramClike(&p);
        freeParserClike(&p);

        if (!verifyASTClikeLinks(modProg, NULL)) {
            fprintf(stderr, "AST verification failed for module '%s' after parsing.\n", path);
            freeASTClike(modProg);
            free(src);
            if (allocated_path) free(allocated_path);
            EXIT_FAILURE_HANDLER();
            return;
        }

        analyzeSemanticsClike(modProg, orig_path);

        if (!verifyASTClikeLinks(modProg, NULL)) {
            fprintf(stderr, "AST verification failed for module '%s' after semantic analysis.\n", path);
            freeASTClike(modProg);
            free(src);
            if (allocated_path) free(allocated_path);
            EXIT_FAILURE_HANDLER();
            return;
        }

        for (int j = 0; j < modProg->child_count; ++j) {
            ASTNodeClike *decl = modProg->children[j];
            if (decl->type == TCAST_VAR_DECL) {
                compileGlobalVar(decl, chunk);
            }
        }

        mod->prog = modProg;
        mod->source = src;
        mod->allocated_path = allocated_path;
    }

    // Predeclare all functions so forward references are recognized.
    predeclareFunctions(program);
    for (int i = 0; i < clike_import_count; ++i) {
        if (modules[i].prog) {
            predeclareFunctions(modules[i].prog);
        }
    }

    // Emit a call to main after globals have been defined.
    writeBytecodeChunk(chunk, CALL_USER_PROC, 0);
    int mainNameIdx = addStringConstant(chunk, "main");
    emitShort(chunk, (uint16_t)mainNameIdx, 0);
    int mainArityPatch = chunk->count;
    writeBytecodeChunk(chunk, 0, 0);
    writeBytecodeChunk(chunk, HALT, 0);

    bool mainDefined = false;
    uint8_t mainArity = 0;

    for (int i = 0; i < clike_import_count; ++i) {
        if (!modules[i].prog) continue;
        for (int j = 0; j < modules[i].prog->child_count; ++j) {
            ASTNodeClike *decl = modules[i].prog->children[j];
            if (decl->type == TCAST_FUN_DECL) {
                compileFunction(decl, chunk);
            }
        }
    }

    for (int i = 0; i < program->child_count; ++i) {
        ASTNodeClike *decl = program->children[i];
        if (decl->type != TCAST_FUN_DECL) continue;
        char *name = tokenToCString(decl->token);
        compileFunction(decl, chunk);
        if (strcmp(name, "main") == 0) {
            Symbol* sym = (Symbol*)hashTableLookup(procedure_table, name);
            sym = resolveSymbolAlias(sym);
            if (sym && sym->is_defined) {
                mainDefined = true;
                mainArity = sym->arity;
            }
        }
        free(name);
    }

    if (mainDefined) {
        chunk->code[mainArityPatch] = mainArity;
    }

    patchForwardCalls(chunk);

    for (int i = 0; i < clike_import_count; ++i) {
        if (modules && modules[i].prog) freeASTClike(modules[i].prog);
        if (modules && modules[i].source) free(modules[i].source);
        if (modules && modules[i].allocated_path) free(modules[i].allocated_path);
        free(clike_imports[i]);
    }
    if (modules) free(modules);
    free(clike_imports);
    clike_imports = NULL;
    clike_import_count = 0;
}

void clikeResetCodegenState(void) {
    for (int i = 0; i < globalVarCount; ++i) {
        free(globalVars[i].name);
        globalVars[i].name = NULL;
        globalVars[i].name_idx = 0;
        globalVars[i].type = TYPE_UNKNOWN;
        globalVars[i].elemType = TYPE_UNKNOWN;
    }
    globalVarCount = 0;
}
