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

#define MAX_GLOBALS 256 // Define a reasonable limit for global variables for now

static bool compiler_had_error = false;

typedef struct {
    char* name;
    int depth; // Scope depth
} CompilerLocal;

typedef struct {
    CompilerLocal locals[MAX_GLOBALS]; // Re-use MAX_GLOBALS for max locals per function
    int local_count;
    int scope_depth;
} FunctionCompilerState;

FunctionCompilerState* current_function_compiler = NULL;


// --- Forward Declarations for Recursive Compilation ---
static void compileNode(AST* node, BytecodeChunk* chunk, int current_line_approx);
static void compileExpression(AST* node, BytecodeChunk* chunk, int current_line_approx);
static void compileStatement(AST* node, BytecodeChunk* chunk, int current_line_approx);
static void compileDefinedFunction(AST* func_decl_node, BytecodeChunk* chunk, int line);
static void compileStoreForLValue(AST* container, BytecodeChunk* chunk, int line);

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

static void initFunctionCompiler(FunctionCompilerState* fc) {
    fc->local_count = 0;
    fc->scope_depth = 0;
    // The first slot (index 0) is reserved for the function itself, for potential recursion.
    // So we add a dummy local for it.
    //CompilerLocal* first_local = &fc->locals[fc->local_count++];
    //first_local->depth = 0;
    //first_local->name = ""; // No name, just occupies slot 0
}

// Add this new function
static void addLocal(FunctionCompilerState* fc, const char* name, int line) {
    if (fc->local_count == MAX_GLOBALS) {
        fprintf(stderr, "L%d: Compiler error: Too many local variables in one function.\n", line);
        compiler_had_error = true;
        return;
    }
    CompilerLocal* local = &fc->locals[fc->local_count++];
    local->name = strdup(name);
    local->depth = fc->scope_depth;
}

// Add this new function
static int resolveLocal(FunctionCompilerState* fc, const char* name) {
    for (int i = fc->local_count - 1; i >= 0; i--) {
        CompilerLocal* local = &fc->locals[i];
        if (strcmp(name, local->name) == 0) {
            return i;
        }
    }
    return -1;
}

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
}

// Helper to find a compile-time constant
Value* findCompilerConstant(const char* name_original_case) {
    char canonical_name[MAX_SYMBOL_LENGTH];
    strncpy(canonical_name, name_original_case, MAX_SYMBOL_LENGTH - 1);
    canonical_name[MAX_SYMBOL_LENGTH - 1] = '\0';
    toLowerString(canonical_name);

    for (int i = 0; i < compilerConstantCount; ++i) {
        if (compilerConstants[i].name && strcmp(compilerConstants[i].name, canonical_name) == 0) {
            return &compilerConstants[i].value;
        }
    }
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
                    return makeCopyOfValue(const_val_ptr); // Return a copy
                } else {
                    return makeVoid();
                }
            }
            break;
        case AST_BINARY_OP:
            if (node->left && node->right && node->token) {
                Value left_val = evaluateCompileTimeValue(node->left);
                Value right_val = evaluateCompileTimeValue(node->right);
                Value result = makeVoid(); // Default

                if (left_val.type != TYPE_VOID && left_val.type != TYPE_UNKNOWN &&
                    right_val.type != TYPE_VOID && right_val.type != TYPE_UNKNOWN) {

                    if (left_val.type == TYPE_INTEGER && right_val.type == TYPE_INTEGER) {
                        if (node->token->type == TOKEN_INT_DIV) {
                            if (right_val.i_val == 0) {
                                fprintf(stderr, "Compile-time Error: Division by zero in constant expression.\n");
                            } else {
                                result = makeInt(left_val.i_val / right_val.i_val);
                            }
                        } else if (node->token->type == TOKEN_PLUS) {
                            result = makeInt(left_val.i_val + right_val.i_val);
                        }
                    }
                }
                freeValue(&left_val);
                freeValue(&right_val);
                return result;
            }
            break;
        case AST_UNARY_OP:
            if (node->left && node->token) {
                Value operand_val = evaluateCompileTimeValue(node->left);
                if (operand_val.type == TYPE_VOID || operand_val.type == TYPE_UNKNOWN) {
                    freeValue(&operand_val);
                    return makeVoid();
                }

                if (node->token->type == TOKEN_MINUS) {
                    if (operand_val.type == TYPE_INTEGER) {
                        operand_val.i_val = -operand_val.i_val;
                        return operand_val; // Return the modified value
                    } else if (operand_val.type == TYPE_REAL) {
                        operand_val.r_val = -operand_val.r_val;
                        return operand_val; // Return the modified value
                    }
                } else if (node->token->type == TOKEN_PLUS) {
                    // Unary plus is a no-op, just return the operand's value.
                    return operand_val;
                }
                // Free the value if the operator was not handled for its type
                freeValue(&operand_val);
            }
            break;
        default:
            break;
    }
    return makeVoid();
}

