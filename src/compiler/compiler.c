// src/compiler/compiler.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // For strcmp, strdup, atoll

#include "compiler/compiler.h"
#include "compiler/bytecode.h"
#include "core/utils.h"
#include "core/types.h"
#include "frontend/ast.h"
#include "symbol/symbol.h" // For access to the main global symbol table, if needed,
                           // though for bytecode compilation, we often build our own tables/mappings.

#define MAX_GLOBALS 256 // Define a reasonable limit for global variables for now

// --- Forward Declarations for Recursive Compilation ---
static void compileNode(AST* node, BytecodeChunk* chunk, int current_line_approx);
static void compileExpression(AST* node, BytecodeChunk* chunk, int current_line_approx);
static void compileStatement(AST* node, BytecodeChunk* chunk, int current_line_approx);

// --- Global/Module State for Compiler ---
// For mapping global variable names to an index during this compilation pass.
// This is a simplified approach for global variables.
// A more robust compiler might use a symbol table specific to compilation.
typedef struct {
    char* name;
    // Other info if needed, like type, scope depth, etc.
} CompilerGlobalVarInfo;

CompilerGlobalVarInfo compilerGlobals[MAX_GLOBALS]; // MAX_GLOBALS from an appropriate header or defined here
int compilerGlobalCount = 0;

// Helper to get the source line number from an AST node's token
// Provides a fallback if token is NULL.
static int getLine(AST* node) {
    if (node && node->token && node->token->line > 0) {
        return node->token->line;
    }
    // Fallback or try to get from a child if node itself has no token (e.g. some implicit nodes)
    if (node && node->left && node->left->token && node->left->token->line > 0) {
        return node->left->token->line;
    }
    return 0; // Default if no line info found
}


// Finds or adds a global variable name and returns its index.
// For bytecode, this index is what OP_GET_GLOBAL/OP_SET_GLOBAL will use.
static int resolveGlobalVariableIndex(BytecodeChunk* chunk, const char* name, int line) {
    for (int i = 0; i < compilerGlobalCount; i++) {
        if (strcmp(compilerGlobals[i].name, name) == 0) {
            return i;
        }
    }
    if (compilerGlobalCount < MAX_GLOBALS) {
        compilerGlobals[compilerGlobalCount].name = strdup(name);
        if (!compilerGlobals[compilerGlobalCount].name) {
            fprintf(stderr, "L%d: Compiler error: Memory allocation failed for global variable name '%s'.\n", line, name);
            exit(1);
        }
        // The OP_DEFINE_GLOBAL opcode will use the *name string* from the constant pool.
        // The VM, upon seeing OP_DEFINE_GLOBAL, will internally map this name to its own storage slot.
        // For OP_GET/SET_GLOBAL, we also need the name.
        // So, ensure the name is in the constant pool.
        addConstantToChunk(chunk, makeString(name)); // Store name in constant pool for OP_DEFINE_GLOBAL
        return compilerGlobalCount++;
    }
    fprintf(stderr, "L%d: Compiler error: Too many global variables.\n", line);
    exit(1);
    return -1;
}

// --- Main Compilation Function ---
bool compileASTToBytecode(AST* rootNode, BytecodeChunk* outputChunk) {
    if (!rootNode || !outputChunk) {
        fprintf(stderr, "Compiler error: Null rootNode or outputChunk passed to compileASTToBytecode.\n");
        return false;
    }
    
    initBytecodeChunk(outputChunk); // Ensure chunk is initialized
    compilerGlobalCount = 0;      // Reset global variable tracking for this compilation

    if (rootNode->type == AST_PROGRAM && rootNode->right && rootNode->right->type == AST_BLOCK) {
        AST* mainBlock = rootNode->right;
        compileNode(mainBlock, outputChunk, getLine(rootNode)); // Pass initial line from PROGRAM token
    } else if (rootNode->type == AST_BLOCK) { // Allow compiling just a block (e.g. for procedures later)
        compileNode(rootNode, outputChunk, getLine(rootNode));
    }
    else {
        fprintf(stderr, "Compiler error: Expected AST_PROGRAM or AST_BLOCK node as root for compilation.\n");
        return false;
    }

    writeBytecodeChunk(outputChunk, OP_HALT, getLine(rootNode)); // End program
    return true;
}

