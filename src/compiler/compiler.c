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
    bool is_ref;
} CompilerLocal;

typedef struct {
    CompilerLocal locals[MAX_GLOBALS]; // Re-use MAX_GLOBALS for max locals per function
    int local_count;
    int scope_depth;
    const char* name;
} FunctionCompilerState;

FunctionCompilerState* current_function_compiler = NULL;


// --- Forward Declarations for Recursive Compilation ---
static void compileNode(AST* node, BytecodeChunk* chunk, int current_line_approx);
static void compileRValue(AST* node, BytecodeChunk* chunk, int current_line_approx);
static void compileStatement(AST* node, BytecodeChunk* chunk, int current_line_approx);
static void compileLValue(AST* node, BytecodeChunk* chunk, int current_line_approx);
static void compileDefinedFunction(AST* func_decl_node, BytecodeChunk* chunk, int line);

// --- Global/Module State for Compiler ---
// For mapping global variable names to an index during this compilation pass.
// This is a simplified approach for global variables.
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
    fc->name = NULL;
}

static void addLocal(FunctionCompilerState* fc, const char* name, int line, bool is_ref) {
    if (fc->local_count >= MAX_GLOBALS) {
        fprintf(stderr, "L%d: Compiler error: Too many local variables in one function.\n", line);
        compiler_had_error = true;
        return;
    }
    CompilerLocal* local = &fc->locals[fc->local_count++];
    local->name = strdup(name);
    local->depth = fc->scope_depth;
    local->is_ref = is_ref;
}

static int resolveLocal(FunctionCompilerState* fc, const char* name) {
    if (!fc) return -1;
    for (int i = fc->local_count - 1; i >= 0; i--) {
        CompilerLocal* local = &fc->locals[i];
        if (strcasecmp(name, local->name) == 0) {
            return i;
        }
    }
    return -1;
}