// Reset for each compilation
void resetCompilerConstants(void) {
    for (int i = 0; i < compilerConstantCount; ++i) {
        if (compilerConstants[i].name) {
            free(compilerConstants[i].name);
            compilerConstants[i].name = NULL;
        }
        freeValue(&compilerConstants[i].value);
    }
    compilerConstantCount = 0;
}

static int getLine(AST* node) {
    if (node && node->token && node->token->line > 0) return node->token->line;
    if (node && node->left && node->left->token && node->left->token->line > 0) return node->left->token->line;
    if (node && node->child_count > 0 && node->children[0] && node->children[0]->token && node->children[0]->token->line > 0) return node->children[0]->token->line;
    return 0;
}

static int resolveGlobalVariableIndex(BytecodeChunk* chunk, const char* name, int line) {
    for (int i = 0; i < compilerGlobalCount; i++) {
        if (compilerGlobals[i].name && strcmp(compilerGlobals[i].name, name) == 0) {
            return i;
        }
    }
    if (compilerGlobalCount < MAX_GLOBALS) {
        compilerGlobals[compilerGlobalCount].name = strdup(name);
        if (!compilerGlobals[compilerGlobalCount].name) {
            fprintf(stderr, "L%d: Compiler error: Memory allocation failed for global variable name '%s'.\n", line, name);
            exit(1);
        }
        return compilerGlobalCount++;
    }
    fprintf(stderr, "L%d: Compiler error: Too many global variables.\n", line);
    exit(1);
    return -1;
}

bool compileASTToBytecode(AST* rootNode, BytecodeChunk* outputChunk) {
    if (!rootNode) {
        fprintf(stderr, "Compiler Error: Cannot compile NULL AST rootNode.\n");
        return false;
    }
    if (!outputChunk) {
        fprintf(stderr, "Compiler Error: outputChunk is NULL in compileASTToBytecode.\n");
        return false;
    }
    
    initBytecodeChunk(outputChunk);
    compilerGlobalCount = 0;
    compiler_had_error = false;

    if (rootNode->type == AST_PROGRAM) {
        if (rootNode->right && rootNode->right->type == AST_BLOCK) {
            compileNode(rootNode->right, outputChunk, getLine(rootNode));
        } else {
            fprintf(stderr, "Compiler error: AST_PROGRAM node missing main block.\n");
            compiler_had_error = true;
        }
    } else {
        fprintf(stderr, "Compiler error: Expected AST_PROGRAM as root for compilation, got %s.\n", astTypeToString(rootNode->type));
        compiler_had_error = true;
    }

    if (!compiler_had_error) {
        writeBytecodeChunk(outputChunk, OP_HALT, rootNode ? getLine(rootNode) : 0);
    }

    return !compiler_had_error;
}

