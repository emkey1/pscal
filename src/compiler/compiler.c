//
//  compiler.c
//  Pscal
//
//  Created by Michael Miller on 5/18/25.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h> // For atoi, atof (careful with error checking)

#include "compiler.h"
#include "bytecode.h"
#include "core/types.h"   // For Value, VarType, astTypeToString etc.
#include "frontend/ast.h"   // For AST, ASTNodeType
#include "symbol/symbol.h"  // For global symbol table interaction (e.g., to get variable indices)
                           // We'll need a way to map global var names to indices for OP_GET/SET_GLOBAL

// --- Forward Declarations for Recursive Compilation ---
static void compileNode(AST* node, BytecodeChunk* chunk);
static void compileExpression(AST* node, BytecodeChunk* chunk);
static void compileStatement(AST* node, BytecodeChunk* chunk);
// ... other helpers as needed ...

// --- Global/Module State for Compiler (if any) ---
// For now, we'll pass the BytecodeChunk around.
// We might need access to the global symbol table to resolve variable names to indices.
// For simplicity, let's assume global variables will be assigned indices 0, 1, 2...
// A more robust system would involve a mapping from name to index during compilation.

// A simple way to manage global variable indices for this initial version
// This is a placeholder. A proper compiler would build this map from VAR declarations.
#define MAX_GLOBALS 100
const char* globalVariableNames[MAX_GLOBALS];
int globalVariableCount = 0;

int resolveOrAddGlobal(const char* name) {
    for (int i = 0; i < globalVariableCount; i++) {
        if (strcmp(globalVariableNames[i], name) == 0) {
            return i; // Found existing global
        }
    }
    if (globalVariableCount < MAX_GLOBALS) {
        // For this simple version, strdup is okay. A real compiler might intern strings.
        globalVariableNames[globalVariableCount] = strdup(name);
        if (!globalVariableNames[globalVariableCount]) {
            fprintf(stderr, "Compiler error: Failed to strdup global variable name.\n");
            exit(1); // Or handle error
        }
        return globalVariableCount++;
    }
    fprintf(stderr, "Compiler error: Too many global variables.\n");
    exit(1); // Or handle error
    return -1; // Should not reach
}


// --- Main Compilation Function ---
bool compileASTToBytecode(AST* rootNode, BytecodeChunk* outputChunk) {
    if (!rootNode || !outputChunk) {
        return false;
    }
    
    // Initialize the output chunk (caller should ideally do this, but good to be safe)
    // initBytecodeChunk(outputChunk); // Assuming chunk is already inited by caller

    globalVariableCount = 0; // Reset for each compilation run (simple approach)

    // Start compilation from the root node
    // For a PROGRAM node, we'd typically compile its main block.
    if (rootNode->type == AST_PROGRAM && rootNode->right && rootNode->right->type == AST_BLOCK) {
        AST* mainBlock = rootNode->right;
        compileNode(mainBlock, outputChunk);
    } else {
        // Handle other root types or error if not a program
        fprintf(stderr, "Compiler error: Expected AST_PROGRAM node as root.\n");
        return false;
    }

    // End the main program's bytecode with a HALT or RETURN
    writeBytecodeChunk(outputChunk, OP_HALT, rootNode->token ? rootNode->token->line : 0); // Approximate line
    return true;
}

