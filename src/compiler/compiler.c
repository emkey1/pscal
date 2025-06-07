// src/compiler/compiler.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // For strcmp, strdup, atoll

#include "compiler/compiler.h"
#include "backend_ast/builtin.h" // For isBuiltin
#include "core/utils.h"
#include "core/types.h"
#include "frontend/ast.h"
#include "symbol/symbol.h" // For access to the main global symbol table, if needed,
                           // though for bytecode compilation, we often build our own tables/mappings.
#include "vm/vm.h"         // For HostFunctionID
#include "backend_ast/interpreter.h" // For makeCopyOfValue()
#include "compiler/bytecode.h"

static bool compiler_had_error = false;


// --- Forward Declarations for Recursive Compilation ---
static void compileNode(AST* node, BytecodeChunk* chunk, int current_line_approx);
static void compileExpression(AST* node, BytecodeChunk* chunk, int current_line_approx);
static void compileStatement(AST* node, BytecodeChunk* chunk, int current_line_approx);
static void compileDefinedFunction(AST* func_decl_node, BytecodeChunk* chunk, int line); // From previous step

#define MAX_GLOBALS 256 // Define a reasonable limit for global variables for now

// --- Global/Module State for Compiler ---
// For mapping global variable names to an index during this compilation pass.
// This is a simplified approach for global variables.
// A more robust compiler might use a symbol table specific to compilation.
typedef struct {
    char* name;
} CompilerGlobalVarInfo;

int compilerGlobalCount = 0;

CompilerGlobalVarInfo compilerGlobals[MAX_GLOBALS]; // MAX_GLOBALS from an appropriate header or defined here

CompilerConstant compilerConstants[MAX_COMPILER_CONSTANTS];
int compilerConstantCount = 0;

// Helper to add a constant during compilation
void addCompilerConstant(const char* name_original_case, Value value, int line) {
    if (compilerConstantCount >= MAX_COMPILER_CONSTANTS) {
        fprintf(stderr, "L%d: Compiler error: Too many compile-time constants.\n", line);
        freeValue(&value); // Free incoming value's data if it's heap allocated
        compiler_had_error = true;
        return;
    }

    char canonical_name[MAX_SYMBOL_LENGTH]; // Assuming MAX_SYMBOL_LENGTH from globals.h
    strncpy(canonical_name, name_original_case, sizeof(canonical_name) - 1);
    canonical_name[sizeof(canonical_name) - 1] = '\0';
    toLowerString(canonical_name); // Convert to lowercase

    for (int i = 0; i < compilerConstantCount; i++) {
        if (compilerConstants[i].name && strcmp(compilerConstants[i].name, canonical_name) == 0) {
            // Constant with this canonical name already exists.
            fprintf(stderr, "L%d: Compiler warning: Constant '%s' (canonical: '%s') redefined. Using new value.\n", line, name_original_case, canonical_name);
            
            freeValue(&compilerConstants[i].value); // Free old value's contents
            compilerConstants[i].value = makeCopyOfValue(&value); // Store a deep copy of the new value
            
            // The caller of addCompilerConstant is responsible for the original 'value' it passed.
            // If 'value' was a temporary (e.g., from makeString prior to this call), its s_val should be freed by the caller
            // after addCompilerConstant returns, as makeCopyOfValue would have duplicated its content.
            return;
        }
    }

    // Add new constant
    compilerConstants[compilerConstantCount].name = strdup(canonical_name);
    if (!compilerConstants[compilerConstantCount].name) {
        fprintf(stderr, "L%d: Malloc error for canonical constant name '%s'\n", line, canonical_name);
        freeValue(&value); // Free incoming value's data
        compiler_had_error = true;
        return;
    }
    compilerConstants[compilerConstantCount].value = makeCopyOfValue(&value);
    compilerConstantCount++;
    // Caller of addCompilerConstant is responsible for the original 'value' passed.
}

// Helper to find a compile-time constant
Value* findCompilerConstant(const char* name_original_case) {
    char canonical_name[MAX_SYMBOL_LENGTH];
    // ... (strncpy, toLowerString as before) ...
    strncpy(canonical_name, name_original_case, MAX_SYMBOL_LENGTH - 1);
    canonical_name[MAX_SYMBOL_LENGTH - 1] = '\0';
    toLowerString(canonical_name);

    fprintf(stderr, "[DEBUG findCompilerConstant] Searching for '%s' (canonical: '%s'). Current compilerConstantCount = %d\n",
            name_original_case, canonical_name, compilerConstantCount);
    fflush(stderr);

    for (int i = 0; i < compilerConstantCount; ++i) {
        fprintf(stderr, "[DEBUG findCompilerConstant] Checking against stored: '%s'\n", compilerConstants[i].name);
        fflush(stderr);
        if (compilerConstants[i].name && strcmp(compilerConstants[i].name, canonical_name) == 0) {
            fprintf(stderr, "[DEBUG findCompilerConstant] Found '%s' at index %d.\n", canonical_name, i);
            fflush(stderr);
            return &compilerConstants[i].value;
        }
    }
    fprintf(stderr, "[DEBUG findCompilerConstant] '%s' (canonical: '%s') NOT FOUND.\n", name_original_case, canonical_name);
    fflush(stderr);
    return NULL;
}

// New function for parser/compiler to evaluate simple constant expressions
Value evaluateCompileTimeValue(AST* node) {
    if (!node) return makeVoid(); // Or some error indicator

    switch (node->type) {
        case AST_NUMBER:
            if (node->token) {
                if (node->var_type == TYPE_REAL || (node->token->type == TOKEN_REAL_CONST)) {
                    return makeReal(atof(node->token->value));
                } else {
                    return makeInt(atoll(node->token->value));
                }
            }
            break;
        case AST_STRING:
            if (node->token) return makeString(node->token->value);
            break;
        case AST_BOOLEAN:
            return makeBoolean(node->i_val);
        case AST_NIL:
            return makeNil();
        case AST_VARIABLE: // Reference to another constant
            if (node->token && node->token->value) {
                Value* const_val_ptr = findCompilerConstant(node->token->value);
                if (const_val_ptr) {
                    fprintf(stderr, "[DEBUG evaluateCTV] Found '%s'. Type: %s\n", node->token->value, varTypeToString(const_val_ptr->type));
                    return makeCopyOfValue(const_val_ptr); // Return a copy
                } else {
                    // Constant not found, cannot evaluate at this time
                    fprintf(stderr, "[DEBUG evaluateCTV] FAILED to find const_var: '%s'\n", node->token->value);
                    // Return a special error Value or TYPE_VOID
                    return makeVoid();
                }
            }
            break;
        case AST_BINARY_OP:
            if (node->left && node->right && node->token) {
                Value left_val = evaluateCompileTimeValue(node->left);
                Value right_val = evaluateCompileTimeValue(node->right);
                Value result = makeVoid(); // Default

                // Only proceed if both operands evaluated successfully to known types
                if (left_val.type != TYPE_VOID && left_val.type != TYPE_UNKNOWN &&
                    right_val.type != TYPE_VOID && right_val.type != TYPE_UNKNOWN) {

                    // Handle simple integer arithmetic for now (e.g., DIV)
                    if (left_val.type == TYPE_INTEGER && right_val.type == TYPE_INTEGER) {
                        if (node->token->type == TOKEN_INT_DIV) {
                            if (right_val.i_val == 0) {
                                fprintf(stderr, "Compile-time Error: Division by zero in constant expression.\n");
                            } else {
                                result = makeInt(left_val.i_val / right_val.i_val);
                            }
                        } else if (node->token->type == TOKEN_PLUS) {
                            result = makeInt(left_val.i_val + right_val.i_val);
                        } // Add other ops: MINUS, MUL
                        // TODO: Add other operations and type combinations (Real, String concat)
                    } else {
                        // Did not evaluate to two integers, or op not supported yet for folding
                        // fprintf(stderr, "Compile-time Info: Cannot fold binary op (%s) with types %s, %s.\n",
                        //         tokenTypeToString(node->token->type), varTypeToString(left_val.type), varTypeToString(right_val.type));
                    }
                }
                freeValue(&left_val);
                freeValue(&right_val);
                return result; // Returns makeVoid() if folding failed
            }
            break;
        // Add AST_UNARY_OP if needed
        default:
            break; // Cannot evaluate this node type to a constant value
    }
    return makeVoid(); // Default: cannot evaluate
}