static void compileNode(AST* node, BytecodeChunk* chunk, int current_line_approx) {
    if (!node) return;
    int line = getLine(node);
    if (line <= 0) line = current_line_approx;

    switch (node->type) {
        case AST_BLOCK: {
            for (int i = 0; i < node->child_count; i++) {
                AST* child = node->children[i];
                if (!child) continue;
                if (child->type == AST_PROCEDURE_DECL || child->type == AST_FUNCTION_DECL) {
                    compileNode(child, chunk, getLine(child) > 0 ? getLine(child) : line);
                }
            }
            for (int i = 0; i < node->child_count; i++) {
                AST* child = node->children[i];
                if (!child) continue;
                if (child->type != AST_PROCEDURE_DECL && child->type != AST_FUNCTION_DECL) {
                    compileNode(child, chunk, getLine(child) > 0 ? getLine(child) : line);
                }
            }
            break;
        }
        case AST_VAR_DECL: {
            // If we are not compiling a function, these are global variables.
            if (current_function_compiler == NULL) {
                 VarType declared_type_enum = node->var_type;
                 AST* type_specifier_node = node->right;
                
                 AST* actual_type_def_node = type_specifier_node;
                 if (actual_type_def_node && actual_type_def_node->type == AST_TYPE_REFERENCE) {
                     actual_type_def_node = lookupType(actual_type_def_node->token->value);
                 }
                
                 if (node->children) {
                     for (int i = 0; i < node->child_count; i++) {
                         AST* varNameNode = node->children[i];
                         if (varNameNode && varNameNode->token) {
                             // This part is the same: add the variable name to constants
                             int var_name_idx = addConstantToChunk(chunk, makeString(varNameNode->token->value));
                             
                             // Emit the define opcode and the name index
                             writeBytecodeChunk(chunk, OP_DEFINE_GLOBAL, getLine(varNameNode));
                             writeBytecodeChunk(chunk, (uint8_t)var_name_idx, getLine(varNameNode));
                             
                             // Now, emit type information based on the type
                             writeBytecodeChunk(chunk, (uint8_t)declared_type_enum, getLine(varNameNode));

                             if (declared_type_enum == TYPE_ARRAY && actual_type_def_node && actual_type_def_node->type == AST_ARRAY_TYPE) {
                                                         int dimension_count = actual_type_def_node->child_count;
                                 if (dimension_count > 255) {
                                     fprintf(stderr, "L%d: Compiler error: Maximum number of array dimensions (255) exceeded.\n", getLine(varNameNode));
                                     compiler_had_error = true;
                                     break; // Exit the loop for this var decl group
                                 }
                                 // Emit the number of dimensions as an operand
                                 writeBytecodeChunk(chunk, (uint8_t)dimension_count, getLine(varNameNode));

                                 // Loop through each dimension to emit its bounds
                                 for (int dim = 0; dim < dimension_count; dim++) {
                                     AST* subrange = type_specifier_node->children[dim];
                                     if (subrange && subrange->type == AST_SUBRANGE) {
                                         Value lower_b = evaluateCompileTimeValue(subrange->left);
                                         Value upper_b = evaluateCompileTimeValue(subrange->right);
                                         uint8_t lower_idx = addConstantToChunk(chunk, lower_b);
                                         uint8_t upper_idx = addConstantToChunk(chunk, upper_b);
                                         writeBytecodeChunk(chunk, lower_idx, getLine(varNameNode));
                                         writeBytecodeChunk(chunk, upper_idx, getLine(varNameNode));
                                     } else {
                                         fprintf(stderr, "L%d: Compiler error: Malformed array definition for '%s'.\n", getLine(varNameNode), varNameNode->token->value);
                                         compiler_had_error = true;
                                         // Emit dummy operands to maintain instruction size if needed, or just break
                                         writeBytecodeChunk(chunk, 0, getLine(varNameNode));
                                         writeBytecodeChunk(chunk, 0, getLine(varNameNode));
                                     }
                                 }

                                 // Emit element type name index at the end
                                 AST* elem_type = type_specifier_node->right;
                                 const char* elem_type_name = (elem_type && elem_type->token) ? elem_type->token->value : "";
                                 uint8_t elem_name_idx = addConstantToChunk(chunk, makeString(elem_type_name));
                                 writeBytecodeChunk(chunk, elem_name_idx, getLine(varNameNode));
                             } else if (declared_type_enum == TYPE_RECORD || (type_specifier_node && type_specifier_node->type == AST_TYPE_REFERENCE)) {
                                // --- LOGIC FOR NAMED TYPES ---
                                const char* type_name = (type_specifier_node && type_specifier_node->token) ? type_specifier_node->token->value : "";
                                uint8_t type_name_idx = addConstantToChunk(chunk, makeString(type_name));
                                writeBytecodeChunk(chunk, type_name_idx, getLine(varNameNode));
                             } else {
                                // --- LOGIC FOR SIMPLE TYPES ---
                                // Simple type, needs a placeholder operand.
                                writeBytecodeChunk(chunk, 0, getLine(varNameNode));
                             }
                             
                             resolveGlobalVariableIndex(chunk, varNameNode->token->value, getLine(varNameNode));
                         }
                     }
                 }
            }
            // If current_function_compiler is NOT NULL, we are inside a function.
            // Do nothing, as compileDefinedFunction has already processed the local vars.
            break;
        }
        case AST_CONST_DECL:
        case AST_TYPE_DECL:
        case AST_USES_CLAUSE:
            break;
        case AST_PROCEDURE_DECL:
        case AST_FUNCTION_DECL: {
            if (!node->token || !node->token->value) break;
            writeBytecodeChunk(chunk, OP_JUMP, line);
            int jump_over_body_operand_offset = chunk->count;
            emitShort(chunk, 0xFFFF, line);
            compileDefinedFunction(node, chunk, line);
            uint16_t offset_to_skip_body = (uint16_t)(chunk->count - (jump_over_body_operand_offset + 2));
            patchShort(chunk, jump_over_body_operand_offset, offset_to_skip_body);
            break;
        }
        case AST_COMPOUND:
            for (int i = 0; i < node->child_count; i++) {
                if (node->children[i]) {
                    compileNode(node->children[i], chunk, getLine(node->children[i]));
                }
            }
            break;
        default:
            compileStatement(node, chunk, line);
            break;
    }
}