// --- Recursive Compilation Dispatcher ---
static void compileNode(AST* node, BytecodeChunk* chunk, int current_line_approx) {
    if (!node) return;

    // Update current line if node has more specific info
    int line = node->token ? node->token->line : current_line_approx;
    if (line <= 0 && node->left && node->left->token) line = node->left->token->line; // Try left child
    if (line <= 0) line = current_line_approx; // Fallback

    switch (node->type) {
        case AST_BLOCK:
            // Compile declarations first
            if (node->child_count > 0 && node->children[0] && node->children[0]->type == AST_COMPOUND) {
                AST* declarations = node->children[0];
                for (int i = 0; i < declarations->child_count; i++) {
                    compileNode(declarations->children[i], chunk, line);
                }
            }
            // Then compile the body (statements)
            if (node->child_count > 1 && node->children[1]) {
                compileNode(node->children[1], chunk, line);
            }
            break;

        case AST_VAR_DECL: {
            // node->var_type should hold the type of the variables being declared (e.g., TYPE_INTEGER)
            // node->right usually points to the type specifier AST node (e.g., AST_VARIABLE "integer")
            // The actual VarType should be on the AST_VAR_DECL node itself after annotateTypes.
            VarType declared_type = node->var_type;

            if (node->children) { // `children` holds the AST_VARIABLE nodes for the names (a, b, result)
                for (int i = 0; i < node->child_count; i++) {
                    AST* varNameNode = node->children[i];
                    if (varNameNode && varNameNode->token) {
                        int nameIndex = addConstantToChunk(chunk, makeString(varNameNode->token->value));
                        
                        writeBytecodeChunk(chunk, OP_DEFINE_GLOBAL, getLine(varNameNode));
                        writeBytecodeChunk(chunk, (uint8_t)nameIndex, getLine(varNameNode)); // Operand 1: name string index
                        writeBytecodeChunk(chunk, (uint8_t)declared_type, getLine(varNameNode)); // Operand 2: VarType enum value
                        
                        // The compiler's internal resolveGlobalVariableIndex ensures it knows about the global.
                        // The VM will use the name and type to set up its storage.
                        resolveGlobalVariableIndex(chunk, varNameNode->token->value, getLine(varNameNode));
                    }
                }
            }
            break;
        }
        case AST_COMPOUND:
            for (int i = 0; i < node->child_count; i++) {
                compileStatement(node->children[i], chunk, line);
            }
            break;

        case AST_ASSIGN:
        case AST_WRITELN: // WriteLn is a statement
            compileStatement(node, chunk, line);
            break;
        
        // Expressions
        case AST_NUMBER:
        case AST_STRING:
        case AST_VARIABLE:
        case AST_BINARY_OP:
        case AST_UNARY_OP: // Assuming unary ops are expressions
            compileExpression(node, chunk, line);
            break;
            
        default:
            fprintf(stderr, "L%d: Compiler warning: Unhandled AST node type %s during compilation.\n", line, astTypeToString(node->type));
            break;
    }
}