// Reset for each compilation
void resetCompilerConstants(void) {
    fprintf(stderr, "!!!! RESETTING COMPILER CONSTANTS at %s:%d !!!! (count becomes 0)\n", __FILE__, __LINE__); // Add file and line
    fflush(stderr);
    for (int i = 0; i < compilerConstantCount; ++i) {
        if (compilerConstants[i].name) {
            free(compilerConstants[i].name);
            compilerConstants[i].name = NULL;
        }
        freeValue(&compilerConstants[i].value);
    }
    compilerConstantCount = 0;
}

// Helper to get the source line number from an AST node's token
// Provides a fallback if token is NULL.
static int getLine(AST* node) {
    int current_line_approx = 0;
    if (node && node->token && node->token->line > 0) return node->token->line;
    if (node && node->left && node->left->token && node->left->token->line > 0) return node->left->token->line;
    if (node && node->child_count > 0 && node->children[0] && node->children[0]->token && node->children[0]->token->line > 0) return node->children[0]->token->line;
    return current_line_approx > 0 ? current_line_approx : 0; // Return current_line_approx if non-zero
}

// Finds or adds a global variable name and returns its index.
// For bytecode, this index is what OP_GET_GLOBAL/OP_SET_GLOBAL will use.
static int resolveGlobalVariableIndex(BytecodeChunk* chunk, const char* name, int line) {
    // ... (Your existing implementation - seems okay) ...
    for (int i = 0; i < compilerGlobalCount; i++) {
        if (compilerGlobals[i].name && strcmp(compilerGlobals[i].name, name) == 0) {
            return i;
        }
    }
    if (compilerGlobalCount < MAX_GLOBALS) {
        compilerGlobals[compilerGlobalCount].name = strdup(name);
        if (!compilerGlobals[compilerGlobalCount].name) {
            fprintf(stderr, "L%d: Compiler error: Memory allocation failed for global variable name '%s'.\n", line, name);
            exit(1); // Consider a less abrupt exit or error flag
        }
        return compilerGlobalCount++;
    }
    fprintf(stderr, "L%d: Compiler error: Too many global variables.\n", line);
    exit(1); // Consider a less abrupt exit
    return -1;
}

// --- Simple Type Resolution (Placeholder - needs to be robust) ---
// This is a very basic version. A real one would look up user-defined types.

/*
static VarType resolveASTTypeToVarType(AST* type_node, int line_for_error) {
    if (!type_node) return TYPE_UNKNOWN;

    if (type_node->var_type != TYPE_UNKNOWN && type_node->var_type != TYPE_VOID) {
        return type_node->var_type;
    }
    // AST_TYPE_IDENTIFIER is defined in your core/types.h
    if (type_node->type == AST_TYPE_IDENTIFIER && type_node->token && type_node->token->value) {
        const char* type_name = type_node->token->value;
        if (strcasecmp(type_name, "integer") == 0) return TYPE_INTEGER;
        if (strcasecmp(type_name, "real") == 0) return TYPE_REAL;
        // ... (other built-in types) ...
        if (strcasecmp(type_name, "boolean") == 0) return TYPE_BOOLEAN; // Ensure BOOLEAN is handled
        fprintf(stderr, "L%d: Compiler Warning: Unknown type name '%s' in resolveASTTypeToVarType. Defaulting to UNKNOWN.\n", line_for_error, type_name);
        return TYPE_UNKNOWN;
    }
    fprintf(stderr, "L%d: Compiler Warning: Could not resolve AST type node %s (%s) to VarType. Defaulting to UNKNOWN.\n",
            line_for_error, astTypeToString(type_node->type),
            type_node->token ? type_node->token->value : "N/A");
    return TYPE_UNKNOWN;
}
 */

// --- Main Compilation Function ---
bool compileASTToBytecode(AST* rootNode, BytecodeChunk* outputChunk) {
    // Debug print for entry and initial state of compilerConstantCount
    fprintf(stderr, "[DEBUG compileASTToBytecode] ENTER. Initial compilerConstantCount = %d\n", compilerConstantCount);
    fflush(stderr);

    if (!rootNode) {
        fprintf(stderr, "Compiler Error: Cannot compile NULL AST rootNode.\n");
        compiler_had_error = true; // Ensure error is flagged
        return false;
    }
    if (!outputChunk) {
        fprintf(stderr, "Compiler Error: outputChunk is NULL in compileASTToBytecode.\n");
        compiler_had_error = true; // Ensure error is flagged
        return false;
    }
    
    // Initialize the output chunk for this compilation pass.
    // This is crucial if outputChunk is being reused or needs a fresh state.
    initBytecodeChunk(outputChunk);

    // Reset compiler's global variable counter for THIS compilation pass.
    // This is for variables defined with VAR, not for compile-time constants.
    compilerGlobalCount = 0;

    // Reset the global error flag for THIS compilation pass.
    compiler_had_error = false;

    // DO NOT CALL resetCompilerConstants() here.
    // The parser has already populated compilerConstants.
    // The "simplified pre-pass" that was here is now redundant and removed.

    fprintf(stderr, "[DEBUG compileASTToBytecode] Before main compileNode call, compilerConstantCount = %d\n", compilerConstantCount);
    fflush(stderr);

    // Main compilation pass - determines the top-level node to compile
    if (rootNode->type == AST_PROGRAM) {
        // A program node should have a name (left) and a block (right)
        if (rootNode->right && rootNode->right->type == AST_BLOCK) {
            AST* mainBlock = rootNode->right;
            fprintf(stderr, "[DEBUG compileASTToBytecode] Compiling mainBlock of AST_PROGRAM. Current compilerConstantCount = %d\n", compilerConstantCount);
            fflush(stderr);
            compileNode(mainBlock, outputChunk, getLine(rootNode)); // Pass a valid line number
        } else {
            fprintf(stderr, "Compiler error: AST_PROGRAM node missing main block (rootNode->right is not AST_BLOCK).\n");
            compiler_had_error = true;
        }
    } else if (rootNode->type == AST_BLOCK) {
        // Allow compiling a block directly (e.g., for unit implementations if handled this way)
        fprintf(stderr, "[DEBUG compileASTToBytecode] Compiling AST_BLOCK directly. Current compilerConstantCount = %d\n", compilerConstantCount);
        fflush(stderr);
        compileNode(rootNode, outputChunk, getLine(rootNode)); // Pass a valid line number
    } else if (rootNode->type == AST_UNIT) {
        // If you compile units separately into bytecode, handle AST_UNIT here.
        // This might involve compiling its interface (if needed for symbol tables)
        // and its implementation's declaration block and initialization block.
        // For now, assuming unit's constants are part of global compilerConstants.
        // The main executable part of a unit is its initialization block.
        fprintf(stderr, "[DEBUG compileASTToBytecode] Compiling AST_UNIT. Name: '%s'. Current compilerConstantCount = %d\n",
                rootNode->token ? rootNode->token->value : "N/A", compilerConstantCount);
        fflush(stderr);
        // A unit typically has interface declarations (left), implementation declarations (extra),
        // and an initialization block (right).
        // You'd compile the parts that generate executable code, usually the init block.
        // Global declarations from the unit's INTERFACE and IMPLEMENTATION (like CONST, VAR)
        // should have been processed by the parser and compiler's const/var handling already.
        
        // Example: Compile the initialization block of the unit if it exists
        if (rootNode->right && (rootNode->right->type == AST_COMPOUND || rootNode->right->type == AST_BLOCK)) { // Assuming init block is in 'right'
             compileNode(rootNode->right, outputChunk, getLine(rootNode->right));
        }
        // Or, if the unit structure implies compiling its main "block" which includes its parts:
        // compileNode(rootNode, outputChunk, getLine(rootNode)); // This would need compileNode to handle AST_UNIT
        // For now, let's assume the main interest is program or program block.
        // If compiling a unit directly to bytecode isn't supported yet, this can be an error:
        // fprintf(stderr, "Compiler warning: Direct bytecode compilation of AST_UNIT not fully implemented.\n");
        // compiler_had_error = true; // Or false if only init block is compiled or it's a valid path
    }
    else {
        fprintf(stderr, "Compiler error: Expected AST_PROGRAM or AST_BLOCK as root for compilation, got %s.\n", astTypeToString(rootNode->type));
        compiler_had_error = true;
    }

    // Add OP_HALT only if compilation was successful and produced some code.
    // If outputChunk->count is 0 and no error, HALT is still fine.
    // If error, perhaps skip HALT or clear chunk.
    if (!compiler_had_error) {
        writeBytecodeChunk(outputChunk, OP_HALT, rootNode ? getLine(rootNode) : 0); // Use a valid line if possible
    } else {
        // If errors occurred, the chunk might be incomplete/invalid.
        // Consider not adding HALT or even clearing the chunk.
        // For now, it will add HALT even if errors happened before this point but set compiler_had_error.
    }

    fprintf(stderr, "[DEBUG compileASTToBytecode] EXIT. Final compilerConstantCount = %d. Had Error: %s. Bytecode size: %d\n",
            compilerConstantCount, compiler_had_error ? "YES" : "NO", outputChunk->count);
    fflush(stderr);

    return !compiler_had_error;
}