static void compileDefinedFunction(AST* func_decl_node, BytecodeChunk* chunk, int line) {
    FunctionCompilerState fc;
    initFunctionCompiler(&fc);
    current_function_compiler = &fc;
    
    const char* func_name = func_decl_node->token->value;
    int func_bytecode_start_address = chunk->count;

    Symbol* proc_symbol = lookupSymbolIn(procedure_table, func_name);
    if (!proc_symbol) {
        // This case should ideally not be hit if the parser added the symbol.
        // Clean up and return.
        current_function_compiler = NULL;
        return;
    }
    proc_symbol->bytecode_address = func_bytecode_start_address;
    proc_symbol->is_defined = true;
    
    // --- Step 1: Add all parameters to the local scope FIRST. ---
    // Their slot indices (0, 1, 2...) will now match the VM stack frame.
    if (func_decl_node->children) {
        for (int i = 0; i < func_decl_node->child_count; i++) {
            AST* param_group_node = func_decl_node->children[i];
            if (param_group_node && param_group_node->type == AST_VAR_DECL) {
                for (int j = 0; j < param_group_node->child_count; j++) {
                    AST* param_name_node = param_group_node->children[j];
                    if (param_name_node && param_name_node->token) {
                        addLocal(&fc, param_name_node->token->value, getLine(param_name_node));
                    }
                }
            }
        }
    }
    // The number of locals added so far is the arity (parameter count).
    proc_symbol->arity = fc.local_count;

    // --- Step 2: Add all local variables to the scope AFTER parameters. ---
    AST* blockNode = (func_decl_node->type == AST_PROCEDURE_DECL) ? func_decl_node->right : func_decl_node->extra;
    uint8_t local_var_count = 0;
    if (blockNode && blockNode->type == AST_BLOCK && blockNode->child_count > 0 && blockNode->children[0]->type == AST_COMPOUND) {
        AST* decls = blockNode->children[0];
        for (int i = 0; i < decls->child_count; i++) {
            if (decls->children[i] && decls->children[i]->type == AST_VAR_DECL) {
                AST* var_decl_group = decls->children[i];
                for (int j = 0; j < var_decl_group->child_count; j++) {
                    AST* var_name_node = var_decl_group->children[j];
                    if (var_name_node && var_name_node->token) {
                        addLocal(&fc, var_name_node->token->value, getLine(var_name_node));
                        local_var_count++;
                    }
                }
            }
        }
    }
    // The number of true local variables (excluding parameters).
    proc_symbol->locals_count = local_var_count;
    
    // --- Step 3: Compile the function body. ---
    if (blockNode) {
        compileNode(blockNode, chunk, getLine(blockNode));
    }

    // --- Step 4: Emit the return instruction. ---
    // A function with a return value should have its result on the stack.
    // A procedure will have this instruction implicitly add a 'nil' return value if needed.
    writeBytecodeChunk(chunk, OP_RETURN, line);

    // --- Step 5: Clean up the local compiler state for this function. ---
    for(int i = 0; i < fc.local_count; i++) {
        free(fc.locals[i].name);
    }
    current_function_compiler = NULL;
}