// --- Recursive Compilation Dispatcher ---
static void compileNode(AST* node, BytecodeChunk* chunk) {
    if (!node) return;

    // Store current source line for this node (approximate for now)
    // A proper implementation would track line numbers more accurately through the AST
    int line = node->token ? node->token->line : 0; // Placeholder for line number

    switch (node->type) {
        case AST_BLOCK:
            // Compile declarations first (to register globals)
            if (node->child_count > 0 && node->children[0] && node->children[0]->type == AST_COMPOUND) {
                AST* declarations = node->children[0];
                for (int i = 0; i < declarations->child_count; i++) {
                    compileNode(declarations->children[i], chunk);
                }
            }
            // Then compile the body (statements)
            if (node->child_count > 1 && node->children[1]) {
                compileNode(node->children[1], chunk);
            }
            break;

        case AST_VAR_DECL:
            // For each variable in the declaration (a, b, result: Integer)
            // For globals, we "define" them by adding their name to our mapping
            // The actual memory allocation will happen in the VM when it sees OP_DEFINE_GLOBAL
            // or simply by having a fixed-size global array.
            // For this simple example, we'll assume OP_DEFINE_GLOBAL takes a constant index
            // which refers to the variable's name string.
            if (node->children) { // `children` holds the AST_VARIABLE nodes for names
                for (int i = 0; i < node->child_count; i++) {
                    AST* varNameNode = node->children[i];
                    if (varNameNode && varNameNode->token) {
                        // For global variables, add name to constant pool and emit OP_DEFINE_GLOBAL
                        int nameConstIndex = addConstantToChunk(chunk, makeString(varNameNode->token->value));
                        writeBytecodeChunk(chunk, OP_DEFINE_GLOBAL, line);
                        writeBytecodeChunk(chunk, (uint8_t)nameConstIndex, line); // Operand: index of name string
                        // For our simple global model:
                        resolveOrAddGlobal(varNameNode->token->value);
                    }
                }
            }
            break;

        case AST_COMPOUND: // A sequence of statements
            for (int i = 0; i < node->child_count; i++) {
                compileStatement(node->children[i], chunk);
            }
            break;

        // We need compileStatement and compileExpression helpers
        case AST_ASSIGN:
        case AST_WRITELN:
            compileStatement(node, chunk);
            break;
        
        case AST_NUMBER:
        case AST_STRING: // String literals will also go into constant pool
        case AST_VARIABLE:
        case AST_BINARY_OP:
            compileExpression(node, chunk); // Expressions leave their value on the stack
            break;
            
        // Add more cases as needed...
        default:
            fprintf(stderr, "Compiler warning: Unhandled AST node type %s during compilation.\n", astTypeToString(node->type));
            break;
    }
}


// --- Compile Statements ---
static void compileStatement(AST* node, BytecodeChunk* chunk) {
    if (!node) return;
    int line = node->token ? node->token->line : (node->left ? (node->left->token ? node->left->token->line : 0) : 0);

    switch (node->type) {
        case AST_ASSIGN:
            // Compile the expression on the right-hand side (leaves value on stack)
            compileExpression(node->right, chunk);
            // Get the name of the variable on the left-hand side
            if (node->left && node->left->type == AST_VARIABLE && node->left->token) {
                const char* varName = node->left->token->value;
                int globalIndex = resolveOrAddGlobal(varName); // Use our simple global mapping

                writeBytecodeChunk(chunk, OP_SET_GLOBAL, line);
                writeBytecodeChunk(chunk, (uint8_t)globalIndex, line); // Operand: index of global var
            } else {
                fprintf(stderr, "Compiler error: LHS of assignment is not a simple global variable.\n");
                // Handle error: In a real compiler, this might be a semantic error.
            }
            break;

        case AST_WRITELN: {
            // Arguments are in node->children, compile them in reverse order for stack
            // Or, compile them in order and have VM expect them in order.
            // Let's compile in order, and OP_WRITE_LN will know how many to pop.
            int argCount = node->child_count;
            for (int i = 0; i < argCount; i++) {
                compileExpression(node->children[i], chunk); // Each arg value pushed to stack
            }
            writeBytecodeChunk(chunk, OP_WRITE_LN, line);
            writeBytecodeChunk(chunk, (uint8_t)argCount, line); // Operand: number of arguments
            break;
        }
        
        // If an expression is used as a statement (e.g. a function call whose result is ignored)
        // We compile the expression, then pop its result from the stack.
        case AST_PROCEDURE_CALL: // Assuming function calls are parsed as this
            compileExpression(node, chunk); // Compile the call, its result is on stack
            if (node->var_type != TYPE_VOID) { // If it's a function call (not procedure)
                writeBytecodeChunk(chunk, OP_POP, line); // Pop the unused result
            }
            break;

        // other statement types (IF, WHILE, etc.) will be added later
        default:
            // Might be an expression used as a statement; if so, its value should be popped
            // For now, only handle specific statements or log unknown.
            fprintf(stderr, "Compiler warning: Unhandled AST node type %s in compileStatement.\n", astTypeToString(node->type));
            break;
    }
}