static void compileStatement(AST* node, BytecodeChunk* chunk, int current_line_approx) {
    if (!node) return;
    int line = getLine(node);
    if (line <= 0) line = current_line_approx;

    switch (node->type) {
        case AST_ASSIGN:
            // Compile the expression on the right-hand side (leaves value on stack)
            compileExpression(node->right, chunk, line);
            
            if (node->left && node->left->type == AST_VARIABLE && node->left->token) {
                const char* varName = node->left->token->value;
                // For SET_GLOBAL, the operand will be the index of the variable name string in the constant pool.
                // The VM will use this to find the actual global variable slot.
                int nameIndex = addConstantToChunk(chunk, makeString(varName));
                writeBytecodeChunk(chunk, OP_SET_GLOBAL, line);
                writeBytecodeChunk(chunk, (uint8_t)nameIndex, line);
            } else {
                fprintf(stderr, "L%d: Compiler error: LHS of assignment is not a simple global variable ('%s').\n", line, node->left && node->left->token ? node->left->token->value : "unknown");
            }
            break;

        case AST_WRITELN: {
            int argCount = node->child_count;
            for (int i = 0; i < argCount; i++) {
                compileExpression(node->children[i], chunk, getLine(node->children[i]));
            }
            writeBytecodeChunk(chunk, OP_WRITE_LN, line);
            writeBytecodeChunk(chunk, (uint8_t)argCount, line);
            break;
        }
        
        case AST_PROCEDURE_CALL: // e.g. a function call whose result is ignored
            compileExpression(node, chunk, line); // Compile the call
            if (node->var_type != TYPE_VOID) { // If it was a function, pop its result
                writeBytecodeChunk(chunk, OP_POP, line);
            }
            break;
        case AST_IF: {
            // Structure: node->left is condition, node->right is then_branch, node->extra is else_branch (optional)
            if (!node->left || !node->right) {
                fprintf(stderr, "L%d: Compiler error: IF node is missing condition or then-branch.\n", line);
                return; // Or handle error more gracefully
            }

            // 1. Compile the condition
            compileExpression(node->left, chunk, line); // Condition result will be on stack

            // 2. Emit OP_JUMP_IF_FALSE with a placeholder for the offset
            //    The offset will point to the instruction *after* the THEN branch
            //    (or to the start of the ELSE branch).
            int thenBranchJumpPatchOffset = chunk->count; // Position where OP_JUMP_IF_FALSE will be
            writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line);
            emitShort(chunk, 0xFFFF, line); // Placeholder for 16-bit jump offset

            // 3. Compile the THEN branch
            compileStatement(node->right, chunk, line); // Then branch

            if (node->extra) { // If there's an ELSE branch
                // 4a. Emit OP_JUMP to skip the ELSE branch after THEN branch executes
                int elseBranchJumpOverPatchOffset = chunk->count;
                writeBytecodeChunk(chunk, OP_JUMP, line);
                emitShort(chunk, 0xFFFF, line); // Placeholder to jump past ELSE

                // 4b. Backpatch OP_JUMP_IF_FALSE:
                //      It should jump to the current position (start of ELSE branch)
                //      The offset is from the instruction *after* the OP_JUMP_IF_FALSE's operand
                //      to the current end of the chunk.
                uint16_t offsetToElse = chunk->count - (thenBranchJumpPatchOffset + 1 + 2); // +1 for opcode, +2 for its short operand
                patchShort(chunk, thenBranchJumpPatchOffset + 1, offsetToElse); // Patch the operand of OP_JUMP_IF_FALSE

                // 4c. Compile the ELSE branch
                compileStatement(node->extra, chunk, line);

                // 4d. Backpatch OP_JUMP:
                //      It should jump to the current position (after the ELSE branch)
                uint16_t offsetToEndOfIf = chunk->count - (elseBranchJumpOverPatchOffset + 1 + 2);
                patchShort(chunk, elseBranchJumpOverPatchOffset + 1, offsetToEndOfIf);

            } else { // No ELSE branch
                // 4e. Backpatch OP_JUMP_IF_FALSE directly to the end of the THEN branch
                uint16_t offsetToEndOfThen = chunk->count - (thenBranchJumpPatchOffset + 1 + 2);
                patchShort(chunk, thenBranchJumpPatchOffset + 1, offsetToEndOfThen);
            }
            break;
        }
            
        case AST_COMPOUND: { // <<< ADD THIS CASE
            for (int i = 0; i < node->child_count; i++) {
                if (node->children[i]) {
                    // Recursively call compileStatement for each statement in the block
                    // Or compileNode if that's your main dispatcher that then calls compileStatement/Expression
                    compileStatement(node->children[i], chunk, getLine(node->children[i]));
                }
            }
            break;
        }

        default:
            fprintf(stderr, "L%d: Compiler warning: Unhandled AST node type %s in compileStatement.\n", line, astTypeToString(node->type));
            break;
    }
}