static void compileStatement(AST* node, BytecodeChunk* chunk, int current_line_approx) {
    if (!node) return;
    int line = getLine(node);
    if (line <= 0) line = current_line_approx;

    switch (node->type) {
        case AST_WRITELN: {
            int argCount = node->child_count;
            for (int i = 0; i < argCount; i++) {
                compileExpression(node->children[i], chunk, getLine(node->children[i]));
            }
            writeBytecodeChunk(chunk, OP_WRITE_LN, line);
            writeBytecodeChunk(chunk, (uint8_t)argCount, line);
            break;
        }

        case AST_WRITE: {
            int argCount = node->child_count;
            for (int i = 0; i < argCount; i++) {
                compileExpression(node->children[i], chunk, getLine(node->children[i]));
            }
            writeBytecodeChunk(chunk, OP_WRITE, line);
            writeBytecodeChunk(chunk, (uint8_t)argCount, line);
            break;
        }
        case AST_ASSIGN: {
            AST* lvalue = node->left;
            AST* rvalue = node->right;
            // First, compile the R-Value. Its result will be on top of the stack.
            compileExpression(rvalue, chunk, getLine(rvalue));
            // Then, call the recursive helper to generate the chain of GETs and SETs.
            compileStoreForLValue(lvalue, chunk, line);
            break;
        }
        case AST_WHILE: {
            if (!node->left || !node->right) {
                fprintf(stderr, "L%d: Compiler error: WHILE node is missing condition or body.\n", line);
                compiler_had_error = true;
                break;
            }
            int loop_start_address = chunk->count;
            compileExpression(node->left, chunk, getLine(node->left));
            writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line);
            int exit_jump_operand_offset = chunk->count;
            emitShort(chunk, 0xFFFF, line);
            compileStatement(node->right, chunk, getLine(node->right));
            writeBytecodeChunk(chunk, OP_JUMP, line);
            int backward_jump_offset = loop_start_address - (chunk->count + 2);
            emitShort(chunk, (uint16_t)backward_jump_offset, line);
            uint16_t forward_jump_offset = (uint16_t)(chunk->count - (exit_jump_operand_offset + 2));
            patchShort(chunk, exit_jump_operand_offset, forward_jump_offset);
            break;
        }
        case AST_FOR_TO:
        case AST_FOR_DOWNTO: {
            if (!node->left || !node->right || !node->extra || node->child_count < 1) {
                fprintf(stderr, "L%d: Compiler error: Malformed FOR loop AST node.\n", line);
                compiler_had_error = true;
                break;
            }

            AST* loopVarNode = node->children[0];
            const char* varName = loopVarNode->token->value;
            int varNameIndex = addConstantToChunk(chunk, makeString(varName));

            // 1. Compile and assign the start value
            compileExpression(node->left, chunk, getLine(node->left));
            writeBytecodeChunk(chunk, OP_SET_GLOBAL, line);
            writeBytecodeChunk(chunk, (uint8_t)varNameIndex, line);

            // 2. Mark the loop start address
            int loop_start_address = chunk->count;

            // 3. Compile the loop condition check
            //    Stack: [ ... ]
            compileExpression(node->right, chunk, getLine(node->right)); // End value
            //    Stack: [ ..., EndValue ]
            writeBytecodeChunk(chunk, OP_GET_GLOBAL, line); // Loop variable's current value
            writeBytecodeChunk(chunk, (uint8_t)varNameIndex, line);
            //    Stack: [ ..., EndValue, CurrentValue ]

            if (node->type == AST_FOR_TO) {
                writeBytecodeChunk(chunk, OP_GREATER, line); // Check if Current > End
            } else { // AST_FOR_DOWNTO
                writeBytecodeChunk(chunk, OP_LESS, line); // Check if Current < End
            }
            //    Stack: [ ..., ResultOfComparison (boolean) ]
            
            // 4. Jump out of the loop if the condition is met (loop is finished)
            writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line); // Jumps if comparison is FALSE (i.e., continue loop)
            int exit_jump_operand_offset = chunk->count;
            emitShort(chunk, 0xFFFF, line); // Placeholder for jump distance

            // 5. Compile the loop body
            compileStatement(node->extra, chunk, getLine(node->extra));

            // 6. Increment/Decrement the loop variable
            writeBytecodeChunk(chunk, OP_GET_GLOBAL, line);
            writeBytecodeChunk(chunk, (uint8_t)varNameIndex, line);
            
            Value one_val = makeInt(1);
            int one_const_idx = addConstantToChunk(chunk, one_val);
            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, (uint8_t)one_const_idx, line);

            if (node->type == AST_FOR_TO) {
                writeBytecodeChunk(chunk, OP_ADD, line);
            } else { // AST_FOR_DOWNTO
                writeBytecodeChunk(chunk, OP_SUBTRACT, line);
            }
            
            writeBytecodeChunk(chunk, OP_SET_GLOBAL, line);
            writeBytecodeChunk(chunk, (uint8_t)varNameIndex, line);

            // 7. Jump back to the beginning of the loop
            writeBytecodeChunk(chunk, OP_JUMP, line);
            int backward_jump_offset = loop_start_address - (chunk->count + 2); // +2 for the jump's own operands
            emitShort(chunk, (uint16_t)backward_jump_offset, line);

            // 8. Patch the forward jump to exit the loop
            uint16_t forward_jump_offset = (uint16_t)(chunk->count - (exit_jump_operand_offset + 2));
            patchShort(chunk, exit_jump_operand_offset, forward_jump_offset);

            break;
        }
        case AST_IF: {
            if (!node->left || !node->right) { return; }
            compileExpression(node->left, chunk, line);
            int jump_to_else_or_end_addr = chunk->count;
            writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line);
            emitShort(chunk, 0xFFFF, line);
            compileStatement(node->right, chunk, getLine(node->right));
            if (node->extra) {
                int jump_over_else_addr = chunk->count;
                writeBytecodeChunk(chunk, OP_JUMP, line);
                emitShort(chunk, 0xFFFF, line);
                uint16_t offsetToElse = (uint16_t)(chunk->count - (jump_to_else_or_end_addr + 3));
                patchShort(chunk, jump_to_else_or_end_addr + 1, offsetToElse);
                compileStatement(node->extra, chunk, getLine(node->extra));
                uint16_t offsetToEndOfIf = (uint16_t)(chunk->count - (jump_over_else_addr + 3));
                patchShort(chunk, jump_over_else_addr + 1, offsetToEndOfIf);
            } else {
                uint16_t offsetToEndOfThen = (uint16_t)(chunk->count - (jump_to_else_or_end_addr + 3));
                patchShort(chunk, jump_to_else_or_end_addr + 1, offsetToEndOfThen);
            }
            break;
        }
        case AST_COMPOUND: {
            for (int i = 0; i < node->child_count; i++) {
                if (node->children[i]) {
                    compileStatement(node->children[i], chunk, getLine(node->children[i]));
                }
            }
            break;
        }
        case AST_PROCEDURE_CALL: {
            int call_line = getLine(node);
            for (int i = 0; i < node->child_count; i++) {
                if (node->children[i]) {
                    compileExpression(node->children[i], chunk, getLine(node->children[i]));
                }
            }
            if (node->token && isBuiltin(node->token->value)) {
                char normalized_name[MAX_SYMBOL_LENGTH];
                strncpy(normalized_name, node->token->value, sizeof(normalized_name) - 1);
                normalized_name[sizeof(normalized_name) - 1] = '\0';
                toLowerString(normalized_name);
                int nameIndex = addConstantToChunk(chunk, makeString(normalized_name));
                writeBytecodeChunk(chunk, OP_CALL_BUILTIN, call_line);
                writeBytecodeChunk(chunk, (uint8_t)nameIndex, call_line);
                writeBytecodeChunk(chunk, (uint8_t)node->child_count, call_line);
                if (node->var_type != TYPE_VOID) {
                    writeBytecodeChunk(chunk, OP_POP, call_line);
                }
            } else if (node->token) {
                const char* name_to_lookup = node->token->value;
                Symbol* proc_symbol = lookupSymbolIn(procedure_table, name_to_lookup);
                if (proc_symbol && proc_symbol->is_defined) {
                    int expected_arity = proc_symbol->type_def ? proc_symbol->type_def->child_count : proc_symbol->arity;
                    if (expected_arity != node->child_count) {
                        fprintf(stderr, "L%d: Compiler Error: Routine '%s' expects %d arguments, but %d were given.\n", call_line, name_to_lookup, expected_arity, node->child_count);
                        compiler_had_error = true;
                    } else {
                        writeBytecodeChunk(chunk, OP_CALL, call_line);
                        emitShort(chunk, (uint16_t)proc_symbol->bytecode_address, call_line);
                        writeBytecodeChunk(chunk, (uint8_t)expected_arity, call_line);
                    }
                } else {
                    fprintf(stderr, "L%d: Compiler Error: Undefined or forward-declared procedure '%s'.\n", call_line, name_to_lookup);
                    compiler_had_error = true;
                }
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
            if (node->type >= AST_BINARY_OP || node->type == AST_NOOP) {
                compileExpression(node, chunk, line);
                writeBytecodeChunk(chunk, OP_POP, line);
            } else {
                 fprintf(stderr, "L%d: Compiler WARNING: Unhandled AST node type %s in compileStatement's default case.\n", line, astTypeToString(node->type));
            }
            break;
        }
    }
}