// --- Recursive Compilation Dispatcher ---
static void compileNode(AST* node, BytecodeChunk* chunk, int current_line_approx) {
    if (!node) return;
    int line = getLine(node);
    if (line <= 0) line = current_line_approx;

    switch (node->type) {
        case AST_BLOCK: {
            // First pass for procedure/function declarations to define them (get their start address)
            // This pass ensures forward jumps to procedures/functions can be resolved.
            for (int i = 0; i < node->child_count; i++) {
                AST* child = node->children[i];
                if (!child) continue;
                if (child->type == AST_PROCEDURE_DECL || child->type == AST_FUNCTION_DECL) {
                    compileNode(child, chunk, getLine(child) > 0 ? getLine(child) : line);
                } else if (child->type == AST_COMPOUND) {
                    // Handle case where declarations (including procedures) might be grouped in a compound node
                    // (e.g., if that's how the parser structures unit implementation declarations)
                    bool only_procs_or_funcs = child->child_count > 0;
                    for(int k=0; k < child->child_count; ++k) {
                        if (!child->children[k] ||
                            !(child->children[k]->type == AST_PROCEDURE_DECL || child->children[k]->type == AST_FUNCTION_DECL)) {
                            only_procs_or_funcs = false;
                            break;
                        }
                    }
                    if (only_procs_or_funcs) {
                        for(int k=0; k < child->child_count; ++k) {
                            compileNode(child->children[k], chunk, getLine(child->children[k]) > 0 ? getLine(child->children[k]) : line);
                        }
                    }
                }
            }

            // Second pass for VAR_DECLs (to define globals), CONST/TYPE (informational), and the main body compound statement
            AST* main_body_compound = NULL;
            for (int i = 0; i < node->child_count; i++) {
                AST* child = node->children[i];
                if (!child) continue;
                int child_line = getLine(child) > 0 ? getLine(child) : line;
                
                switch(child->type) {
                    case AST_VAR_DECL:
                    case AST_CONST_DECL:
                    case AST_TYPE_DECL:
                    case AST_USES_CLAUSE: // Add USES_CLAUSE here to be handled by the outer compileNode
                        compileNode(child, chunk, child_line); // Let the main compileNode dispatcher handle these
                        break;
                    case AST_COMPOUND: {
                        // This logic tries to identify if the AST_COMPOUND is the main executable body
                        // or just a list of declarations (which should have been handled if they were individual nodes)
                        bool is_executable_body = false;
                        if (child->child_count > 0) {
                            for(int k=0; k < child->child_count; ++k) {
                                if (child->children[k]) {
                                    ASTNodeType type = child->children[k]->type;
                                    // If it contains anything that's not a declaration, it's likely executable
                                    if (type != AST_VAR_DECL && type != AST_CONST_DECL &&
                                        type != AST_TYPE_DECL && type != AST_PROCEDURE_DECL &&
                                        type != AST_FUNCTION_DECL && type != AST_USES_CLAUSE) {
                                        is_executable_body = true;
                                        break;
                                    }
                                }
                            }
                        } else { // Empty BEGIN..END is an executable body (a no-op one)
                            is_executable_body = true;
                        }

                        if (is_executable_body) {
                            if (!main_body_compound) {
                                main_body_compound = child;
                            } else {
                                // This implies multiple executable compound blocks at the same level within an AST_BLOCK,
                                // which is unusual for standard Pascal program/procedure structure.
                                fprintf(stderr, "L%d: Compiler Warning: Multiple executable AST_COMPOUND statements found directly in AST_BLOCK. Compiling all.\n", child_line);
                                compileNode(child, chunk, child_line); // Compile it as a compound statement sequence
                            }
                        } else {
                            // This compound contains only declarations; they should have been individual children
                            // or handled by their specific cases if compileNode was called on them.
                            // This path might indicate a parser structure that groups declarations in an extra compound.
                            #ifdef DEBUG
                            if(dumpExec) fprintf(stderr, "L%d: Compiler Info: AST_COMPOUND child of AST_BLOCK contains only declarations. Individual declarations should be processed.\n", child_line);
                            #endif
                            for(int k=0; k < child->child_count; ++k) { // Process them individually
                                compileNode(child->children[k], chunk, getLine(child->children[k]));
                            }
                        }
                        break;
                    }
                    case AST_PROCEDURE_DECL: // Already handled in the first pass
                    case AST_FUNCTION_DECL:
                        break;
                    default: // Any other type of node here is assumed to be a statement
                        compileStatement(child, chunk, child_line);
                        break;
                }
            }
            
            if (main_body_compound) {
                compileNode(main_body_compound, chunk, getLine(main_body_compound) > 0 ? getLine(main_body_compound) : line);
            } else if (node->parent && (node->parent->type == AST_PROGRAM)) {
                 // Only error for missing main body if this block is the direct child of AST_PROGRAM
                 bool has_executable_statements = false;
                 for(int i=0; i < node->child_count; ++i) {
                     if(node->children[i] && node->children[i]->type == AST_COMPOUND) { // Heuristic
                        has_executable_statements = true; break;
                     }
                 }
                 if (!has_executable_statements) {
                     fprintf(stderr, "L%d: Compiler Error: No main compound statement (BEGIN...END block) found in program block.\n", line);
                     compiler_had_error = true;
                 }
            } // For procedures/functions, an empty executable part is fine.
            break;
        }
        
        case AST_VAR_DECL: {
             VarType declared_type_enum = node->var_type;
             AST* type_specifier_node = node->right; // This node describes the type, e.g., AST_VARIABLE "TBrickArray" or "integer"

             if (node->children) {
                 for (int i = 0; i < node->child_count; i++) {
                     AST* varNameNode = node->children[i];
                     if (varNameNode && varNameNode->token) {
                         int var_name_idx = addConstantToChunk(chunk, makeString(varNameNode->token->value));
                         uint8_t type_name_idx = 0; // Default for simple types or anonymous complex types

                         // If it's a complex type AND its type_specifier_node has a token (i.e., it's a named type)
                         if ((declared_type_enum == TYPE_ARRAY || declared_type_enum == TYPE_RECORD) &&
                             type_specifier_node && type_specifier_node->token &&
                             (type_specifier_node->type == AST_VARIABLE || type_specifier_node->type == AST_TYPE_REFERENCE)) {
                             // The token of type_specifier_node holds the type name (e.g., "TBrickArray")
                             type_name_idx = (uint8_t)addConstantToChunk(chunk, makeString(type_specifier_node->token->value));
                         }
                         // For anonymous types like "var x: array[1..2] of integer;", type_specifier_node would be AST_ARRAY_TYPE
                         // and would not have a .token in the same way. type_name_idx would remain 0.
                         // The VM would need more complex bytecode to handle defining anonymous structures if this path is taken.

                         writeBytecodeChunk(chunk, OP_DEFINE_GLOBAL, getLine(varNameNode));
                         writeBytecodeChunk(chunk, (uint8_t)var_name_idx, getLine(varNameNode));
                         writeBytecodeChunk(chunk, type_name_idx, getLine(varNameNode)); // Operand for type name string index
                         writeBytecodeChunk(chunk, (uint8_t)declared_type_enum, getLine(varNameNode)); // Operand for VarType enum
                         
                         resolveGlobalVariableIndex(chunk, varNameNode->token->value, getLine(varNameNode));
                     }
                 }
             }
             break;
         }

        case AST_CONST_DECL: {
            if (node->token && node->token->value) {
                // Check if the parser already evaluated and added this constant
                Value* existing_const_val = findCompilerConstant(node->token->value);

                if (existing_const_val) {
                    // Constant was already processed by the parser and its value is known.
                    // No further action needed here for its definition.
                    // When this constant is USED in an expression, compileExpression for AST_VARIABLE
                    // will find it via findCompilerConstant and emit OP_CONSTANT with its value.
                    #ifdef DEBUG
                    if(dumpExec) fprintf(stderr, "L%d: Compiler Info: CONST_DECL for '%s' already processed by parser. Value inlined on use.\n",
                            getLine(node), node->token->value);
                    #endif
                } else {
                    // This constant was NOT a simple literal resolvable by the parser,
                    // or parser's evaluateCompileTimeValue couldn't fold it.
                    // This is the path for BrickWidth = ScreenWidth div BricksPerRow if ScreenWidth wasn't known to parser.
                    // Use the existing logic to generate runtime initialization code for it.
                    if (node->left) { // Ensure there's an expression for the value
                        fprintf(stderr, "L%d: Compiler Info: CONST_DECL for '%s' (unresolved by parser, type: %s). Generating runtime init for VM.\n",
                                getLine(node), node->token->value, astTypeToString(node->left->type));
                        
                        char canonical_const_name[MAX_SYMBOL_LENGTH];
                        strncpy(canonical_const_name, node->token->value, sizeof(canonical_const_name) - 1);
                        canonical_const_name[sizeof(canonical_const_name) - 1] = '\0';
                        toLowerString(canonical_const_name); // Parser should already provide lowercase

                        int var_name_idx = addConstantToChunk(chunk, makeString(canonical_const_name));
                        // ... (rest of your existing runtime init codegen: OP_DEFINE_GLOBAL, compileExpression(node->left), OP_SET_GLOBAL) ...
                        // Ensure types are correctly inferred/used for OP_DEFINE_GLOBAL.
                        VarType const_actual_type = node->var_type;
                        if (const_actual_type == TYPE_VOID || const_actual_type == TYPE_UNKNOWN) {
                           if (node->left->type == AST_BINARY_OP && node->left->token && node->left->token->type == TOKEN_INT_DIV) {
                               const_actual_type = TYPE_INTEGER;
                           } else {
                               // Attempt to get type from the expression node if annotated, otherwise default.
                               const_actual_type = node->left->var_type != TYPE_VOID ? node->left->var_type : TYPE_INTEGER;
                               if(const_actual_type == TYPE_VOID)
                                 fprintf(stderr, "L%d: Compiler Warning: Could not infer type for const expression '%s', defaulting to INTEGER for DEFINE_GLOBAL.\n", getLine(node), node->token->value);
                           }
                        }
                        uint8_t type_name_idx = 0;

                        writeBytecodeChunk(chunk, OP_DEFINE_GLOBAL, getLine(node));
                        writeBytecodeChunk(chunk, (uint8_t)var_name_idx, getLine(node));
                        writeBytecodeChunk(chunk, type_name_idx, getLine(node));
                        writeBytecodeChunk(chunk, (uint8_t)const_actual_type, getLine(node));
                        
                        resolveGlobalVariableIndex(chunk, canonical_const_name, getLine(node));
                        compileExpression(node->left, chunk, getLine(node->left));
                        writeBytecodeChunk(chunk, OP_SET_GLOBAL, getLine(node));
                        writeBytecodeChunk(chunk, (uint8_t)var_name_idx, getLine(node));

                    } else {
                        fprintf(stderr, "L%d: Compiler Error: CONST_DECL for '%s' missing value expression.\n", getLine(node), node->token->value);
                        compiler_had_error = true;
                    }
                }
            } else {
                 fprintf(stderr, "L%d: Compiler Error: Ill-formed AST_CONST_DECL node (missing token).\n", getLine(node));
                 compiler_had_error = true;
            }
            break;
        }

        case AST_TYPE_DECL: { // Moved from AST_BLOCK's second loop default path
            if (node->token && node->token->value) {
                #ifdef DEBUG
                if(dumpExec) fprintf(stderr, "L%d: Compiler Info: Processed TYPE_DECL for '%s'. (No bytecode emitted).\n",
                        line, node->token->value);
                #endif
            } else {
                fprintf(stderr, "L%d: Compiler Warning: Ill-formed AST_TYPE_DECL node.\n", line);
            }
            break;
        }
        
        case AST_USES_CLAUSE: { // Added case
            #ifdef DEBUG
            if(dumpExec) fprintf(stderr, "L%d: Compiler Info: Encountered AST_USES_CLAUSE. (Handled by parser/linker, no direct bytecode).\n", line);
            #endif
            // No bytecode needed for the USES clause itself at this stage.
            break;
        }

        case AST_PROCEDURE_DECL:
        case AST_FUNCTION_DECL: {
            if (!node->token || !node->token->value) {
                fprintf(stderr, "L%d: Compiler Error: Procedure/Function declaration node missing name token.\n", line);
                compiler_had_error = true;
                break;
            }
            #ifdef DEBUG
            if(dumpExec) fprintf(stderr, "L%d: Compiler: Processing definition of %s '%s'. Emitting jump over body.\n",
                    line, astTypeToString(node->type), node->token->value);
            #endif

            // Emit a jump to skip over the body. Main code path doesn't execute function bodies directly.
            writeBytecodeChunk(chunk, OP_JUMP, line);
            int jump_over_body_operand_offset = chunk->count;
            emitShort(chunk, 0xFFFF, line); // Placeholder for jump offset

            // Compile the function/procedure body itself.
            compileDefinedFunction(node, chunk, line);

            // Backpatch the jump to go to the current end of the chunk.
            uint16_t offset_to_skip_body = (uint16_t)(chunk->count - (jump_over_body_operand_offset + 2));
            patchShort(chunk, jump_over_body_operand_offset, offset_to_skip_body);
            
            #ifdef DEBUG
            if(dumpExec) fprintf(stderr, "L%d: Compiler: Patched jump over body for '%s'. Jump offset: %u.\n",
                    line, node->token->value, offset_to_skip_body);
            #endif
            break;
        }

        case AST_COMPOUND: // This is for executable blocks of statements (e.g., main program body, loop body, if branch body)
            for (int i = 0; i < node->child_count; i++) {
                if (node->children[i]) { // Added NULL check
                    compileStatement(node->children[i], chunk, getLine(node->children[i]) > 0 ? getLine(node->children[i]) : line);
                }
            }
            break;
        
        default:
            // Check if it's a statement type node that should be handled by compileStatement
            if (node->type >= AST_ASSIGN && node->type <= AST_USES_CLAUSE) { // Adjusted range, USES_CLAUSE now handled explicitly
                 compileStatement(node, chunk, line);
            }
            // Check if it's an expression type node that should be handled by compileExpression
            // This path is usually hit if an expression is on its own line (e.g. function call for side effect, deprecated)
            // or if compileNode is called directly on an expression subtree.
            else if ((node->type >= AST_BINARY_OP && node->type <= AST_VARIABLE) ||
                     node->type == AST_NIL || node->type == AST_BOOLEAN || node->type == AST_ARRAY_LITERAL || // expand this for all expression types
                     node->type == AST_FIELD_ACCESS || node->type == AST_ARRAY_ACCESS || node->type == AST_DEREFERENCE ||
                     node->type == AST_FORMATTED_EXPR) {
                 compileExpression(node, chunk, line);
                 // If an expression is compiled at the "node" level (not as part of a larger statement/expression),
                 // its result will be on the stack. It usually needs to be popped if not used.
                 // This depends on context; compileStatement for AST_PROCEDURE_CALL already handles popping results of functions called as procedures.
                 // For now, let's assume if compileNode hits an expression directly, its value isn't immediately used by an outer construct.
                 // However, this path in compileNode's default is tricky. It's better to ensure all statement/expression
                 // types are routed through compileStatement/compileExpression from their containing structures.
                 #ifdef DEBUG
                 if(dumpExec) fprintf(stderr, "L%d: Compiler Info: Expression node %s compiled directly by compileNode. Consider if its value needs OP_POP.\n", line, astTypeToString(node->type));
                 #endif
            } else {
                 fprintf(stderr, "L%d: Compiler warning: Unhandled AST node type %s in main compileNode dispatcher.\n", line, astTypeToString(node->type));
            }
            break;
    }
}