static void compileExpression(AST* node, BytecodeChunk* chunk, int current_line_approx) {
    if (!node) return;
    int line = getLine(node);
    if (line <= 0) line = current_line_approx;

    switch (node->type) {
        case AST_NUMBER: {
            Value numVal;
            if (node->token->type == TOKEN_REAL_CONST) {
                numVal = makeReal(atof(node->token->value));
            } else { // INTEGER_CONST or HEX_CONST
                numVal = makeInt(atoll(node->token->value));
            }
            int constIndex = addConstantToChunk(chunk, numVal);
            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, (uint8_t)constIndex, line);
            break;
        }
        case AST_STRING: {
            // Important: makeString strdups, addConstantToChunk copies the Value struct.
            // freeValue in freeBytecodeChunk will free the strdup'd string.
            Value strVal = makeString(node->token->value);
            int constIndex = addConstantToChunk(chunk, strVal);
            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, (uint8_t)constIndex, line);
            break;
        }
        case AST_VARIABLE: {
            const char* varName = node->token->value;
            // For GET_GLOBAL, the operand will be the index of the variable name string in the constant pool.
            int nameIndex = addConstantToChunk(chunk, makeString(varName));
            writeBytecodeChunk(chunk, OP_GET_GLOBAL, line);
            writeBytecodeChunk(chunk, (uint8_t)nameIndex, line);
            break;
        }
        case AST_BINARY_OP:
            compileExpression(node->left, chunk, getLine(node->left));
            compileExpression(node->right, chunk, getLine(node->right));

            if (node->token) {
                switch (node->token->type) {
                    case TOKEN_PLUS:    writeBytecodeChunk(chunk, OP_ADD, line); break;
                    case TOKEN_MINUS:   writeBytecodeChunk(chunk, OP_SUBTRACT, line); break;
                    case TOKEN_MUL:     writeBytecodeChunk(chunk, OP_MULTIPLY, line); break;
                    case TOKEN_SLASH:   writeBytecodeChunk(chunk, OP_DIVIDE, line); break;
                    case TOKEN_EQUAL:         writeBytecodeChunk(chunk, OP_EQUAL, line); break;
                    case TOKEN_NOT_EQUAL:     writeBytecodeChunk(chunk, OP_NOT_EQUAL, line); break; // Or OP_EQUAL then OP_NOT
                    case TOKEN_LESS:          writeBytecodeChunk(chunk, OP_LESS, line); break;
                    case TOKEN_LESS_EQUAL:    writeBytecodeChunk(chunk, OP_LESS_EQUAL, line); break;
                    case TOKEN_GREATER:       writeBytecodeChunk(chunk, OP_GREATER, line); break;
                    case TOKEN_GREATER_EQUAL: writeBytecodeChunk(chunk, OP_GREATER_EQUAL, line); break;
                    // TODO: Add other binary ops (DIV, MOD, comparisons)
                    default:
                        fprintf(stderr, "L%d: Compiler error: Unknown binary operator %s\n", line, tokenTypeToString(node->token->type));
                        break;
                }
            }
            break;
        case AST_UNARY_OP:
            compileExpression(node->left, chunk, getLine(node->left));
            if (node->token) {
                switch (node->token->type) {
                    case TOKEN_MINUS: writeBytecodeChunk(chunk, OP_NEGATE, line); break;
                    case TOKEN_NOT:   writeBytecodeChunk(chunk, OP_NOT, line);    break;
                    default:
                        fprintf(stderr, "L%d: Compiler error: Unknown unary operator %s\n", line, tokenTypeToString(node->token->type));
                        break;
                }
            }
            break;
        case AST_BOOLEAN: {
             #ifdef DEBUG
             if(dumpExec) fprintf(stderr, "COMPILER: AST_BOOLEAN, token value '%s', node->i_val = %d, node->var_type = %s\n",
                    node->token ? node->token->value : "NULL", node->i_val, varTypeToString(node->var_type));
             #endif
             Value boolConst = makeBoolean(node->i_val);
             int constIndex = addConstantToChunk(chunk, boolConst);
             writeBytecodeChunk(chunk, OP_CONSTANT, line);
             writeBytecodeChunk(chunk, (uint8_t)constIndex, line);
             break;
        }
        case AST_PROCEDURE_CALL: // Function calls used in expressions
            // For SimpleMath, this case isn't hit for `upcase` because `WriteLn` takes expressions.
            // But if you had `x := upcase('a');`, this would be used.
            // This is a placeholder for now as SimpleMath doesn't use function calls in expressions
            // directly other than as arguments to WriteLn.
            fprintf(stderr, "L%d: Compiler: Expression function call for '%s' needs OP_CALL_BUILTIN/OP_CALL logic.\n", line, node->token ? node->token->value : "unknown");
            // For now, push a dummy value to satisfy expression context.
            {
                int dummyIndex = addConstantToChunk(chunk, makeInt(0)); // Or type from node->var_type
                writeBytecodeChunk(chunk, OP_CONSTANT, line);
                writeBytecodeChunk(chunk, (uint8_t)dummyIndex, line);
            }
            break;

        default:
            fprintf(stderr, "L%d: Compiler warning: Unhandled AST node type %s in compileExpression.\n", line, astTypeToString(node->type));
            {
                int dummyIndex = addConstantToChunk(chunk, makeInt(0)); // Push a dummy 0
                writeBytecodeChunk(chunk, OP_CONSTANT, line);
                writeBytecodeChunk(chunk, (uint8_t)dummyIndex, line);
            }
            break;
    }
}