static void compileExpression(AST* node, BytecodeChunk* chunk, int current_line_approx) {
#ifdef DEBUG
    fprintf(stderr, ">>>> DEBUG: compileExpression: ENTERED for Node Type: %s, Token: '%s' <<<<\n",
            node ? astTypeToString(node->type) : "NULL_NODE",
            node && node->token ? (node->token->value ? node->token->value : "N/A_VAL") : "NO_TOKEN");
        fflush(stderr);
#endif
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
        case AST_FORMATTED_EXPR: {
            // First, compile the expression to be formatted. Its value will be on the stack.
            compileExpression(node->left, chunk, getLine(node->left));

            // Now, parse the width and precision from the token
            int width = 0, decimals = -1; // -1 indicates not specified
            if (node->token && node->token->value) {
                sscanf(node->token->value, "%d,%d", &width, &decimals);
            }

            // Emit the format opcode and its operands
            writeBytecodeChunk(chunk, OP_FORMAT_VALUE, line);
            writeBytecodeChunk(chunk, (uint8_t)width, line);
            writeBytecodeChunk(chunk, (uint8_t)decimals, line); // Using -1 (0xFF) for "not specified"
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

            int local_slot = -1;
            if (current_function_compiler) {
                local_slot = resolveLocal(current_function_compiler, varName);
            }

            if (local_slot != -1) {
                // It's a local variable.
                writeBytecodeChunk(chunk, OP_GET_LOCAL, line);
                writeBytecodeChunk(chunk, (uint8_t)local_slot, line);
            } else if (strcasecmp(varName, "quitrequested") == 0) {
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
        // --- NEW: Handle Field and Array Access ---
        case AST_FIELD_ACCESS: {
            // First, put the record object on the stack
            compileExpression(node->left, chunk, getLine(node->left));
            // Then, emit the GET_FIELD opcode with the field name
            int fieldNameIndex = addConstantToChunk(chunk, makeString(node->token->value));
            writeBytecodeChunk(chunk, OP_GET_FIELD, line);
            writeBytecodeChunk(chunk, (uint8_t)fieldNameIndex, line);
            break;
        }
        case AST_ARRAY_ACCESS: {
            // First, put the array object on the stack
            compileExpression(node->left, chunk, getLine(node->left));
            
            int dimension_count = node->child_count;
            if (dimension_count > 255) {
                 fprintf(stderr, "L%d: Compiler error: Maximum number of array dimensions (255) exceeded for access.\n", line);
                 compiler_had_error = true;
                 dimension_count = 0; // Prevent further processing
            }

            for (int i = 0; i < dimension_count; i++) {
                if (node->children[i]) {
                    compileExpression(node->children[i], chunk, getLine(node->children[i]));
                } else {
                    fprintf(stderr, "L%d: Compiler error: Array access node has a NULL index expression at index %d.\n", line, i);
                    compiler_had_error = true;
                    // Push a dummy value to avoid corrupting the stack
                    int dummyIdx = addConstantToChunk(chunk, makeInt(0));
                    writeBytecodeChunk(chunk, OP_CONSTANT, line);
                    writeBytecodeChunk(chunk, (uint8_t)dummyIdx, line);
                }
            }
            
            // Finally, emit the GET_ELEMENT opcode followed by the dimension count
            writeBytecodeChunk(chunk, OP_GET_ELEMENT, line);
            writeBytecodeChunk(chunk, (uint8_t)dimension_count, line);
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
                    case TOKEN_SHL:           writeBytecodeChunk(chunk, OP_SHL, line); break; 
                    case TOKEN_SHR:           writeBytecodeChunk(chunk, OP_SHR, line); break;
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
            } else if (isBuiltin(functionName)) {
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
                    if (func_symbol->type == TYPE_VOID) {
                        fprintf(stderr, "L%d: Compiler Error: Procedure '%s' cannot be used as a function in an expression(compileExpression).\n", line, original_display_name);
                        compiler_had_error = true;
                        for(uint8_t i=0; i<arg_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                        writeBytecodeChunk(chunk, OP_CONSTANT, line);
                        writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk, makeNil()), line);
                    } else {
                        int expected_arity = func_symbol->type_def ? func_symbol->type_def->child_count : 0;
                        if (expected_arity != arg_count) {
                            fprintf(stderr, "L%d: Compiler Error: Function '%s' expects %d arguments, but %d were given.\n", line, original_display_name, expected_arity, arg_count);
                            compiler_had_error = true;
                            for(uint8_t i=0; i<arg_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                            writeBytecodeChunk(chunk, OP_CONSTANT, line);
                            writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk, makeNil()), line);
                        } else {
                            writeBytecodeChunk(chunk, OP_CALL, line);
                            emitShort(chunk, (uint16_t)func_symbol->bytecode_address, line);
                            writeBytecodeChunk(chunk, arg_count, line);
                        }
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

static void compileStoreForLValue(AST* container, BytecodeChunk* chunk, int line) {
    if (!container) {
        fprintf(stderr, "L%d: Compiler internal error: compileStoreForLValue called with null container.\n", line);
        return;
    }

    switch (container->type) {
        case AST_VARIABLE:
            int local_slot = -1;
            if (current_function_compiler) {
                local_slot = resolveLocal(current_function_compiler, container->token->value);
            }

            if (local_slot != -1) {
                writeBytecodeChunk(chunk, OP_SET_LOCAL, line);
                writeBytecodeChunk(chunk, (uint8_t)local_slot, line);
            } else {
                // Fallback to global
                int nameIndex = addConstantToChunk(chunk, makeString(container->token->value));
                writeBytecodeChunk(chunk, OP_SET_GLOBAL, line);
                writeBytecodeChunk(chunk, (uint8_t)nameIndex, line);
            }
            break;

        case AST_ARRAY_ACCESS:
            // The modified element is on the stack. Now we store it in the array.
            // Stack: [ModifiedElement]
            // We need to get it to: [Array, Index1, Index2, ..., ModifiedElement] for OP_SET_ELEMENT.

            // 1. Get the container array.
            compileExpression(container->left, chunk, getLine(container->left));
            // Stack: [ModifiedElement, Array]
            // 2. Swap them.
            writeBytecodeChunk(chunk, OP_SWAP, line);
            // Stack: [Array, ModifiedElement]
            
            // 3. Get all the indices and push them onto the stack.
            int dimension_count = container->child_count;
            for (int i = 0; i < dimension_count; i++) {
                compileExpression(container->children[i], chunk, getLine(container->children[i]));
                // Stack grows: [Array, ModifiedElement, Index1], [Array, ModifiedElement, Index1, Index2], ...
            }

            // 4. We now need to get the ModifiedElement to the top of the stack.
            // This requires a series of swaps. For N indices, we need N swaps.
            // Stack is: [Array, ModifiedElement, Index1, ..., IndexN]
            for (int i = 0; i < dimension_count; i++) {
                 writeBytecodeChunk(chunk, OP_SWAP, line);
            }
            // After 1 swap: [Array, Index1, ModifiedElement, Index2, ...]
            // After N swaps: [Array, Index1, ..., IndexN, ModifiedElement]
            
            // 5. Emit the SET_ELEMENT opcode with the dimension count
            writeBytecodeChunk(chunk, OP_SET_ELEMENT, line);
            writeBytecodeChunk(chunk, (uint8_t)dimension_count, line);

            // Now the modified array is on the stack. Recursively store IT back.
            compileStoreForLValue(container->left, chunk, line);
            break;
        case AST_FIELD_ACCESS:
            // The modified field value is on the stack. Now we store it in the record.
            // Stack: [ModifiedFieldValue]
            // We need to get it to: [Record, ModifiedFieldValue] for OP_SET_FIELD.

            // 1. Get the container record.
            compileExpression(container->left, chunk, getLine(container->left));
            // Stack: [ModifiedFieldValue, Record]
            // 2. Swap them.
            writeBytecodeChunk(chunk, OP_SWAP, line);
            // Stack: [Record, ModifiedFieldValue]
            int fieldNameIndex = addConstantToChunk(chunk, makeString(container->token->value));
            writeBytecodeChunk(chunk, OP_SET_FIELD, line);
            writeBytecodeChunk(chunk, (uint8_t)fieldNameIndex, line);

            // Now the modified record is on the stack. Recursively store IT back.
            compileStoreForLValue(container->left, chunk, line);
            break;

        default:
            fprintf(stderr, "L%d: Compiler error: Cannot store back into LValue container of type %s.\n", line, astTypeToString(container->type));
            // Pop the value that was meant to be stored to prevent stack corruption.
            writeBytecodeChunk(chunk, OP_POP, line);
            break;
    }
}