// --- Compile Defined Function/Procedure Body ---
static void compileDefinedFunction(AST* func_decl_node, BytecodeChunk* chunk, int line) {
    const char* func_name = func_decl_node->token->value;
    int func_bytecode_start_address = chunk->count;

    Symbol* proc_symbol = lookupSymbolIn(procedure_table, func_name);
    if (!proc_symbol) {
        fprintf(stderr, "L%d: Compiler INTERNAL ERROR: Procedure/function '%s' not found for body compilation.\n", line, func_name);
        return;
    }
    
    proc_symbol->bytecode_address = func_bytecode_start_address;
    proc_symbol->is_defined = true;
    proc_symbol->arity = func_decl_node->child_count;
    
    // Simple local variable counting (a more robust solution would traverse the declaration block)
    AST* blockNode = (func_decl_node->type == AST_PROCEDURE_DECL) ? func_decl_node->right : func_decl_node->extra;
    uint8_t local_count = 0;
    if (blockNode && blockNode->type == AST_BLOCK && blockNode->child_count > 0 && blockNode->children[0]->type == AST_COMPOUND) {
        AST* decls = blockNode->children[0];
        for (int i = 0; i < decls->child_count; i++) {
            if (decls->children[i] && decls->children[i]->type == AST_VAR_DECL) {
                local_count += decls->children[i]->child_count;
            }
        }
    }
    proc_symbol->locals_count = local_count;

    #ifdef DEBUG
    if(dumpExec) fprintf(stderr, "L%d: Compiler: Compiling body of %s '%s' at address %d, Arity: %d, Locals: %d\n",
            line, astTypeToString(func_decl_node->type), func_name, func_bytecode_start_address, proc_symbol->arity, proc_symbol->locals_count);
    #endif

    // Compile the actual block of the procedure/function
    if (blockNode) {
        compileNode(blockNode, chunk, getLine(blockNode));
    }
    
    // Explicitly add return. If it's a function, result is on stack. If procedure, a nil/dummy value is.
    writeBytecodeChunk(chunk, OP_RETURN, line);
    
    #ifdef DEBUG
    if(dumpExec) fprintf(stderr, "L%d: Compiler: Finished body of %s '%s'. Ended with OP_RETURN.\n",
            line, astTypeToString(func_decl_node->type), func_name);
    #endif
}