// Helper to add a constant during compilation
void addCompilerConstant(const char* name_original_case, Value value, int line) {
    if (compilerConstantCount >= MAX_COMPILER_CONSTANTS) {
        fprintf(stderr, "L%d: Compiler error: Too many compile-time constants.\n", line);
        freeValue(&value);
        compiler_had_error = true;
        return;
    }
    char canonical_name[MAX_SYMBOL_LENGTH];
    strncpy(canonical_name, name_original_case, sizeof(canonical_name) - 1);
    canonical_name[sizeof(canonical_name) - 1] = '\0';
    toLowerString(canonical_name);
    for (int i = 0; i < compilerConstantCount; i++) {
        if (compilerConstants[i].name && strcmp(compilerConstants[i].name, canonical_name) == 0) {
            fprintf(stderr, "L%d: Compiler warning: Constant '%s' redefined.\n", line, name_original_case);
            freeValue(&compilerConstants[i].value);
            compilerConstants[i].value = makeCopyOfValue(&value);
            freeValue(&value);
            return;
        }
    }
    compilerConstants[compilerConstantCount].name = strdup(canonical_name);
    compilerConstants[compilerConstantCount].value = value;
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

static void compileLValue(AST* node, BytecodeChunk* chunk, int current_line_approx) {
    if (!node) return;
    int line = getLine(node);
    if (line <= 0) line = current_line_approx;

    switch(node->type) {
        case AST_VARIABLE: {
            // This case remains the same as before...
            if (!node->token || !node->token->value) { /* error */ return; }
            const char* varName = node->token->value;
            int local_slot = -1;
            bool is_ref = false;

            if (current_function_compiler) {
                if (current_function_compiler->name && strcasecmp(varName, current_function_compiler->name) == 0) {
                    local_slot = resolveLocal(current_function_compiler, current_function_compiler->name);
                } else {
                    local_slot = resolveLocal(current_function_compiler, varName);
                }

                if (local_slot != -1) {
                    is_ref = current_function_compiler->locals[local_slot].is_ref;
                }
            }

            if (local_slot != -1) {
                if (is_ref) {
                    writeBytecodeChunk(chunk, OP_GET_LOCAL, line);
                    writeBytecodeChunk(chunk, (uint8_t)local_slot, line);
                } else {
                    writeBytecodeChunk(chunk, OP_GET_LOCAL_ADDRESS, line);
                    writeBytecodeChunk(chunk, (uint8_t)local_slot, line);
                }
            } else {
                int nameIndex = addConstantToChunk(chunk, makeString(varName));
                writeBytecodeChunk(chunk, OP_GET_GLOBAL_ADDRESS, line);
                writeBytecodeChunk(chunk, (uint8_t)nameIndex, line);
            }
            break;
        }
        case AST_FIELD_ACCESS: {
            // Recursively compile the L-Value of the base (e.g., myRec or p^)
            compileLValue(node->left, chunk, getLine(node->left));

            // Now, get the address of the specific field.
            int fieldNameIndex = addConstantToChunk(chunk, makeString(node->token->value));
            writeBytecodeChunk(chunk, OP_GET_FIELD_ADDRESS, line);
            writeBytecodeChunk(chunk, (uint8_t)fieldNameIndex, line);
            break;
        }
        case AST_ARRAY_ACCESS: {
            // Get the L-Value of the base array (e.g., address of 'myArray' or value of 'p')
            compileLValue(node->left, chunk, getLine(node->left));

            // Compile all index expressions. Their values will be on the stack.
            for (int i = 0; i < node->child_count; i++) {
                compileRValue(node->children[i], chunk, getLine(node->children[i]));
            }
            
            // Now, get the address of the specific element.
            writeBytecodeChunk(chunk, OP_GET_ELEMENT_ADDRESS, line);
            writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
            break;
        }
        case AST_DEREFERENCE: {
            // The L-Value of p^ is the address stored inside p.
            // So we just need the R-Value of p.
            compileRValue(node->left, chunk, getLine(node->left));
            break;
        }
        default:
            fprintf(stderr, "L%d: Compiler error: Invalid expression cannot be used as a variable reference (L-Value).\n", line);
            compiler_had_error = true;
            break;
    }
}

bool compileASTToBytecode(AST* rootNode, BytecodeChunk* outputChunk) {
    if (!rootNode || !outputChunk) return false;
    initBytecodeChunk(outputChunk);
    compilerGlobalCount = 0;
    compiler_had_error = false;
    current_function_compiler = NULL;

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
            if (current_function_compiler == NULL) { // Global variables
                AST* type_specifier_node = node->right;

                // First, resolve the type alias if one exists.
                AST* actual_type_def_node = type_specifier_node;
                if (actual_type_def_node && actual_type_def_node->type == AST_TYPE_REFERENCE) {
                    AST* resolved_node = lookupType(actual_type_def_node->token->value);
                    if (resolved_node) {
                        actual_type_def_node = resolved_node; // This now points to the AST_ARRAY_TYPE node
                    } else {
                        fprintf(stderr, "L%d: Compiler error: User-defined type '%s' not found.\n", getLine(actual_type_def_node), actual_type_def_node->token->value);
                        compiler_had_error = true;
                        break;
                    }
                }

                if (!actual_type_def_node) {
                    fprintf(stderr, "L%d: Compiler error: Could not determine type definition for a variable declaration.\n", getLine(node));
                    compiler_had_error = true;
                    break;
                }

                // Now, handle based on the *actual* resolved type definition
                for (int i = 0; i < node->child_count; i++) {
                    AST* varNameNode = node->children[i];
                    if (varNameNode && varNameNode->token) {
                        int var_name_idx = addConstantToChunk(chunk, makeString(varNameNode->token->value));
                        writeBytecodeChunk(chunk, OP_DEFINE_GLOBAL, getLine(varNameNode));
                        writeBytecodeChunk(chunk, (uint8_t)var_name_idx, getLine(varNameNode));
                        writeBytecodeChunk(chunk, (uint8_t)node->var_type, getLine(varNameNode)); // The overall type (e.g., TYPE_ARRAY)

                        if (actual_type_def_node->type == AST_ARRAY_TYPE) {
                            // This block now correctly handles both inline and aliased arrays.
                            int dimension_count = actual_type_def_node->child_count;
                            if (dimension_count > 255) {
                                fprintf(stderr, "L%d: Compiler error: Maximum array dimensions (255) exceeded.\n", getLine(varNameNode));
                                compiler_had_error = true;
                                break;
                            }
                            writeBytecodeChunk(chunk, (uint8_t)dimension_count, getLine(varNameNode));

                            for (int dim = 0; dim < dimension_count; dim++) {
                                AST* subrange = actual_type_def_node->children[dim];
                                if (subrange && subrange->type == AST_SUBRANGE) {
                                    Value lower_b = evaluateCompileTimeValue(subrange->left);
                                    Value upper_b = evaluateCompileTimeValue(subrange->right);
                                    writeBytecodeChunk(chunk, addConstantToChunk(chunk, lower_b), getLine(varNameNode));
                                    writeBytecodeChunk(chunk, addConstantToChunk(chunk, upper_b), getLine(varNameNode));
                                } else {
                                    fprintf(stderr, "L%d: Compiler error: Malformed array definition for '%s'.\n", getLine(varNameNode), varNameNode->token->value);
                                    compiler_had_error = true;
                                    writeBytecodeChunk(chunk, 0, getLine(varNameNode));
                                    writeBytecodeChunk(chunk, 0, getLine(varNameNode));
                                }
                            }

                            AST* elem_type = actual_type_def_node->right;
                            writeBytecodeChunk(chunk, (uint8_t)elem_type->var_type, getLine(varNameNode));
                            const char* elem_type_name = (elem_type && elem_type->token) ? elem_type->token->value : "";
                            writeBytecodeChunk(chunk, addConstantToChunk(chunk, makeString(elem_type_name)), getLine(varNameNode));

                        } else {
                            // This handles simple types, records, and other non-array aliased types.
                            const char* type_name = (type_specifier_node && type_specifier_node->token) ? type_specifier_node->token->value : "";
                            writeBytecodeChunk(chunk, addConstantToChunk(chunk, makeString(type_name)), getLine(varNameNode));
                        }
                        resolveGlobalVariableIndex(chunk, varNameNode->token->value, getLine(varNameNode));
                    }
                }
            }
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
    fc.name = func_name;
    
    int func_bytecode_start_address = chunk->count;

    Symbol* proc_symbol = lookupSymbolIn(procedure_table, func_name);
    if (!proc_symbol) {
        current_function_compiler = NULL;
        return;
    }
    proc_symbol->bytecode_address = func_bytecode_start_address;
    proc_symbol->is_defined = true;
    
    int return_value_slot = -1;

    // Step 1: Add parameters to the local scope FIRST. They will occupy slots 0, 1, ...
    if (func_decl_node->children) {
        for (int i = 0; i < func_decl_node->child_count; i++) {
            AST* param_group_node = func_decl_node->children[i];
            if (param_group_node && param_group_node->type == AST_VAR_DECL) {
                bool is_var_param = param_group_node->by_ref;
                for (int j = 0; j < param_group_node->child_count; j++) {
                    AST* param_name_node = param_group_node->children[j];
                    if (param_name_node && param_name_node->token) {
                        addLocal(&fc, param_name_node->token->value, getLine(param_name_node), is_var_param);
                    }
                }
            }
        }
    }
    proc_symbol->arity = fc.local_count;

    // Step 2: If it's a function, add its name as a local variable for the return value.
    if (func_decl_node->type == AST_FUNCTION_DECL) {
        addLocal(&fc, func_name, line, false);
        return_value_slot = fc.local_count - 1; // Its index is the last one added.
    }
    
    // Step 3: Add all other local variables.
    uint8_t local_var_count = 0;
    AST* blockNode = (func_decl_node->type == AST_PROCEDURE_DECL) ? func_decl_node->right : func_decl_node->extra;
    if (blockNode && blockNode->type == AST_BLOCK && blockNode->child_count > 0 && blockNode->children[0]->type == AST_COMPOUND) {
        AST* decls = blockNode->children[0];
        for (int i = 0; i < decls->child_count; i++) {
            if (decls->children[i] && decls->children[i]->type == AST_VAR_DECL) {
                AST* var_decl_group = decls->children[i];
                for (int j = 0; j < var_decl_group->child_count; j++) {
                    AST* var_name_node = var_decl_group->children[j];
                    if (var_name_node && var_name_node->token) {
                        addLocal(&fc, var_name_node->token->value, getLine(var_name_node), false);
                        local_var_count++;
                    }
                }
            }
        }
    }
    
    proc_symbol->locals_count = local_var_count + (func_decl_node->type == AST_FUNCTION_DECL ? 1 : 0);
    
    // Step 4: Compile the function body.
    if (blockNode) {
        compileNode(blockNode, chunk, getLine(blockNode));
    }

    // Step 5: Emit the return instruction.
    if (func_decl_node->type == AST_FUNCTION_DECL) {
        writeBytecodeChunk(chunk, OP_GET_LOCAL, line);
        writeBytecodeChunk(chunk, (uint8_t)return_value_slot, line);
    } else {
        int nil_const_idx = addConstantToChunk(chunk, makeNil());
        writeBytecodeChunk(chunk, OP_CONSTANT, line);
        writeBytecodeChunk(chunk, (uint8_t)nil_const_idx, line);
    }
    writeBytecodeChunk(chunk, OP_RETURN, line);
    
    // Step 6: Cleanup.
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
                compileRValue(node->children[i], chunk, getLine(node->children[i]));
            }
            writeBytecodeChunk(chunk, OP_WRITE_LN, line);
            writeBytecodeChunk(chunk, (uint8_t)argCount, line);
            break;
        }
        case AST_WHILE: {
            int loopStart = chunk->count; // Mark address for the top of the loop (for condition re-check)

            // 1. Compile the condition expression
            compileRValue(node->left, chunk, line);

            // 2. Emit a conditional jump that skips the body if the condition is false
            writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line);
            int exitJumpOffset = chunk->count; // Remember where the jump operand is
            emitShort(chunk, 0xFFFF, line);     // Write a placeholder jump distance

            // 3. Compile the loop body
            compileStatement(node->right, chunk, getLine(node->right));

            // 4. Emit an unconditional jump back to the top of the loop
            writeBytecodeChunk(chunk, OP_JUMP, line);
            int backwardJumpOffset = loopStart - (chunk->count + 2); // +2 for this jump's own operands
            emitShort(chunk, (uint16_t)backwardJumpOffset, line);

            // 5. Patch the original conditional jump to point to the instruction right after the loop
            uint16_t forwardJumpOffset = (uint16_t)(chunk->count - (exitJumpOffset + 2));
            patchShort(chunk, exitJumpOffset, forwardJumpOffset);

            break;
        }
        case AST_REPEAT: {
            int loopStart = chunk->count;
            // The body of the loop is the left child (which is an AST_COMPOUND)
            if (node->left) {
                compileStatement(node->left, chunk, getLine(node->left));
            }
            // The until condition is the right child
            if (node->right) {
                compileRValue(node->right, chunk, getLine(node->right));
            } else {
                // An until without a condition is a syntax error, but we'll treat it as `until false` (infinite loop)
                int falseConstIdx = addConstantToChunk(chunk, makeBoolean(false));
                writeBytecodeChunk(chunk, OP_CONSTANT, line);
                writeBytecodeChunk(chunk, (uint8_t)falseConstIdx, line);
            }
            // Jump back to the start if the condition is false
            writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line);
            // The offset is negative to jump backwards
            int backward_jump_offset = loopStart - (chunk->count + 2);
            emitShort(chunk, (uint16_t)backward_jump_offset, line);
            break;
        }
        case AST_CASE: {
            // NOTE: Full compilation for CASE statements is complex.
            // This placeholder prevents the warning but does not implement functionality.
            // We compile the main expression and pop it to keep the stack balanced.
            fprintf(stderr, "L%d: Compiler NOTE: CASE statement compilation is not yet implemented. Statement will be ignored.\n", line);
            if (node->left) {
                compileRValue(node->left, chunk, line);
                writeBytecodeChunk(chunk, OP_POP, line);
            }
            break;
        }
        // Add this case as well
        case AST_READLN: {
            // `readln` arguments are all L-Values.
            // The VM builtin expects pointers to the Value structs of these variables.
            // compileLValue does exactly this: pushes the address of the variable onto the stack.
            for (int i = 0; i < node->child_count; i++) {
                AST* arg_node = node->children[i];
                compileLValue(arg_node, chunk, getLine(arg_node));
            }

            // Now, call the built-in `readln` function using the generic OP_CALL_BUILTIN.
            int nameIndex = addConstantToChunk(chunk, makeString("readln"));
            writeBytecodeChunk(chunk, OP_CALL_BUILTIN, line);
            writeBytecodeChunk(chunk, (uint8_t)nameIndex, line);
            writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);

            // `readln` is a procedure, so it doesn't leave a value on the stack. No OP_POP is needed.
            break;
        }
        case AST_WRITE: {
            int argCount = node->child_count;
            for (int i = 0; i < argCount; i++) {
                compileRValue(node->children[i], chunk, getLine(node->children[i]));
            }
            writeBytecodeChunk(chunk, OP_WRITE, line);
            writeBytecodeChunk(chunk, (uint8_t)argCount, line);
            break;
        }
        case AST_ASSIGN: {
            AST* lvalue = node->left;
            AST* rvalue = node->right;

            compileRValue(rvalue, chunk, getLine(rvalue));

            if (current_function_compiler && lvalue->type == AST_VARIABLE &&
                strcasecmp(lvalue->token->value, current_function_compiler->name) == 0) {
                
                int return_slot = resolveLocal(current_function_compiler, current_function_compiler->name);
                if (return_slot != -1) {
                    writeBytecodeChunk(chunk, OP_SET_LOCAL, line);
                    writeBytecodeChunk(chunk, (uint8_t)return_slot, line);
                    writeBytecodeChunk(chunk, OP_POP, line); // Pop the RHS value after setting the local
                } else {
                    fprintf(stderr, "L%d: Compiler internal error: could not resolve slot for function return value '%s'.\n", line, current_function_compiler->name);
                    compiler_had_error = true;
                }
            } else {
                compileLValue(lvalue, chunk, getLine(lvalue));
                writeBytecodeChunk(chunk, OP_SWAP, line);
                writeBytecodeChunk(chunk, OP_SET_INDIRECT, line);
            }
            break;
        }
        case AST_FOR_TO:
        case AST_FOR_DOWNTO: {
            bool is_downto = node->type == AST_FOR_DOWNTO;
            AST* var_node = node->children[0];
            AST* start_node = node->left;
            AST* end_node = node->right;
            AST* body_node = node->extra;

            // --- START: MODIFIED LOGIC ---
            int var_slot = -1;
            int var_name_idx = -1;
            
            // Check if the loop variable is a local or global.
            if (current_function_compiler) {
                var_slot = resolveLocal(current_function_compiler, var_node->token->value);
            }

            if (var_slot == -1) { // Not found as a local, treat as global
                var_name_idx = addConstantToChunk(chunk, makeString(var_node->token->value));
            }

            // 1. Compile and set the initial value
            compileRValue(start_node, chunk, getLine(start_node));
            if (var_slot != -1) {
                writeBytecodeChunk(chunk, OP_SET_LOCAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_slot, line);
            } else {
                writeBytecodeChunk(chunk, OP_SET_GLOBAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_name_idx, line);
            }

            int loopStart = chunk->count;

            // 2. Loop condition check
            if (var_slot != -1) {
                writeBytecodeChunk(chunk, OP_GET_LOCAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_slot, line);
            } else {
                writeBytecodeChunk(chunk, OP_GET_GLOBAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_name_idx, line);
            }
            compileRValue(end_node, chunk, getLine(end_node));
            writeBytecodeChunk(chunk, is_downto ? OP_LESS_EQUAL : OP_GREATER_EQUAL, line);

            // 3. Jump out of the loop if condition is met
            writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line);
            int exitJump = chunk->count;
            emitShort(chunk, 0xFFFF, line);

            // 4. Compile the loop body
            compileStatement(body_node, chunk, getLine(body_node));

            // 5. Increment/Decrement loop variable
            if (var_slot != -1) {
                writeBytecodeChunk(chunk, OP_GET_LOCAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_slot, line);
            } else {
                writeBytecodeChunk(chunk, OP_GET_GLOBAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_name_idx, line);
            }
            int one_const_idx = addConstantToChunk(chunk, makeInt(1));
            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, (uint8_t)one_const_idx, line);
            writeBytecodeChunk(chunk, is_downto ? OP_SUBTRACT : OP_ADD, line);

            if (var_slot != -1) {
                writeBytecodeChunk(chunk, OP_SET_LOCAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_slot, line);
            } else {
                writeBytecodeChunk(chunk, OP_SET_GLOBAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_name_idx, line);
            }

            // 6. Jump back to the top of the loop
            writeBytecodeChunk(chunk, OP_JUMP, line);
            int backward_jump_offset = loopStart - (chunk->count + 2);
            emitShort(chunk, (uint16_t)backward_jump_offset, line);

            // 7. Patch the exit jump
            patchShort(chunk, exitJump, (uint16_t)(chunk->count - (exitJump + 2)));
            // --- END MODIFICATION ---
            break;
        }
        case AST_IF: {
            if (!node->left || !node->right) { return; }
            compileRValue(node->left, chunk, line);
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
        case AST_PROCEDURE_CALL: {
            // This is the correct handler for procedure calls as statements.
            // It's nearly identical to the R-Value version, but it pops function results.
            int call_line = getLine(node);
            const char* calleeName = node->token->value;
            Symbol* proc_symbol = lookupSymbolIn(procedure_table, calleeName);

            // Compile arguments first
            for (int i = 0; i < node->child_count; i++) {
                AST* arg_node = node->children[i];
                bool is_var_param = false;

                if (calleeName && (strcasecmp(calleeName, "new") == 0 || strcasecmp(calleeName, "dispose") == 0)) {
                    is_var_param = true;
                } else if (proc_symbol && proc_symbol->type_def && i < proc_symbol->type_def->child_count) {
                    AST* param_node = proc_symbol->type_def->children[i];
                    if (param_node && param_node->by_ref) {
                        is_var_param = true;
                    }
                }

                if (is_var_param) {
                    compileLValue(arg_node, chunk, getLine(arg_node));
                } else {
                    compileRValue(arg_node, chunk, getLine(arg_node));
                }
            }

            if (isBuiltin(calleeName)) {
                char normalized_name[MAX_SYMBOL_LENGTH];
                strncpy(normalized_name, calleeName, sizeof(normalized_name) - 1);
                normalized_name[sizeof(normalized_name) - 1] = '\0';
                toLowerString(normalized_name);
                int nameIndex = addConstantToChunk(chunk, makeString(normalized_name));
                writeBytecodeChunk(chunk, OP_CALL_BUILTIN, call_line);
                writeBytecodeChunk(chunk, (uint8_t)nameIndex, call_line);
                writeBytecodeChunk(chunk, (uint8_t)node->child_count, call_line);
                
                // If a function is called as a statement, pop its return value
                if (getBuiltinType(calleeName) == BUILTIN_TYPE_FUNCTION) {
                    writeBytecodeChunk(chunk, OP_POP, call_line);
                }
            } else {
                if (proc_symbol && proc_symbol->is_defined) {
                    writeBytecodeChunk(chunk, OP_CALL, call_line);
                    emitShort(chunk, (uint16_t)proc_symbol->bytecode_address, call_line);
                    writeBytecodeChunk(chunk, (uint8_t)node->child_count, call_line);
                    
                    // If it's a user function, pop its result
                    if (proc_symbol->type != TYPE_VOID) {
                        writeBytecodeChunk(chunk, OP_POP, call_line);
                    }
                } else {
                    fprintf(stderr, "L%d: Compiler Error: Undefined or forward-declared procedure '%s'.\n", call_line, calleeName);
                    compiler_had_error = true;
                }
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
        default: {
            // This case should now only be hit for unhandled statement types, not expressions.
            fprintf(stderr, "L%d: Compiler WARNING: Unhandled AST node type %s in compileStatement's default case.\n", line, astTypeToString(node->type));
            break;
        }
    }
}

static void compileRValue(AST* node, BytecodeChunk* chunk, int current_line_approx) {
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
            compileRValue(node->left, chunk, getLine(node->left));

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
        case AST_NIL: {
            int constIndex = addConstantToChunk(chunk, makeNil());
            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, (uint8_t)constIndex, line);
            break;
        }
        case AST_DEREFERENCE: {
            // A dereference on the right-hand side means we get the value.
            // First, get the pointer value itself onto the stack by compiling the l-value.
            compileRValue(node->left, chunk, getLine(node->left));
            // Then, use GET_INDIRECT to replace the pointer with the value it points to.
            writeBytecodeChunk(chunk, OP_GET_INDIRECT, line);
            break;
        }
        case AST_VARIABLE: {
            if (!node_token || !node_token->value) { /* error */ break; }
            const char* varName = node_token->value;

            int local_slot = -1;
            bool is_ref = false;
            if (current_function_compiler) {
                // Check if it's an assignment to the function name itself
                if (current_function_compiler->name && strcasecmp(varName, current_function_compiler->name) == 0) {
                    local_slot = resolveLocal(current_function_compiler, current_function_compiler->name);
                } else {
                    local_slot = resolveLocal(current_function_compiler, varName);
                }
                
                if (local_slot != -1) {
                    is_ref = current_function_compiler->locals[local_slot].is_ref;
                }
            }
            
            if (local_slot != -1) {
                writeBytecodeChunk(chunk, OP_GET_LOCAL, line);
                writeBytecodeChunk(chunk, (uint8_t)local_slot, line);
                if (is_ref) {
                    writeBytecodeChunk(chunk, OP_GET_INDIRECT, line);
                }
            } else {
                Value* const_val_ptr = findCompilerConstant(varName);
                if (const_val_ptr) {
                    int constIndex = addConstantToChunk(chunk, makeCopyOfValue(const_val_ptr));
                    writeBytecodeChunk(chunk, OP_CONSTANT, line);
                    writeBytecodeChunk(chunk, (uint8_t)constIndex, line);
                } else {
                    int nameIndex = addConstantToChunk(chunk, makeString(varName));
                    writeBytecodeChunk(chunk, OP_GET_GLOBAL, line);
                    writeBytecodeChunk(chunk, (uint8_t)nameIndex, line);
                }
            }
            break;
        }
        case AST_FIELD_ACCESS:
        case AST_ARRAY_ACCESS: {
            compileLValue(node, chunk, line);
            writeBytecodeChunk(chunk, OP_GET_INDIRECT, line);
            break;
        }
        case AST_BINARY_OP: {
            compileRValue(node->left, chunk, getLine(node->left));
            compileRValue(node->right, chunk, getLine(node->right));
            if (node_token) { // node_token is the operator
                switch (node_token->type) {
                    case TOKEN_PLUS:          writeBytecodeChunk(chunk, OP_ADD, line); break;
                    case TOKEN_MINUS:         writeBytecodeChunk(chunk, OP_SUBTRACT, line); break;
                    case TOKEN_MUL:           writeBytecodeChunk(chunk, OP_MULTIPLY, line); break;
                    case TOKEN_SLASH:         writeBytecodeChunk(chunk, OP_DIVIDE, line); break;
                    case TOKEN_INT_DIV:       writeBytecodeChunk(chunk, OP_INT_DIV, line); break;
                    case TOKEN_MOD:           writeBytecodeChunk(chunk, OP_MOD, line); break;
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
            compileRValue(node->left, chunk, getLine(node->left)); // Operand
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
        case AST_BOOLEAN: {
            if (!node_token && node->i_val == 0 && node->var_type == TYPE_BOOLEAN) {
            } else if (!node_token) {
                fprintf(stderr, "L%d: Compiler error: AST_BOOLEAN node missing token.\n", line);
                compiler_had_error = true;
                break;
            }
            Value boolConst = makeBoolean(node->i_val != 0);
            int constIndex = addConstantToChunk(chunk, boolConst);
            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, (uint8_t)constIndex, line);
            break;
        }

        case AST_PROCEDURE_CALL: {
            int line = getLine(node);
            if (line <= 0) line = current_line_approx;

            const char* functionName = NULL;
            bool isCallQualified = false;

            if (node->left &&
                node->left->type == AST_VARIABLE &&
                node->left->token && node->left->token->value &&
                node->token && node->token->value && node->token->type == TOKEN_IDENTIFIER) {
                functionName = node->token->value;
                isCallQualified = true;
            } else if (node->token && node->token->value && node->token->type == TOKEN_IDENTIFIER) {
                functionName = node->token->value;
                isCallQualified = false;
            } else {
                fprintf(stderr, "L%d: Compiler error: Invalid callee in AST_PROCEDURE_CALL (expression).\n", line);
                compiler_had_error = true;
                writeBytecodeChunk(chunk, OP_CONSTANT, line);
                writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk, makeNil()), line);
                break;
            }

            Symbol* func_symbol = functionName ? lookupSymbolIn(procedure_table, functionName) : NULL;
            
            if (node->child_count > 0 && node->children) {
                for (int i = 0; i < node->child_count; i++) {
                    AST* arg_node = node->children[i];
                    if (!arg_node) continue;
                    
                    bool is_var_param = false;
                    
                    if (functionName && (strcasecmp(functionName, "new") == 0 || strcasecmp(functionName, "dispose") == 0)) {
                        is_var_param = true;
                    }
                    else if (func_symbol && func_symbol->type_def && i < func_symbol->type_def->child_count) {
                        AST* param_group_node = func_symbol->type_def->children[i];
                        if (param_group_node && param_group_node->type == AST_VAR_DECL && param_group_node->by_ref) {
                            is_var_param = true;
                        }
                    }

                    if (is_var_param) {
                        compileLValue(arg_node, chunk, getLine(arg_node));
                    } else {
                        compileRValue(arg_node, chunk, getLine(arg_node));
                    }
                }
            }
            
            if (isBuiltin(functionName)) {
                BuiltinRoutineType type = getBuiltinType(functionName);
                if (type == BUILTIN_TYPE_PROCEDURE) {
                    fprintf(stderr, "L%d: Compiler Error: Built-in procedure '%s' cannot be used as a function in an expression.\n", line, functionName);
                    compiler_had_error = true;
                    for(uint8_t i = 0; i < node->child_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                    writeBytecodeChunk(chunk, OP_CONSTANT, line);
                    writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk, makeNil()), line);
                } else if (type == BUILTIN_TYPE_FUNCTION) {
                    char normalized_name[MAX_SYMBOL_LENGTH];
                    strncpy(normalized_name, functionName, sizeof(normalized_name) - 1);
                    normalized_name[sizeof(normalized_name) - 1] = '\0';
                    toLowerString(normalized_name);
                    int nameIndex = addConstantToChunk(chunk, makeString(normalized_name));
                    writeBytecodeChunk(chunk, OP_CALL_BUILTIN, line);
                    writeBytecodeChunk(chunk, (uint8_t)nameIndex, line);
                    writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
                } else {
                     fprintf(stderr, "L%d: Compiler Error: '%s' is not a recognized built-in function for expression context.\n", line, functionName);
                    compiler_had_error = true;
                    for(uint8_t i = 0; i < node->child_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                    writeBytecodeChunk(chunk, OP_CONSTANT, line);
                    writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk, makeNil()), line);
                }
            } else { // User-defined function call
                char lookup_name[MAX_SYMBOL_LENGTH * 2 + 2];
                char original_display_name[MAX_SYMBOL_LENGTH * 2 + 2];
                if (isCallQualified) {
                    snprintf(original_display_name, sizeof(original_display_name), "%s.%s", node->left->token->value, functionName);
                    char unit_name_lower[MAX_SYMBOL_LENGTH], func_name_lower[MAX_SYMBOL_LENGTH];
                    strncpy(unit_name_lower, node->left->token->value, sizeof(unit_name_lower)-1); unit_name_lower[sizeof(unit_name_lower)-1] = '\0';
                    strncpy(func_name_lower, functionName, sizeof(func_name_lower)-1); func_name_lower[sizeof(func_name_lower)-1] = '\0';
                    toLowerString(unit_name_lower); toLowerString(func_name_lower);
                    snprintf(lookup_name, sizeof(lookup_name), "%s.%s", unit_name_lower, func_name_lower);
                } else {
                    strncpy(original_display_name, functionName, sizeof(original_display_name)-1); original_display_name[sizeof(original_display_name)-1] = '\0';
                    strncpy(lookup_name, functionName, sizeof(lookup_name)-1); lookup_name[sizeof(lookup_name)-1] = '\0';
                    toLowerString(lookup_name);
                }

                if (func_symbol && func_symbol->is_defined) {
                    if (func_symbol->type == TYPE_VOID) {
                        fprintf(stderr, "L%d: Compiler Error: Procedure '%s' cannot be used as a function.\n", line, original_display_name);
                        compiler_had_error = true;
                        for(uint8_t i=0; i < node->child_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                        writeBytecodeChunk(chunk, OP_CONSTANT, line);
                        writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk, makeNil()), line);
                    } else {
                        if (func_symbol->arity != node->child_count) {
                            fprintf(stderr, "L%d: Compiler Error: Function '%s' expects %d arguments, got %d.\n", line, original_display_name, func_symbol->arity, node->child_count);
                            compiler_had_error = true;
                            for(uint8_t i=0; i < node->child_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                            writeBytecodeChunk(chunk, OP_CONSTANT, line);
                            writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk, makeNil()), line);
                        } else {
                            writeBytecodeChunk(chunk, OP_CALL, line);
                            emitShort(chunk, (uint16_t)func_symbol->bytecode_address, line);
                            writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
                        }
                    }
                } else {
                     if (func_symbol && !func_symbol->is_defined) fprintf(stderr, "L%d: Compiler Error: Function '%s' is forward declared.\n", line, original_display_name);
                     else fprintf(stderr, "L%d: Compiler Error: Undefined function '%s'.\n", line, original_display_name);
                     compiler_had_error = true;
                     for(uint8_t i=0; i < node->child_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                     writeBytecodeChunk(chunk, OP_CONSTANT, line);
                     writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk, makeNil()), line);
                }
            }
            break;
        }
        default:
            fprintf(stderr, "L%d: Compiler warning: Unhandled AST node type %s in compileRValue.\n", line, astTypeToString(node->type));
            int dummyIdx = addConstantToChunk(chunk, makeInt(0)); // Push dummy 0 for expression context
            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, (uint8_t)dummyIdx, line);
            break;
    }
}