// --- Compile Expressions ---
// Expressions evaluate and leave their result on top of the stack.
static void compileExpression(AST* node, BytecodeChunk* chunk) {
    if (!node) return;
    int line = node->token ? node->token->line : 0; // Approximate line

    switch (node->type) {
        case AST_NUMBER: {
            // Add the number as a constant, then emit OP_CONSTANT
            // For simplicity, assuming all numbers are integers for now
            long long val = atoll(node->token->value); // Use atoll for long long
            int constIndex = addConstantToChunk(chunk, makeInt(val));
            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, (uint8_t)constIndex, line); // Operand: index in constant pool
            break;
        }
        case AST_STRING: { // String literals
            int constIndex = addConstantToChunk(chunk, makeString(node->token->value));
            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, (uint8_t)constIndex, line);
            break;
        }
        case AST_VARIABLE: {
            const char* varName = node->token->value;
            int globalIndex = resolveOrAddGlobal(varName); // Use our simple global mapping

            writeBytecodeChunk(chunk, OP_GET_GLOBAL, line);
            writeBytecodeChunk(chunk, (uint8_t)globalIndex, line); // Operand: index of global var
            break;
        }
        case AST_BINARY_OP:
            // Compile left operand (result on stack)
            compileExpression(node->left, chunk);
            // Compile right operand (result on stack)
            compileExpression(node->right, chunk);

            // Emit operator
            // This assumes type checking/conversion for ops like Real division happens in VM
            // or that specific typed opcodes exist. For now, generic ops.
            if (node->token) {
                switch (node->token->type) {
                    case TOKEN_PLUS:    writeBytecodeChunk(chunk, OP_ADD, line); break;
                    case TOKEN_MINUS:   writeBytecodeChunk(chunk, OP_SUBTRACT, line); break;
                    case TOKEN_MUL:     writeBytecodeChunk(chunk, OP_MULTIPLY, line); break;
                    case TOKEN_SLASH:   writeBytecodeChunk(chunk, OP_DIVIDE, line); break; // Real division
                    // Add TOKEN_INT_DIV (OP_INT_DIVIDE), TOKEN_MOD (OP_MODULO) later
                    // Add comparison ops later (OP_EQUAL, OP_LESS, OP_GREATER, etc.)
                    default:
                        fprintf(stderr, "Compiler error: Unknown binary operator %s\n", tokenTypeToString(node->token->type));
                        break;
                }
            }
            break;
        
        case AST_UNARY_OP:
            compileExpression(node->left, chunk); // Operand on stack
            if (node->token) {
                switch (node->token->type) {
                    case TOKEN_MINUS: writeBytecodeChunk(chunk, OP_NEGATE, line); break;
                    case TOKEN_NOT:   writeBytecodeChunk(chunk, OP_NOT, line);    break;
                    default:
                        fprintf(stderr, "Compiler error: Unknown unary operator %s\n", tokenTypeToString(node->token->type));
                        break;
                }
            }
            break;

        case AST_PROCEDURE_CALL: // Actually a function call if used in an expression
            // Handle arguments
            if (node->children) {
                for (int i = 0; i < node->child_count; i++) {
                    compileExpression(node->children[i], chunk); // Arguments pushed onto stack
                }
            }
            // TODO: Need a proper way to map function name to a built-in index or user function index
            // For now, let's assume a hardcoded index for 'upcase' if we were compiling it.
            // For SimpleMath, we only have WriteLn which is handled as a statement.
            // If upcase was here:
            // int builtinIndex = find_builtin_index("upcase");
            // writeBytecodeChunk(chunk, OP_CALL_BUILTIN, line);
            // writeBytecodeChunk(chunk, (uint8_t)builtinIndex, line);
            // writeBytecodeChunk(chunk, (uint8_t)node->child_count, line); // Arg count
            fprintf(stderr, "Compiler: Expression function calls not fully implemented yet for %s.\n", node->token ? node->token->value : "unknown_func");
            // Push a dummy value for now if a result is expected
            if (node->var_type != TYPE_VOID) {
                 int dummyIdx = addConstantToChunk(chunk, makeInt(0)); // Dummy result
                 writeBytecodeChunk(chunk, OP_CONSTANT, line);
                 writeBytecodeChunk(chunk, dummyIdx, line);
            }
            break;


        // Parenthesized expressions are handled by AST structure, no specific opcode needed
        // (e.g., (2*3) just compiles 2, then 3, then MUL)

        default:
            fprintf(stderr, "Compiler warning: Unhandled AST node type %s in compileExpression.\n", astTypeToString(node->type));
            // Push a dummy value if an expression was expected to leave something on stack
            int dummyIdx = addConstantToChunk(chunk, makeInt(0)); // Default dummy value
            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, dummyIdx, line);
            break;
    }
}