static void compileStatement(AST* node, BytecodeChunk* chunk, int current_line_approx) {
    if (!node) return;
    int line = getLine(node);
    if (line <= 0) line = current_line_approx;

    switch (node->type) {
        case AST_ASSIGN: { // From your compiler.c
            // Compile the expression on the right-hand side (leaves value on stack)
            compileExpression(node->right, chunk, getLine(node->right));
            
            // Left-hand side (target of assignment)
            if (node->left && node->left->type == AST_VARIABLE && node->left->token) {
                const char* varName = node->left->token->value;
                // For OP_SET_GLOBAL, operand is the index of the variable name string in the constant pool.
                int nameIndex = addConstantToChunk(chunk, makeString(varName)); // makeString creates copy
                writeBytecodeChunk(chunk, OP_SET_GLOBAL, line);
                writeBytecodeChunk(chunk, (uint8_t)nameIndex, line);
            } else {
                // TODO: Handle other LValue types like array access, record field access
                fprintf(stderr, "L%d: Compiler error: LHS of assignment is not a simple global variable ('%s'). Complex LValues not yet supported for SET operations.\n", line, node->left && node->left->token ? node->left->token->value : "unknown_lhs");
                compiler_had_error = true;
            }
            break;
        }
        case AST_WHILE: { // From your compiler.c, adapted
            if (!node->left || !node->right) {
                fprintf(stderr, "L%d: Compiler error: WHILE node is missing condition or body.\n", line);
                compiler_had_error = true;
                break;
            }
            
            int loop_start_address = chunk->count; // Address before condition evaluation
            
            // 1. Compile the condition
            compileExpression(node->left, chunk, getLine(node->left)); // Condition result on stack
            
            // 2. Emit OP_JUMP_IF_FALSE with a placeholder for the offset to jump out of loop
            writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line);
            int exit_jump_operand_offset = chunk->count; // Remember where the jump's 2-byte operand starts
            emitShort(chunk, 0xFFFF, line); // Placeholder for jump offset (2 bytes)
            
            // 3. Compile the loop body
            compileStatement(node->right, chunk, getLine(node->right)); // Body is typically AST_COMPOUND or single statement
            
            // 4. Emit OP_JUMP back to the start of the loop (before condition)
            writeBytecodeChunk(chunk, OP_JUMP, line);
            // Offset = target_address (loop_start_address) - address_of_instruction_AFTER_this_jump_operand
            int backward_jump_offset = loop_start_address - (chunk->count + 2); // +2 for the short operand of OP_JUMP
            emitShort(chunk, (uint16_t)backward_jump_offset, line); // Cast to uint16_t for 2's complement if negative
            
            // 5. Backpatch the OP_JUMP_IF_FALSE
            // Target for OP_JUMP_IF_FALSE is the instruction immediately after the backward OP_JUMP (current end of chunk)
            // Offset = target_address (chunk->count) - address_of_instruction_AFTER_JUMP_IF_FALSE_operand
            uint16_t forward_jump_offset = (uint16_t)(chunk->count - (exit_jump_operand_offset + 2));
            patchShort(chunk, exit_jump_operand_offset, forward_jump_offset);
            break;
        }

        case AST_WRITELN: { // From your compiler.c
            int argCount = node->child_count;
            for (int i = 0; i < argCount; i++) { // Children are expressions to write
                compileExpression(node->children[i], chunk, getLine(node->children[i]));
            }
            writeBytecodeChunk(chunk, OP_WRITE_LN, line);
            writeBytecodeChunk(chunk, (uint8_t)argCount, line); // Pass arg count to VM
            break;
        }
        
        case AST_IF: { // From your compiler.c, adapted
            if (!node->left || !node->right) { /* error */ return; } // Condition (left), Then-branch (right)
            
            // 1. Compile the condition
            compileExpression(node->left, chunk, line);

            // 2. Emit OP_JUMP_IF_FALSE to jump over the THEN branch (or to ELSE)
            int jump_to_else_or_end_addr = chunk->count;
            writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line);
            emitShort(chunk, 0xFFFF, line); // Placeholder for offset

            // 3. Compile the THEN branch
            compileStatement(node->right, chunk, getLine(node->right));

            if (node->extra) { // If there's an ELSE branch (node->extra)
                // 4a. Emit OP_JUMP to skip the ELSE branch (after THEN branch has executed)
                int jump_over_else_addr = chunk->count;
                writeBytecodeChunk(chunk, OP_JUMP, line);
                emitShort(chunk, 0xFFFF, line); // Placeholder

                // 4b. Backpatch OP_JUMP_IF_FALSE: jump to start of ELSE branch
                // Target is current chunk->count
                uint16_t offsetToElse = (uint16_t)(chunk->count - (jump_to_else_or_end_addr + 1 + 2)); // +1 for opcode, +2 for its short
                patchShort(chunk, jump_to_else_or_end_addr + 1, offsetToElse);

                // 4c. Compile the ELSE branch
                compileStatement(node->extra, chunk, getLine(node->extra));

                // 4d. Backpatch OP_JUMP (that jumps over ELSE): jump to current end
                uint16_t offsetToEndOfIf = (uint16_t)(chunk->count - (jump_over_else_addr + 1 + 2));
                patchShort(chunk, jump_over_else_addr + 1, offsetToEndOfIf);
            } else { // No ELSE branch
                // 4e. Backpatch OP_JUMP_IF_FALSE to jump to end of THEN branch (current end of chunk)
                uint16_t offsetToEndOfThen = (uint16_t)(chunk->count - (jump_to_else_or_end_addr + 1 + 2));
                patchShort(chunk, jump_to_else_or_end_addr + 1, offsetToEndOfThen);
            }
            break;
        }
        
        case AST_COMPOUND: { // For BEGIN...END blocks, using AST_COMPOUND from your types.h
            for (int i = 0; i < node->child_count; i++) {
                if (node->children[i]) {
                    compileStatement(node->children[i], chunk, getLine(node->children[i]));
                }
            }
            break;
        }
        case AST_PROCEDURE_CALL: {
             int call_line = getLine(node);

             // 1. Compile all arguments first and push them onto the stack.
             for (int i = 0; i < node->child_count; i++) {
                 if (node->children[i]) {
                     compileExpression(node->children[i], chunk, getLine(node->children[i]));
                 } else {
                      fprintf(stderr, "L%d: Compiler error: NULL argument node in call to '%s'.\n", call_line, node->token ? node->token->value : "unknown_proc");
                 }
             }

             // 2. Handle the call itself
             if (node->token && isBuiltin(node->token->value)) {
                 // It's a built-in. All built-ins use OP_CALL_BUILTIN.
                 // The VM handler will pop the result for procedures.
                 char normalized_name[MAX_SYMBOL_LENGTH];
                 strncpy(normalized_name, node->token->value, sizeof(normalized_name) - 1);
                 normalized_name[sizeof(normalized_name) - 1] = '\0';
                 toLowerString(normalized_name);

                 int nameIndex = addConstantToChunk(chunk, makeString(normalized_name));
                 writeBytecodeChunk(chunk, OP_CALL_BUILTIN, call_line);
                 writeBytecodeChunk(chunk, (uint8_t)nameIndex, call_line);
                 writeBytecodeChunk(chunk, (uint8_t)node->child_count, call_line);

                 // If it was a function call used as a statement, its result must be popped.
                 if (node->var_type != TYPE_VOID) {
                     writeBytecodeChunk(chunk, OP_POP, call_line);
                 }

             } else if (node->token) {
                 // It's a user-defined procedure or function call.
                 // <<<< THIS IS THE KEY CHANGE >>>>
                 const char* name_to_lookup = node->token->value;
                 Symbol* proc_symbol = lookupSymbolIn(procedure_table, name_to_lookup);

                 if (proc_symbol && proc_symbol->is_defined) {
                     // Check if the number of arguments provided matches the procedure's arity.
                     if (proc_symbol->arity != node->child_count) {
                         fprintf(stderr, "L%d: Compiler Error: Routine '%s' expects %d arguments, but %d were given.\n", call_line, name_to_lookup, proc_symbol->arity, node->child_count);
                         compiler_had_error = true;
                     } else {
                         // Emit the new OP_CALL instruction with the routine's address and arity.
                         writeBytecodeChunk(chunk, OP_CALL, call_line);
                         emitShort(chunk, (uint16_t)proc_symbol->bytecode_address, call_line);
                         writeBytecodeChunk(chunk, proc_symbol->arity, call_line);
                     }
                 } else {
                     fprintf(stderr, "L%d: Compiler Error: Undefined or forward-declared procedure '%s'.\n", call_line, name_to_lookup);
                     compiler_had_error = true;
                 }

                 // If it was a function call used as a statement, its result must also be popped.
                 if (node->var_type != TYPE_VOID) {
                     writeBytecodeChunk(chunk, OP_POP, call_line);
                 }

             } else {
                  fprintf(stderr, "L%d: Compiler Error: AST_PROCEDURE_CALL node missing token for procedure name.\n", call_line);
                  compiler_had_error = true;
             }
             break;
         }
        
        default: {
            bool was_handled_in_default = false;
            // This default case should ideally only be reached for node types
            // that are not explicitly handled by other cases and might be
            // expressions used as statements, or truly unhandled statement types.

            // First, specifically check if an AST_PROCEDURE_CALL reached default.
            if (node->type == AST_PROCEDURE_CALL) {
                // An AST_PROCEDURE_CALL should have been handled by its specific 'case AST_PROCEDURE_CALL:'.
                // If it reaches here, it strongly indicates a problem with the switch structure itself,
                // like a missing 'break;' in a case BEFORE 'case AST_PROCEDURE_CALL:', causing a fallthrough.
                if (node->token && node->token->value) {
                    char lookup_name_default[MAX_SYMBOL_LENGTH * 2 + 2];
                    char original_display_name_default[MAX_SYMBOL_LENGTH*2 + 2];
                    bool is_call_qualified_default = false;

                    // Determine functionName and qualified names for lookup
                    if (node->left && node->left->type == AST_VARIABLE && node->left->token && node->left->token->value) {
                        snprintf(original_display_name_default, sizeof(original_display_name_default), "%s.%s", node->left->token->value, node->token->value);
                        char unit_name_lower[MAX_SYMBOL_LENGTH], func_name_lower[MAX_SYMBOL_LENGTH];
                        strncpy(unit_name_lower, node->left->token->value, sizeof(unit_name_lower)-1); unit_name_lower[sizeof(unit_name_lower)-1] = '\0';
                        strncpy(func_name_lower, node->token->value, sizeof(func_name_lower)-1); func_name_lower[sizeof(func_name_lower)-1] = '\0';
                        toLowerString(unit_name_lower); toLowerString(func_name_lower);
                        snprintf(lookup_name_default, sizeof(lookup_name_default), "%s.%s", unit_name_lower, func_name_lower);
                        is_call_qualified_default = true;
                    } else {
                        strncpy(original_display_name_default, node->token->value, sizeof(original_display_name_default)-1); original_display_name_default[sizeof(original_display_name_default)-1] = '\0';
                        strncpy(lookup_name_default, node->token->value, sizeof(lookup_name_default)-1); lookup_name_default[sizeof(lookup_name_default)-1] = '\0';
                        toLowerString(lookup_name_default);
                    }
                    
                    bool is_function = false;
                    // Check if it's a known function (built-in or user-defined) that returns a value
                    if (isBuiltin(node->token->value) && !is_call_qualified_default && getBuiltinType(node->token->value) == BUILTIN_TYPE_FUNCTION) {
                        is_function = true;
                    } else if (!isBuiltin(node->token->value) || is_call_qualified_default) {
                        Symbol* sym = lookupSymbolIn(procedure_table, lookup_name_default);
                        if (sym && sym->type != TYPE_VOID && sym->is_defined) { // Check if it's a defined function
                            is_function = true;
                        }
                    }

                    if (is_function) {
                        // This is a function call being used as a statement. Compile it and pop its result.
                        fprintf(stderr, "L%d: Compiler Info (compileStatement default): Compiling function call '%s' as statement (result will be popped).\n", line, original_display_name_default);
                        compileExpression(node, chunk, line); // This will call compileExpression's AST_PROCEDURE_CALL case
                        writeBytecodeChunk(chunk, OP_POP, line);
                    } else {
                        // This is an AST_PROCEDURE_CALL for an actual procedure (or undefined)
                        // that incorrectly reached the default case.
                        fprintf(stderr, "L%d: Compiler FATAL ERROR: Procedure call '%s' (type: %s) reached default case in compileStatement. This implies a missing 'break;' in a case before 'case AST_PROCEDURE_CALL:'.\n",
                                line, original_display_name_default, astTypeToString(node->type));
                        compiler_had_error = true;
                    }
                } else {
                     fprintf(stderr, "L%d: Compiler WARNING: AST_PROCEDURE_CALL without a callable token reached default statement handler.\n", line);
                     compiler_had_error = true;
                }
                was_handled_in_default = true;
            }
            // Handle other specific expression types that might be standalone statements
            // This part should NOT include AST_PROCEDURE_CALL, as it's handled above.
            else if ( (node->type >= AST_BINARY_OP && node->type <= AST_VARIABLE && node->type != AST_PROCEDURE_CALL) ||
                      (node->type == AST_NUMBER) || (node->type == AST_STRING) || (node->type == AST_BOOLEAN) ||
                      (node->type == AST_NIL) || (node->type == AST_FIELD_ACCESS) || (node->type == AST_ARRAY_ACCESS) ||
                      (node->type == AST_DEREFERENCE) || (node->type == AST_FORMATTED_EXPR)
                    ) {
                #ifdef DEBUG
                if(dumpExec) fprintf(stderr, "L%d: Compiler Info: compileStatement default: Treating AST node %s as expression statement (will be popped).\n", line, astTypeToString(node->type));
                #endif
                compileExpression(node, chunk, line);
                writeBytecodeChunk(chunk, OP_POP, line);
                was_handled_in_default = true;
            } else if (node->type == AST_NOOP) {
                // Do nothing for NOOP
                was_handled_in_default = true;
            }

            if (!was_handled_in_default) {
                 fprintf(stderr, "L%d: Compiler WARNING: Unhandled AST node type %s in compileStatement's default case.\n", line, astTypeToString(node->type));
                 // compiler_had_error = true; // Optionally make this an error
            }
            break;
        } // End default case
    }
}

static void compileExpression(AST* node, BytecodeChunk* chunk, int current_line_approx) {
    fprintf(stderr, ">>>> DEBUG: compileExpression: ENTERED for Node Type: %s, Token: '%s' <<<<\n",
                node ? astTypeToString(node->type) : "NULL_NODE",
                node && node->token ? (node->token->value ? node->token->value : "N/A_VAL") : "NO_TOKEN");
        fflush(stderr);
    if (!node) return;
    int line = getLine(node);
    if (line <= 0) line = current_line_approx;
    
    Token* node_token = node->token;

    switch (node->type) {
        case AST_NUMBER: {
            if (!node_token) { /* error */ break; }
            Value numVal;
            if (node_token->type == TOKEN_REAL_CONST) {
                numVal = makeReal(atof(node_token->value));
            } else {
                numVal = makeInt(atoll(node_token->value));
            }
            int constIndex = addConstantToChunk(chunk, numVal);
            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, (uint8_t)constIndex, line);
            break;
        }
        case AST_STRING: {
            if (!node_token || !node_token->value) { /* error */ break; }
            Value strVal = makeString(node_token->value);
            int constIndex = addConstantToChunk(chunk, strVal);
            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, (uint8_t)constIndex, line);
            break;
        }
        case AST_VARIABLE: {
            if (!node_token || !node_token->value) { /* error */ break; }
            const char* varName = node_token->value;
            if (strcasecmp(varName, "quitrequested") == 0) {
                // Special handling for QuitRequested: compile as a host function call
                // This mirrors the AST interpreter's special handling for this variable.
                #ifdef DEBUG
                if(dumpExec) fprintf(stderr, "L%d: Compiler: AST_VARIABLE '%s' recognized as special. Emitting OP_CALL_HOST for HOST_FN_QUIT_REQUESTED.\n", line, varName);
                #endif
                writeBytecodeChunk(chunk, OP_CALL_HOST, line);
                writeBytecodeChunk(chunk, (uint8_t)HOST_FN_QUIT_REQUESTED, line);
                // No arguments are pushed for HOST_FN_QUIT_REQUESTED when used like a variable.
                // The host function itself doesn't expect arguments from the stack for this ID.
            } else {
                // Check if it's a known compile-time constant first
                Value* const_val_ptr = findCompilerConstant(varName);
                if (const_val_ptr) {
                    // It's a compile-time constant, emit OP_CONSTANT with its value
                    // makeCopyOfValue is important here, especially if the constant is a string,
                    // to ensure the chunk's constant pool owns its copy of the data.
                    Value val_to_add_to_chunk = makeCopyOfValue(const_val_ptr);
                    int constIndex = addConstantToChunk(chunk, val_to_add_to_chunk);
                    // val_to_add_to_chunk is a temporary on-stack Value struct.
                    // If makeCopyOfValue allocated memory for val_to_add_to_chunk.s_val (for strings),
                    // addConstantToChunk just copies the pointer. The chunk now "owns" that s_val.
                    // So, no freeValue(&val_to_add_to_chunk) is needed here.

                    writeBytecodeChunk(chunk, OP_CONSTANT, line);
                    writeBytecodeChunk(chunk, (uint8_t)constIndex, line);
                } else {
                    // If not a compile-time constant, treat as a variable to be fetched globally
                    int nameIndex = addConstantToChunk(chunk, makeString(varName)); // makeString creates copy for the constant pool
                    writeBytecodeChunk(chunk, OP_GET_GLOBAL, line);
                    writeBytecodeChunk(chunk, (uint8_t)nameIndex, line);
                }
            }
            break;
        }
        case AST_BINARY_OP: {
            compileExpression(node->left, chunk, getLine(node->left));
            compileExpression(node->right, chunk, getLine(node->right));
            if (node_token) { // node_token is the operator
                switch (node_token->type) {
                    case TOKEN_PLUS:          writeBytecodeChunk(chunk, OP_ADD, line); break;
                    case TOKEN_MINUS:         writeBytecodeChunk(chunk, OP_SUBTRACT, line); break;
                    case TOKEN_MUL:           writeBytecodeChunk(chunk, OP_MULTIPLY, line); break;
                    case TOKEN_SLASH:         writeBytecodeChunk(chunk, OP_DIVIDE, line); break;
                    case TOKEN_INT_DIV:       writeBytecodeChunk(chunk, OP_INT_DIV, line); break;
                    case TOKEN_AND:           writeBytecodeChunk(chunk, OP_AND, line); break;
                    case TOKEN_OR:            writeBytecodeChunk(chunk, OP_OR, line); break;
                    case TOKEN_EQUAL:         writeBytecodeChunk(chunk, OP_EQUAL, line); break;
                    case TOKEN_NOT_EQUAL:     writeBytecodeChunk(chunk, OP_NOT_EQUAL, line); break;
                    case TOKEN_LESS:          writeBytecodeChunk(chunk, OP_LESS, line); break;
                    case TOKEN_LESS_EQUAL:    writeBytecodeChunk(chunk, OP_LESS_EQUAL, line); break;
                    case TOKEN_GREATER:       writeBytecodeChunk(chunk, OP_GREATER, line); break;
                    case TOKEN_GREATER_EQUAL: writeBytecodeChunk(chunk, OP_GREATER_EQUAL, line); break;
                    default:
                        fprintf(stderr, "L%d: Compiler error: Unknown binary operator %s\n", line, tokenTypeToString(node_token->type));
                        compiler_had_error = true;
                        break;
                }
            }
            break;
        }
        case AST_UNARY_OP: {
            compileExpression(node->left, chunk, getLine(node->left)); // Operand
            if (node_token) { // node_token is the operator
                switch (node_token->type) {
                    case TOKEN_MINUS: writeBytecodeChunk(chunk, OP_NEGATE, line); break;
                    case TOKEN_NOT:   writeBytecodeChunk(chunk, OP_NOT, line);    break;
                    default:
                        fprintf(stderr, "L%d: Compiler error: Unknown unary operator %s\n", line, tokenTypeToString(node_token->type));
                        compiler_had_error = true;
                        break;
                }
            }
            break;
        }
        case AST_BOOLEAN: { // From your types.h
            if (!node_token && node->i_val == 0 && node->var_type == TYPE_BOOLEAN) { // Heuristic for literal false from parser
                 // This can happen if parser directly sets i_val for true/false
            } else if (!node_token) {
                fprintf(stderr, "L%d: Compiler error: AST_BOOLEAN node missing token.\n", line);
                compiler_had_error = true;
                break;
            }
             // node->i_val should be set by parser: 1 for true, 0 for false
            Value boolConst = makeBoolean(node->i_val != 0);
            int constIndex = addConstantToChunk(chunk, boolConst);
            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, (uint8_t)constIndex, line);
            break;
        }
        
        case AST_PROCEDURE_CALL: { // AST_PROCEDURE_CALL is in your types.h
            int line = getLine(node);
            if (line <= 0) line = current_line_approx;

            const char* functionName = NULL;
            bool isCallQualified = false;

            // --- CORRECTED LOGIC for functionName and isCallQualified ---
            if (node->left &&
                node->left->type == AST_VARIABLE &&
                node->left->token && node->left->token->value && /* Ensure qualifier token and value exist */
                node->token && node->token->value && node->token->type == TOKEN_IDENTIFIER) {
                // This pattern matches: unit_identifier.function_identifier(...)
                // unit_identifier is node->left->token->value
                // function_identifier is node->token->value
                functionName = node->token->value; // The actual function name
                isCallQualified = true;
            } else if (node->token && node->token->value && node->token->type == TOKEN_IDENTIFIER) {
                // This pattern matches simple calls: function_identifier(...) or BuiltinFunction(...)
                // where node->left is not a unit qualifier (or is NULL)
                functionName = node->token->value;
                isCallQualified = false;
            } else {
                // If functionName could not be determined (e.g., AST node is malformed)
                fprintf(stderr, "L%d: Compiler error: Invalid callee in AST_PROCEDURE_CALL (expression). Callee token ('%s', type %s) is not a valid identifier or structure is unexpected.\n",
                        line,
                        node->token ? (node->token->value ? node->token->value : "NULL_VAL") : "NO_TOKEN",
                        node->token ? tokenTypeToString(node->token->type) : "NO_TOKEN_TYPE");
                compiler_had_error = true;
                
                // Fallback: Pop any arguments that might have been compiled IF arg compilation was before this check
                // (currently it's after, which is better).
                // Push a nil value onto the stack as this expression failed.
                writeBytecodeChunk(chunk, OP_CONSTANT, line);
                writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk, makeNil()), line); // From utils.h
                break; // Exit this case
            }
            // --- END OF CORRECTED LOGIC for functionName and isCallQualified ---
            
            uint8_t arg_count = 0;
            // Arguments for AST_PROCEDURE_CALL are direct children,
            // as seen in your parser's output for WriteLn(..., Abs(i), ...).
            if (node->child_count > 0 && node->children) {
                for (int i = 0; i < node->child_count; i++) {
                    if (node->children[i]) {
                        compileExpression(node->children[i], chunk, getLine(node->children[i]));
                        arg_count++;
                    }
                }
            }
            // Note: Your original code had 'AST* arg_list_node = node->right;' which is different from
            // how the parser seems to structure calls like Abs(i) where args are direct children.
            // The loop above assumes arguments are in node->children. If your parser *sometimes*
            // puts arguments in node->right (e.g. as an AST_COMPOUND), you'll need to unify how
            // your parser creates AST_PROCEDURE_CALL nodes or add logic here to check both.
            // For now, this matches the observed AST for builtins in WriteLn.


            // Dispatch based on function type
            if (strcasecmp(functionName, "quitrequested") == 0 && !isCallQualified) {
                if (arg_count > 0) {
                    fprintf(stderr, "L%d: Compiler error: QuitRequested() expects no arguments when used as a function.\n", line);
                    compiler_had_error = true;
                    for(uint8_t i=0; i<arg_count; ++i) writeBytecodeChunk(chunk, OP_POP, line); // Pop compiled arguments
                    writeBytecodeChunk(chunk, OP_CONSTANT, line);
                    writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk, makeNil()), line);
                } else {
                    writeBytecodeChunk(chunk, OP_CALL_HOST, line);
                    writeBytecodeChunk(chunk, (uint8_t)HOST_FN_QUIT_REQUESTED, line);
                    // This host function is expected to push its boolean result.
                }
            } else if (isBuiltin(functionName) && !isCallQualified) {
                BuiltinRoutineType type = getBuiltinType(functionName); // From builtin.h

                if (type == BUILTIN_TYPE_PROCEDURE) {
                    fprintf(stderr, "L%d: Compiler Error: Built-in procedure '%s' cannot be used as a function in an expression.\n", line, functionName);
                    compiler_had_error = true;
                    for(uint8_t i = 0; i < arg_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                    writeBytecodeChunk(chunk, OP_CONSTANT, line);
                    writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk, makeNil()), line);
                } else if (type == BUILTIN_TYPE_FUNCTION) {
                    // TODO: Implement arity check for built-in functions using a mechanism
                    // like `getBuiltinExpectedArity(functionName)` if you want stricter compile-time checks.
                    // int expected_builtin_arity = getBuiltinExpectedArity(functionName);
                    // if (expected_builtin_arity >= 0 && expected_builtin_arity != arg_count) { ... error ... }

                    char normalized_name[MAX_SYMBOL_LENGTH]; // MAX_SYMBOL_LENGTH from globals.h or symbol.h
                    strncpy(normalized_name, functionName, sizeof(normalized_name) - 1);
                    normalized_name[sizeof(normalized_name) - 1] = '\0';
                    toLowerString(normalized_name); // VM likely uses normalized names for built-in lookup

                    int nameIndex = addConstantToChunk(chunk, makeString(normalized_name)); // Use corrected makeString
                    writeBytecodeChunk(chunk, OP_CALL_BUILTIN, line);
                    writeBytecodeChunk(chunk, (uint8_t)nameIndex, line);
                    writeBytecodeChunk(chunk, arg_count, line); // Pass actual number of arguments compiled
                } else { // BUILTIN_TYPE_NONE or other issue
                     fprintf(stderr, "L%d: Compiler Error: '%s' is not a recognized built-in function for expression context (getBuiltinType failed or unknown).\n", line, functionName);
                    compiler_had_error = true;
                    for(uint8_t i = 0; i < arg_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                    writeBytecodeChunk(chunk, OP_CONSTANT, line);
                    writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk, makeNil()), line);
                }
            } else { // User-defined function call
                char lookup_name[MAX_SYMBOL_LENGTH * 2 + 2];
                char original_display_name[MAX_SYMBOL_LENGTH * 2 + 2];

                if (isCallQualified) {
                    snprintf(original_display_name, sizeof(original_display_name), "%s.%s", node->left->token->value, functionName);
                    // For lookup, ensure consistent casing as per your symbol table insertion
                    char unit_name_lower[MAX_SYMBOL_LENGTH];
                    char func_name_lower[MAX_SYMBOL_LENGTH];
                    strncpy(unit_name_lower, node->left->token->value, sizeof(unit_name_lower)-1); unit_name_lower[sizeof(unit_name_lower)-1] = '\0';
                    strncpy(func_name_lower, functionName, sizeof(func_name_lower)-1); func_name_lower[sizeof(func_name_lower)-1] = '\0';
                    toLowerString(unit_name_lower);
                    toLowerString(func_name_lower);
                    snprintf(lookup_name, sizeof(lookup_name), "%s.%s", unit_name_lower, func_name_lower);
                } else {
                    strncpy(original_display_name, functionName, sizeof(original_display_name) - 1);
                    original_display_name[sizeof(original_display_name) - 1] = '\0';

                    strncpy(lookup_name, functionName, sizeof(lookup_name) - 1);
                    lookup_name[sizeof(lookup_name) - 1] = '\0';
                    toLowerString(lookup_name);
                }
                                    
                Symbol* func_symbol = lookupSymbolIn(procedure_table, lookup_name);
                
                if (func_symbol && func_symbol->is_defined) {
                    // Assuming func_symbol->type (VarType) == TYPE_VOID means it's a procedure in Pascal terms.
                    if (func_symbol->type == TYPE_VOID) {
                        fprintf(stderr, "L%d: Compiler Error: Procedure '%s' cannot be used as a function in an expression(compileExpression).\n", line, original_display_name);
                        compiler_had_error = true;
                        for(uint8_t i=0; i<arg_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                        writeBytecodeChunk(chunk, OP_CONSTANT, line);
                        writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk, makeNil()), line);
                    } else if (func_symbol->arity != arg_count) {
                        fprintf(stderr, "L%d: Compiler Error: Function '%s' expects %d arguments, but %d were given.\n", line, original_display_name, func_symbol->arity, arg_count);
                        compiler_had_error = true;
                        for(uint8_t i=0; i<arg_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                        writeBytecodeChunk(chunk, OP_CONSTANT, line);
                        writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk, makeNil()), line);
                    } else {
                        writeBytecodeChunk(chunk, OP_CALL, line);
                        emitShort(chunk, (uint16_t)func_symbol->bytecode_address, line);
                        writeBytecodeChunk(chunk, arg_count, line); // Pass actual arg_count
                    }
                } else {
                     if (func_symbol && !func_symbol->is_defined) {
                         fprintf(stderr, "L%d: Compiler Error: Function '%s' (lookup: '%s') is forward declared or not defined for use in expression.\n", line, original_display_name, lookup_name);
                         compiler_had_error = true;
                    } else {
                        fprintf(stderr, "L%d: Compiler Error: Undefined function '%s' (lookup: '%s') called in expression.\n", line, original_display_name, lookup_name);
                        compiler_had_error = true;
                    }
                    for(uint8_t i=0; i<arg_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                    writeBytecodeChunk(chunk, OP_CONSTANT, line);
                    writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk, makeNil()), line);
                }
            }
            break;
        }
        
        default:
            fprintf(stderr, "L%d: Compiler warning: Unhandled AST node type %s in compileExpression.\n", line, astTypeToString(node->type));
            int dummyIdx = addConstantToChunk(chunk, makeInt(0)); // Push dummy 0 for expression context
            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, (uint8_t)dummyIdx, line);
            break;
    }
}
