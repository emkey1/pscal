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

#define MAX_LOOP_DEPTH 16 // Max nested loops

typedef struct {
    int start;          // Address of the loop's start
    int* break_jumps;   // Dynamic array of jump instructions from 'break'
    int break_count;    // Number of 'break' statements
    int scope_depth;    // The scope depth of this loop
} Loop;

static Loop loop_stack[MAX_LOOP_DEPTH];
static int loop_depth = -1; // -1 means we are not in a loop

typedef struct {
    CompilerLocal locals[MAX_GLOBALS]; // Re-use MAX_GLOBALS for max locals per function
    int local_count;
    int scope_depth;
    const char* name;
} FunctionCompilerState;

FunctionCompilerState* current_function_compiler = NULL;

static int addStringConstant(BytecodeChunk* chunk, const char* str) {
    Value val = makeString(str);
    int index = addConstantToChunk(chunk, &val);
    freeValue(&val); // The temporary Value's contents are freed here.
    return index;
}

static int addIntConstant(BytecodeChunk* chunk, long long intValue) {
    Value val = makeInt(intValue);
    int index = addConstantToChunk(chunk, &val);
    // No need to call freeValue for simple types, but it's harmless.
    return index;
}

static int addRealConstant(BytecodeChunk* chunk, double floatValue) {
    Value val = makeReal(floatValue);
    int index = addConstantToChunk(chunk, &val);
    // No need to call freeValue for simple types, but it's harmless.
    return index;
}

static int addNilConstant(BytecodeChunk* chunk) {
    Value val = makeNil();
    int index = addConstantToChunk(chunk, &val);
    // freeValue(&val) is not needed as TYPE_NIL holds no dynamic memory.
    return index;
}

static int addBooleanConstant(BytecodeChunk* chunk, bool boolValue) {
    Value val = makeBoolean(boolValue);
    int index = addConstantToChunk(chunk, &val);
    // No freeValue needed for simple boolean types.
    return index;
}

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

static void startLoop(int start_address) {
    if (loop_depth + 1 >= MAX_LOOP_DEPTH) {
        fprintf(stderr, "Compiler error: Loop nesting too deep.\n");
        compiler_had_error = true;
        return;
    }
    loop_depth++;
    loop_stack[loop_depth].start = start_address;
    loop_stack[loop_depth].break_jumps = NULL;
    loop_stack[loop_depth].break_count = 0;
    loop_stack[loop_depth].scope_depth = current_function_compiler ? current_function_compiler->scope_depth : 0;
}

static void addBreakJump(BytecodeChunk* chunk, int line) {
    if (loop_depth < 0) {
        fprintf(stderr, "L%d: Compiler error: 'break' statement outside of a loop.\n", line);
        compiler_had_error = true;
        return;
    }
    Loop* current_loop = &loop_stack[loop_depth];
    current_loop->break_count++;
    current_loop->break_jumps = realloc(current_loop->break_jumps, sizeof(int) * current_loop->break_count);
    
    writeBytecodeChunk(chunk, OP_JUMP, line);
    current_loop->break_jumps[current_loop->break_count - 1] = chunk->count; // Store offset of the operand
    emitShort(chunk, 0xFFFF, line); // Placeholder
}

static void patchBreaks(BytecodeChunk* chunk) {
    if (loop_depth < 0) return;
    Loop* current_loop = &loop_stack[loop_depth];
    int jump_target = chunk->count; // The address to jump to is the one right after the loop.

    for (int i = 0; i < current_loop->break_count; i++) {
        int jump_offset = current_loop->break_jumps[i];
        patchShort(chunk, jump_offset, (uint16_t)(jump_target - (jump_offset + 2)));
    }

    if (current_loop->break_jumps) {
        free(current_loop->break_jumps);
        // --- MODIFICATION: Set pointer to NULL after freeing ---
        current_loop->break_jumps = NULL;
    }
}

static void endLoop(void) {
    if (loop_depth < 0) return;
    
    // --- MODIFICATION: This function should ONLY manage the loop depth. ---
    // The patching and freeing is handled entirely by patchBreaks().
    // We can add a check here to catch logic errors.
    if (loop_stack[loop_depth].break_jumps != NULL) {
        fprintf(stderr, "Compiler internal warning: endLoop called but break_jumps was not freed. Indicates missing patchBreaks() call.\n");
        // Safeguard free, though the call site is the real issue.
        free(loop_stack[loop_depth].break_jumps);
        loop_stack[loop_depth].break_jumps = NULL;
    }

    loop_depth--;
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
void addCompilerConstant(const char* name_original_case, const Value* value, int line) {
    if (compilerConstantCount >= MAX_COMPILER_CONSTANTS) {
        fprintf(stderr, "L%d: Compiler error: Too many compile-time constants.\n", line);
        // Do not free value; caller is responsible.
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
            
            // <<<< FIX: Pass 'value' directly, not its address. >>>>
            compilerConstants[i].value = makeCopyOfValue(value);

            // <<<< FIX: Remove this free. Caller is responsible. >>>>
            // freeValue(&value);
            return;
        }
    }

    // This block handles adding a NEW constant.
    compilerConstants[compilerConstantCount].name = strdup(canonical_name);
    
    // <<<< FIX: Pass 'value' directly, not its address. >>>>
    compilerConstants[compilerConstantCount].value = makeCopyOfValue(value);
    
    compilerConstantCount++;
    
    // <<<< FIX: Remove this free. Caller is responsible. >>>>
    // freeValue(&value);
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
            if (node->token && strlen(node->token->value) == 1) return makeChar(node->token->value[0]);
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
                int nameIndex =  addStringConstant(chunk, varName);
                writeBytecodeChunk(chunk, OP_GET_GLOBAL_ADDRESS, line);
                writeBytecodeChunk(chunk, (uint8_t)nameIndex, line);
            }
            break;
        }
        case AST_FIELD_ACCESS: {
            // Recursively compile the L-Value of the base (e.g., myRec or p^)
            compileLValue(node->left, chunk, getLine(node->left));

            // Now, get the address of the specific field.
            int fieldNameIndex = addStringConstant(chunk, node->token->value);
            writeBytecodeChunk(chunk, OP_GET_FIELD_ADDRESS, line);
            writeBytecodeChunk(chunk, (uint8_t)fieldNameIndex, line);
            break;
        }
        case AST_ARRAY_ACCESS: {
            // Check if the base is a string for special handling
            if (node->left && node->left->var_type == TYPE_STRING) {
                // For string[index], push the address of the string variable, then the index.
                // The new OP_GET_CHAR_ADDRESS will then resolve to the char's address.
                compileLValue(node->left, chunk, getLine(node->left)); // Push address of string variable (Value*)
                compileRValue(node->children[0], chunk, getLine(node->children[0])); // Push the single index
                writeBytecodeChunk(chunk, OP_GET_CHAR_ADDRESS, line); // New opcode for char address
            } else {
                // Standard array access: push array base address, then all indices.
                compileLValue(node->left, chunk, getLine(node->left)); // Push address of array variable (Value*)

                // Compile all index expressions. Their values will be on the stack.
                for (int i = 0; i < node->child_count; i++) {
                    compileRValue(node->children[i], chunk, getLine(node->children[i]));
                }
                
                // Now, get the address of the specific element.
                writeBytecodeChunk(chunk, OP_GET_ELEMENT_ADDRESS, line);
                writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
            }
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
    // Do NOT re-initialize the chunk here, it's already populated with unit code.
    // initBytecodeChunk(outputChunk);
    compilerGlobalCount = 0;
    compiler_had_error = false;
    current_function_compiler = NULL;

    if (rootNode->type == AST_PROGRAM) {
        // The `USES` clause has already been handled during parsing.
        // We only need to compile the main program block here.
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
            // An AST_BLOCK should have two children: declarations and statements.
            AST* declarations = (node->child_count > 0) ? node->children[0] : NULL;
            AST* statements = (node->child_count > 1) ? node->children[1] : NULL;

            if (declarations && declarations->type == AST_COMPOUND) {
                // Pass 1: Compile variable declarations from the declaration block.
                for (int i = 0; i < declarations->child_count; i++) {
                    AST* decl_child = declarations->children[i];
                    if (decl_child && decl_child->type == AST_VAR_DECL) {
                        compileNode(decl_child, chunk, getLine(decl_child));
                    }
                }
                // Pass 2: Compile routines from the declaration block. <<<< THIS WAS MISSING
                for (int i = 0; i < declarations->child_count; i++) {
                    AST* decl_child = declarations->children[i];
                    if (decl_child && (decl_child->type == AST_PROCEDURE_DECL || decl_child->type == AST_FUNCTION_DECL)) {
                        compileNode(decl_child, chunk, getLine(decl_child));
                    }
                }
            }
            
            // Pass 3: Compile the main statement block.
            if (statements && statements->type == AST_COMPOUND) {
                 for (int i = 0; i < statements->child_count; i++) {
                    if (statements->children[i]) {
                        compileNode(statements->children[i], chunk, getLine(statements->children[i]));
                    }
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
                        int var_name_idx = addStringConstant(chunk, varNameNode->token->value);
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
                                    
                                    // Use the new helper for the lower bound
                                    if (lower_b.type == TYPE_INTEGER) {
                                        writeBytecodeChunk(chunk, (uint8_t)addIntConstant(chunk, lower_b.i_val), getLine(varNameNode));
                                    } else {
                                        fprintf(stderr, "L%d: Compiler error: Array bound did not evaluate to a constant integer.\n", getLine(varNameNode));
                                        compiler_had_error = true;
                                    }
                                    freeValue(&lower_b);
                                    
                                    // Use the new helper for the upper bound
                                    if (upper_b.type == TYPE_INTEGER) {
                                        writeBytecodeChunk(chunk, (uint8_t)addIntConstant(chunk, upper_b.i_val), getLine(varNameNode));
                                    } else {
                                        fprintf(stderr, "L%d: Compiler error: Array bound did not evaluate to a constant integer.\n", getLine(varNameNode));
                                        compiler_had_error = true;
                                    }
                                    freeValue(&upper_b);
                                    
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
                            
                            // <<<< FIX for Snippet 1 >>>>
                            writeBytecodeChunk(chunk, (uint8_t)addStringConstant(chunk, elem_type_name), getLine(varNameNode));

                        } else if (actual_type_def_node->type == AST_VARIABLE && strcasecmp(actual_type_def_node->token->value, "string") == 0 && actual_type_def_node->right) {
                            // Fixed-length string declaration
                            AST* lenNode = actual_type_def_node->right;
                            Value len_val = evaluateCompileTimeValue(lenNode);
                            if (len_val.type == TYPE_INTEGER && len_val.i_val >= 0 && len_val.i_val <= 255) {
                                writeBytecodeChunk(chunk, (uint8_t)addIntConstant(chunk, len_val.i_val), getLine(varNameNode)); // Length
                            } else {
                                fprintf(stderr, "L%d: Compiler error: String length must be a constant integer between 0 and 255.\n", getLine(varNameNode));
                                compiler_had_error = true;
                                writeBytecodeChunk(chunk, 0, getLine(varNameNode)); // Default length 0 or error sentinel
                            }
                            freeValue(&len_val);
                            // Element type name (empty string for basic string type)
                            writeBytecodeChunk(chunk, (uint8_t)addStringConstant(chunk, ""), getLine(varNameNode));

                        } else {
                            // This handles simple types, records, and other non-array aliased types.
                            const char* type_name = (type_specifier_node && type_specifier_node->token) ? type_specifier_node->token->value : "";
                            
                            // <<<< FIX for Snippet 2 >>>>
                            writeBytecodeChunk(chunk, (uint8_t)addStringConstant(chunk, type_name), getLine(varNameNode));
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
        int nil_const_idx = addNilConstant(chunk);
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
        case AST_BREAK: { // <<< NEW CASE
            addBreakJump(chunk, line);
            break;
        }
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
            startLoop(chunk->count); // <<< MODIFIED: Mark loop start

            int loopStart = chunk->count;

            compileRValue(node->left, chunk, line);

            writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line);
            int exitJumpOffset = chunk->count;
            emitShort(chunk, 0xFFFF, line);

            compileStatement(node->right, chunk, getLine(node->right));

            writeBytecodeChunk(chunk, OP_JUMP, line);
            int backwardJumpOffset = loopStart - (chunk->count + 2);
            emitShort(chunk, (uint16_t)backwardJumpOffset, line);

            patchShort(chunk, exitJumpOffset, (uint16_t)(chunk->count - (exitJumpOffset + 2)));
            
            patchBreaks(chunk); // <<< MODIFIED: Patch any breaks inside the loop
            endLoop(); // <<< MODIFIED: End loop context
            break;
        }
        case AST_CASE: {
            int line = getLine(node);
            if (line <= 0) line = current_line_approx;

            // 1. Compile the main expression to be tested. Its value is now on the stack.
            compileRValue(node->left, chunk, line);

            int else_jump = -1; // Jump from the end of the last branch to the end of the CASE
            
            // List of jumps from each successful branch to the very end of the CASE statement.
            int *end_jumps = NULL;
            int end_jumps_count = 0;

            // 2. Iterate through each CASE branch (e.g., '1: stmtA; 2,3: stmtB;')
            for (int i = 0; i < node->child_count; i++) {
                AST* branch = node->children[i];
                if (!branch || branch->type != AST_CASE_BRANCH) continue;

                int next_branch_jump = -1; // Jumps to the start of the next branch's label checks.

                // This is the beginning of the checks for the current branch.
                // If a previous branch didn't match, it would have jumped here.
                if (else_jump != -1) {
                    patchShort(chunk, else_jump, chunk->count - (else_jump + 2));
                }

                AST* labels_node = branch->left;
                AST** labels_to_check = &labels_node;
                int num_labels = 1;
                if (labels_node->type == AST_COMPOUND) {
                    labels_to_check = labels_node->children;
                    num_labels = labels_node->child_count;
                }

                // 3. For each label within the current branch.
                for (int j = 0; j < num_labels; j++) {
                    AST* label = labels_to_check[j];
                    
                    // Duplicate the case value for comparison.
                    writeBytecodeChunk(chunk, OP_DUP, line);
                    
                    if (label->type == AST_SUBRANGE) {
                        // For ranges like 'A'..'C', check if value is within bounds.
                        // Stack: [case_val, case_val]
                        compileRValue(label->left, chunk, getLine(label)); // Stack: [case_val, case_val, 'A']
                        writeBytecodeChunk(chunk, OP_SWAP, line);           // Stack: [case_val, 'A', case_val]
                        writeBytecodeChunk(chunk, OP_GREATER_EQUAL, line);  // Stack: [case_val, true/false]
                        
                        int jump_if_lower = chunk->count;
                        writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line); emitShort(chunk, 0xFFFF, line);
                        
                        // If we are here, it means case_val >= 'A' was true.
                        writeBytecodeChunk(chunk, OP_DUP, line); // Stack: [case_val, case_val]
                        compileRValue(label->right, chunk, getLine(label)); // Stack: [case_val, case_val, 'C']
                        writeBytecodeChunk(chunk, OP_SWAP, line);            // Stack: [case_val, 'C', case_val]
                        writeBytecodeChunk(chunk, OP_LESS_EQUAL, line);   // Stack: [case_val, true/false]
                        
                        patchShort(chunk, jump_if_lower, chunk->count - (jump_if_lower + 2));

                    } else {
                        // For single labels like 'A' or 5.
                        compileRValue(label, chunk, getLine(label));
                        writeBytecodeChunk(chunk, OP_EQUAL, line);
                    }
                    
                    // If the result of the comparison is true, jump past the next branch check.
                    int jump_to_body = chunk->count;
                    writeBytecodeChunk(chunk, OP_NOT, line);
                    writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line); emitShort(chunk, 0xFFFF, line);
                    
                    // If we fall through, the label didn't match. Jump to the next branch's check.
                    if (next_branch_jump != -1) {
                       patchShort(chunk, next_branch_jump, chunk->count - (next_branch_jump + 2));
                    }
                    next_branch_jump = chunk->count;
                    writeBytecodeChunk(chunk, OP_JUMP, line); emitShort(chunk, 0xFFFF, line);
                    
                    // Patch the jump for a successful match to land here.
                    patchShort(chunk, jump_to_body, chunk->count - (jump_to_body + 2));
                }
                
                // This is the start of the body for the current branch.
                writeBytecodeChunk(chunk, OP_POP, line); // Pop the matched case value.
                compileStatement(branch->right, chunk, getLine(branch->right));
                
                // After executing the body, jump to the very end of the CASE statement.
                end_jumps = realloc(end_jumps, (end_jumps_count + 1) * sizeof(int));
                end_jumps[end_jumps_count++] = chunk->count;
                writeBytecodeChunk(chunk, OP_JUMP, line); emitShort(chunk, 0xFFFF, line);
                
                // The next set of label checks will begin here.
                if (next_branch_jump != -1) {
                   patchShort(chunk, next_branch_jump, chunk->count - (next_branch_jump + 2));
                }
                else_jump = next_branch_jump;
            }

            // After all branches, if an 'else' exists, compile it.
            if (node->extra) {
                if (else_jump != -1) {
                    patchShort(chunk, else_jump, chunk->count - (else_jump + 2));
                }
                 writeBytecodeChunk(chunk, OP_POP, line); // Pop the case value before else.
                compileStatement(node->extra, chunk, getLine(node->extra));
            } else {
                if (else_jump != -1) {
                    patchShort(chunk, else_jump, chunk->count - (else_jump + 2));
                }
                // If no else, and no branches matched, we still need to pop the case value.
                writeBytecodeChunk(chunk, OP_POP, line);
            }
            
            // This is the end of the CASE. Patch all jumps from successful branches to here.
            for (int i = 0; i < end_jumps_count; i++) {
                patchShort(chunk, end_jumps[i], chunk->count - (end_jumps[i] + 2));
            }
            if (end_jumps) free(end_jumps);

            break;
        }
        case AST_REPEAT: {
            startLoop(chunk->count); // <<< MODIFIED
            int loopStart = chunk->count;

            if (node->left) {
                compileStatement(node->left, chunk, getLine(node->left));
            }

            if (node->right) {
                compileRValue(node->right, chunk, getLine(node->right));
            } else {
                int falseConstIdx = addBooleanConstant(chunk, false);
                writeBytecodeChunk(chunk, OP_CONSTANT, line);
                writeBytecodeChunk(chunk, (uint8_t)falseConstIdx, line);
            }

            writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line);
            int backward_jump_offset = loopStart - (chunk->count + 2);
            emitShort(chunk, (uint16_t)backward_jump_offset, line);

            patchBreaks(chunk); // <<< MODIFIED
            endLoop(); // <<< MODIFIED
            break;
        }
        case AST_READLN: {
            int line = getLine(node);
            
            int var_start_index = 0;
            // Check if the first argument is a file variable. We can guess based on its type,
            // which the annotation pass should have set on the AST node.
            if (node->child_count > 0 && node->children[0]->var_type == TYPE_FILE) {
                // If the first arg is a file, compile it as an R-Value.
                compileRValue(node->children[0], chunk, getLine(node->children[0]));
                var_start_index = 1; // The rest of the args are variables to read into.
            }

            // Compile all subsequent arguments as L-Values (addresses).
            for (int i = var_start_index; i < node->child_count; i++) {
                AST* arg_node = node->children[i];
                compileLValue(arg_node, chunk, getLine(arg_node));
            }

            // Call the built-in `readln` function. This part is correct.
            int nameIndex = addStringConstant(chunk, "readln");
            writeBytecodeChunk(chunk, OP_CALL_BUILTIN, line);
            writeBytecodeChunk(chunk, (uint8_t)nameIndex, line);
            writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
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
                    // The OP_POP instruction that was here has been removed.
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

            int var_slot = -1;
            int var_name_idx = -1;
            
            if (current_function_compiler) {
                var_slot = resolveLocal(current_function_compiler, var_node->token->value);
            }

            if (var_slot == -1) {
                var_name_idx = addStringConstant(chunk, var_node->token->value);
            }

            // 1. Initial assignment of the loop variable
            compileRValue(start_node, chunk, getLine(start_node));
            if (var_slot != -1) {
                writeBytecodeChunk(chunk, OP_SET_LOCAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_slot, line);
            } else {
                writeBytecodeChunk(chunk, OP_SET_GLOBAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_name_idx, line);
            }

            // 2. Setup loop context for handling 'break'
            startLoop(-1); // Start address is not needed for FOR loop's break handling

            int loopStart = chunk->count;

            // 3. The loop condition check
            if (var_slot != -1) {
                writeBytecodeChunk(chunk, OP_GET_LOCAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_slot, line);
            } else {
                writeBytecodeChunk(chunk, OP_GET_GLOBAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_name_idx, line);
            }
            
            compileRValue(end_node, chunk, getLine(end_node));
            
            writeBytecodeChunk(chunk, is_downto ? OP_GREATER_EQUAL : OP_LESS_EQUAL, line);

            writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line);
            int exitJump = chunk->count;
            emitShort(chunk, 0xFFFF, line);

            // 4. Compile the loop body
            compileStatement(body_node, chunk, getLine(body_node));
            
            // 5. Increment/Decrement the loop variable
            if (var_slot != -1) {
                writeBytecodeChunk(chunk, OP_GET_LOCAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_slot, line);
            } else {
                writeBytecodeChunk(chunk, OP_GET_GLOBAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_name_idx, line);
            }
            int one_const_idx = addIntConstant(chunk, 1);
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

            // The value from the increment/decrement is still on the stack.
            // Pop it to prevent stack overflow.
            //writeBytecodeChunk(chunk, OP_POP, line);

            // 6. Jump back to the top of the loop to re-evaluate the condition
            writeBytecodeChunk(chunk, OP_JUMP, line);
            int backward_jump_offset = loopStart - (chunk->count + 2);
            emitShort(chunk, (uint16_t)backward_jump_offset, line);

            // 7. This is the exit point for the loop. Patch the initial condition jump.
            patchShort(chunk, exitJump, (uint16_t)(chunk->count - (exitJump + 2)));
            
            // 8. Patch any 'break' statements that occurred inside the loop body.
            patchBreaks(chunk);
            endLoop();
            
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
            const char* calleeName = node->token->value;
            Symbol* proc_symbol = lookupSymbolIn(procedure_table, calleeName);
            // Add a flag to identify read/readln calls
            bool is_read_proc = (strcasecmp(calleeName, "read") == 0 || strcasecmp(calleeName, "readln") == 0);


            // Compile arguments first
            for (int i = 0; i < node->child_count; i++) {
                AST* arg_node = node->children[i];
                bool is_var_param = false;

                // For read/readln, any argument that is NOT a file variable is a VAR param.
                // The annotation pass should have set the var_type on the argument's AST node.
                if (is_read_proc && (i > 0 || (i == 0 && arg_node->var_type != TYPE_FILE))) {
                    is_var_param = true;
                }
                else if (calleeName && (strcasecmp(calleeName, "new") == 0 || strcasecmp(calleeName, "dispose") == 0)) {
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
                int nameIndex = addStringConstant(chunk, normalized_name);
                writeBytecodeChunk(chunk, OP_CALL_BUILTIN, line);
                writeBytecodeChunk(chunk, (uint8_t)nameIndex, line);
                writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
                
                // If a function is called as a statement, pop its return value
                if (getBuiltinType(calleeName) == BUILTIN_TYPE_FUNCTION) {
                    writeBytecodeChunk(chunk, OP_POP, line);
                }
            } else {
                if (proc_symbol && proc_symbol->is_defined) {
                    writeBytecodeChunk(chunk, OP_CALL, line);
                    emitShort(chunk, (uint16_t)proc_symbol->bytecode_address, line);
                    writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
                    
                    // If it's a user function, pop its result
                    if (proc_symbol->type != TYPE_VOID) {
                        writeBytecodeChunk(chunk, OP_POP, line);
                    }
                } else {
                    fprintf(stderr, "L%d: Compiler Error: Undefined or forward-declared procedure '%s'.\n", line, calleeName);
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

// --- NEW STATIC HELPER FOR COMPILING SETS ---
// This is a simplified adaptation of the logic from `evalSet`
static void addOrdinalToSetValue(Value* setVal, long long ordinal) {
    // Check for duplicates
    for (int i = 0; i < setVal->set_val.set_size; i++) {
        if (setVal->set_val.set_values[i] == ordinal) {
            return; // Already in set
        }
    }
    // Reallocate if needed
    if (setVal->set_val.set_size >= setVal->max_length) {
        int new_capacity = (setVal->max_length == 0) ? 8 : setVal->max_length * 2;
        long long* new_values = realloc(setVal->set_val.set_values, sizeof(long long) * new_capacity);
        if (!new_values) {
            fprintf(stderr, "FATAL: realloc failed in addOrdinalToSetValue\n");
            EXIT_FAILURE_HANDLER();
        }
        setVal->set_val.set_values = new_values;
        setVal->max_length = new_capacity;
    }
    // Add the new element
    setVal->set_val.set_values[setVal->set_val.set_size++] = ordinal;
}

static void compileRValue(AST* node, BytecodeChunk* chunk, int current_line_approx) {
    if (!node) return;
    int line = getLine(node);
    if (line <= 0) line = current_line_approx;

    Token* node_token = node->token;

    switch (node->type) {
        case AST_SET: {
            // Create a temporary Value struct to build the set constant.
            Value set_const_val;
            memset(&set_const_val, 0, sizeof(Value));
            set_const_val.type = TYPE_SET;
            set_const_val.max_length = 0;
            set_const_val.set_val.set_size = 0;
            set_const_val.set_val.set_values = NULL;

#ifdef DEBUG
            fprintf(stderr, "[DEBUG SET] Initialized set_const_val at %p. size=%d, capacity=%d\n",
                    (void*)&set_const_val, set_const_val.set_val.set_size, set_const_val.max_length);
            fflush(stderr);
#endif

            for (int i = 0; i < node->child_count; i++) {
                AST* member = node->children[i];
                if (member->type == AST_SUBRANGE) {
                    Value start_val = evaluateCompileTimeValue(member->left);
                    Value end_val = evaluateCompileTimeValue(member->right);
                    if ((start_val.type == TYPE_INTEGER || start_val.type == TYPE_CHAR) &&
                        (end_val.type == TYPE_INTEGER || end_val.type == TYPE_CHAR)) {
                        long long start_ord = (start_val.type == TYPE_INTEGER) ? start_val.i_val : start_val.c_val;
                        long long end_ord = (end_val.type == TYPE_INTEGER) ? end_val.i_val : end_val.c_val;
                        for (long long j = start_ord; j <= end_ord; j++) {
                           addOrdinalToSetValue(&set_const_val, j);
                        }
                    } else {
                        fprintf(stderr, "L%d: Compiler error: Set range bounds must be constant ordinal types.\n", getLine(member));
                        compiler_had_error = true;
                    }
                    freeValue(&start_val);
                    freeValue(&end_val);
                } else {
                    Value elem_val = evaluateCompileTimeValue(member);
                    if (elem_val.type == TYPE_INTEGER || elem_val.type == TYPE_CHAR) {
                        long long ord = (elem_val.type == TYPE_INTEGER) ? elem_val.i_val : elem_val.c_val;
                        addOrdinalToSetValue(&set_const_val, ord);
                    } else {
                        fprintf(stderr, "L%d: Compiler error: Set elements must be constant ordinal types.\n", getLine(member));
                        compiler_had_error = true;
                    }
                    freeValue(&elem_val);
                }
            }

            // <<<< ADD THIS DEBUG PRINT >>>>
            fprintf(stderr, "[DEBUG SET] Finished building set. Final size=%d, capacity=%d. Adding to chunk.\n",
                    set_const_val.set_val.set_size, set_const_val.max_length);
            fflush(stderr);
            // Pass the address of the fully constructed set Value.
            int constIndex = addConstantToChunk(chunk, &set_const_val);
            // Now that a deep copy is in the chunk, free our temporary set value.
            freeValue(&set_const_val);

            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, (uint8_t)constIndex, line);
            break;
        }
        case AST_NUMBER: {
            if (!node_token || !node_token->value) { /* error */ break; }
            
            int constIndex;
            // Use the appropriate helper based on the token type
            if (node_token->type == TOKEN_REAL_CONST) {
                constIndex = addRealConstant(chunk, atof(node_token->value));
            } else {
                constIndex = addIntConstant(chunk, atoll(node_token->value));
            }

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

            int constIndex = addStringConstant(chunk, node_token->value);

            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, (uint8_t)constIndex, line);
            break;
        }
        case AST_NIL: {
            int constIndex = addNilConstant(chunk);
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
            
            if (strcasecmp(varName, "break_requested") == 0) {
                // This is a special host-provided variable.
                // Instead of treating it as a global, we call a host function.
                writeBytecodeChunk(chunk, OP_CALL_HOST, line);
                writeBytecodeChunk(chunk, (uint8_t)HOST_FN_QUIT_REQUESTED, line);
                break; // We are done compiling this node.
            }
            
            if (local_slot != -1) {
                writeBytecodeChunk(chunk, OP_GET_LOCAL, line);
                writeBytecodeChunk(chunk, (uint8_t)local_slot, line);
                if (is_ref) {
                    writeBytecodeChunk(chunk, OP_GET_INDIRECT, line);
                }
            } else {
                // Check if it's a compile-time constant first.
                Value* const_val_ptr = findCompilerConstant(varName);
                if (const_val_ptr) {
                    // <<<< FIX for compile-time constant >>>>
                    // Pass the pointer to the constant value directly.
                    int constIndex = addConstantToChunk(chunk, const_val_ptr);
                    writeBytecodeChunk(chunk, OP_CONSTANT, line);
                    writeBytecodeChunk(chunk, (uint8_t)constIndex, line);
                } else {
                    // <<<< FIX for global variable >>>>
                    // It's a global variable, so add its name to the constants
                    // using the new helper and emit OP_GET_GLOBAL.
                    int nameIndex = addStringConstant(chunk, varName);
                    writeBytecodeChunk(chunk, OP_GET_GLOBAL, line);
                    writeBytecodeChunk(chunk, (uint8_t)nameIndex, line);
                }
            }
            break;
        }
        case AST_FIELD_ACCESS:
        case AST_ARRAY_ACCESS: {
            // For any R-Value that is an array access or field access,
            // the logic is the same:
            // 1. Compile the L-Value to get the final address of the element/field on the stack.
            // 2. Use OP_GET_INDIRECT to fetch the value at that address.
            compileLValue(node, chunk, getLine(node));
            writeBytecodeChunk(chunk, OP_GET_INDIRECT, line);
            break;
        }
        case AST_BINARY_OP: {
            if (node_token && node_token->type == TOKEN_AND) {
                // Correct short-circuit for: A AND B
                // If A is false, the expression is false.
                // If A is true, the expression's value is the value of B.
                compileRValue(node->left, chunk, getLine(node->left)); // stack: [A]
                int jump_if_false = chunk->count;
                writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line);     // Pops A. Jumps if A is false.
                emitShort(chunk, 0xFFFF, line);

                // If we get here, A was true and was popped. The stack is empty.
                // The result of the whole expression is now the result of B.
                compileRValue(node->right, chunk, getLine(node->right)); // stack: [B]
                int jump_over_false_case = chunk->count;
                writeBytecodeChunk(chunk, OP_JUMP, line);
                emitShort(chunk, 0xFFFF, line);

                // This is where we land if A was false. The stack is empty.
                // We must push 'false' onto the stack as the expression's result.
                patchShort(chunk, jump_if_false + 1, chunk->count - (jump_if_false + 3));
                int false_const_idx = addBooleanConstant(chunk, false);
                writeBytecodeChunk(chunk, OP_CONSTANT, line);
                writeBytecodeChunk(chunk, (uint8_t)false_const_idx, line); // stack: [false]

                // The end for both paths.
                patchShort(chunk, jump_over_false_case + 1, chunk->count - (jump_over_false_case + 3));

            } else if (node_token && node_token->type == TOKEN_OR) {
                // Correct short-circuit for: A OR B
                // If A is true, the expression is true.
                // If A is false, the expression's value is the value of B.
                compileRValue(node->left, chunk, getLine(node->left)); // stack: [A]
                int jump_if_false = chunk->count;
                writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line);     // Pops A. Jumps if A is false.
                emitShort(chunk, 0xFFFF, line);

                // If we get here, A was true. Stack is empty. The result must be 'true'.
                int true_const_idx = addBooleanConstant(chunk, true);
                writeBytecodeChunk(chunk, OP_CONSTANT, line);
                writeBytecodeChunk(chunk, (uint8_t)true_const_idx, line);
                int jump_to_end = chunk->count;
                writeBytecodeChunk(chunk, OP_JUMP, line);
                emitShort(chunk, 0xFFFF, line);

                // This is where we land if A was false. Stack is empty.
                // The result of the expression is the result of B.
                patchShort(chunk, jump_if_false + 1, chunk->count - (jump_if_false + 3));
                compileRValue(node->right, chunk, getLine(node->right));

                // The end for both paths.
                patchShort(chunk, jump_to_end + 1, chunk->count - (jump_to_end + 3));
            }
            else { // Original logic for all other operators
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
                        // AND and OR are now handled above
                        case TOKEN_SHL:           writeBytecodeChunk(chunk, OP_SHL, line); break;
                        case TOKEN_SHR:           writeBytecodeChunk(chunk, OP_SHR, line); break;
                        case TOKEN_EQUAL:         writeBytecodeChunk(chunk, OP_EQUAL, line); break;
                        case TOKEN_NOT_EQUAL:     writeBytecodeChunk(chunk, OP_NOT_EQUAL, line); break;
                        case TOKEN_LESS:          writeBytecodeChunk(chunk, OP_LESS, line); break;
                        case TOKEN_LESS_EQUAL:    writeBytecodeChunk(chunk, OP_LESS_EQUAL, line); break;
                        case TOKEN_GREATER:       writeBytecodeChunk(chunk, OP_GREATER, line); break;
                        case TOKEN_GREATER_EQUAL: writeBytecodeChunk(chunk, OP_GREATER_EQUAL, line); break;
                        case TOKEN_IN:            writeBytecodeChunk(chunk, OP_IN, line); break;
                        default:
                            fprintf(stderr, "L%d: Compiler error: Unknown binary operator %s\n", line, tokenTypeToString(node_token->type));
                            compiler_had_error = true;
                            break;
                    }
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
            // The check for node_token is still useful for malformed ASTs,
            // though the first 'if' condition was unusual. We can simplify the check.
            if (!node_token) {
                 // This case might be hit for certain internally generated boolean values.
                 // Let's trust node->i_val. The old code did this as well.
            }

            // Use the new helper to add the boolean constant and get its index.
            // The node->i_val for booleans is 0 for false and 1 for true.
            int constIndex = addBooleanConstant(chunk, (node->i_val != 0));
            
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
                // Use the new helper to add a nil constant.
                writeBytecodeChunk(chunk, (uint8_t)addNilConstant(chunk), line);
                break;
            }

            Symbol* func_symbol = functionName ? lookupSymbolIn(procedure_table, functionName) : NULL;
            
            // Special handling for built-ins that take type identifiers instead of values.
            if (isBuiltin(functionName) && (strcasecmp(functionName, "low") == 0 || strcasecmp(functionName, "high") == 0)) {
                if (node->child_count == 1 && node->children[0]->type == AST_VARIABLE) {
                    AST* type_arg_node = node->children[0];
                    // Push the *name* of the type as a string constant.
                    int typeNameIndex = addStringConstant(chunk, type_arg_node->token->value);
                    writeBytecodeChunk(chunk, OP_CONSTANT, line);
                    writeBytecodeChunk(chunk, (uint8_t)typeNameIndex, line);
                } else {
                    fprintf(stderr, "L%d: Compiler error: Argument to '%s' must be a single type identifier.\n", line, functionName);
                    compiler_had_error = true;
                }
            } else {
                if (node->child_count > 0 && node->children) {
                    for (int i = 0; i < node->child_count; i++) {
                        AST* arg_node = node->children[i];
                        if (!arg_node) continue;
                        
                        bool is_var_param = false;
                        
                        if (functionName && (strcasecmp(functionName, "new") == 0 || strcasecmp(functionName, "dispose") == 0)) {
                            is_var_param = true;
                        }
                        else if (func_symbol && func_symbol->type_def && i < func_symbol->type_def->child_count) {
                            AST* param_node = func_symbol->type_def->children[i];
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
                }
            // --- ADDED THIS BRACE ---
            }
            
            if (isBuiltin(functionName)) {
                BuiltinRoutineType type = getBuiltinType(functionName);
                if (type == BUILTIN_TYPE_PROCEDURE) {
                    fprintf(stderr, "L%d: Compiler Error: Built-in procedure '%s' cannot be used as a function in an expression.\n", line, functionName);
                    compiler_had_error = true;
                    for(uint8_t i = 0; i < node->child_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                    writeBytecodeChunk(chunk, OP_CONSTANT, line);
                    // Use the new helper to add a nil constant.
                    writeBytecodeChunk(chunk, (uint8_t)addNilConstant(chunk), line);
                } else if (type == BUILTIN_TYPE_FUNCTION) {
                    char normalized_name[MAX_SYMBOL_LENGTH];
                    strncpy(normalized_name, functionName, sizeof(normalized_name) - 1);
                    normalized_name[sizeof(normalized_name) - 1] = '\0';
                    toLowerString(normalized_name);
                    int nameIndex = addStringConstant(chunk, normalized_name);
                    writeBytecodeChunk(chunk, OP_CALL_BUILTIN, line);
                    writeBytecodeChunk(chunk, (uint8_t)nameIndex, line);
                    writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
                } else {
                     fprintf(stderr, "L%d: Compiler Error: '%s' is not a recognized built-in function for expression context.\n", line, functionName);
                    compiler_had_error = true;
                    for(uint8_t i = 0; i < node->child_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                    writeBytecodeChunk(chunk, OP_CONSTANT, line);
                    writeBytecodeChunk(chunk, (uint8_t)addNilConstant(chunk), line);
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
                        writeBytecodeChunk(chunk, (uint8_t)addNilConstant(chunk), line);
                    } else {
                        if (func_symbol->arity != node->child_count) {
                            fprintf(stderr, "L%d: Compiler Error: Function '%s' expects %d arguments, got %d.\n", line, original_display_name, func_symbol->arity, node->child_count);
                            compiler_had_error = true;
                            for(uint8_t i=0; i < node->child_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                            writeBytecodeChunk(chunk, OP_CONSTANT, line);
                            writeBytecodeChunk(chunk, (uint8_t)addNilConstant(chunk), line);
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
                    writeBytecodeChunk(chunk, (uint8_t)addNilConstant(chunk), line);
                }
            }
            break;
        }
        default:
            fprintf(stderr, "L%d: Compiler warning: Unhandled AST node type %s in compileRValue.\n", line, astTypeToString(node->type));
            int dummyIdx = addIntConstant(chunk, 0); // Push dummy 0 for expression context
            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, (uint8_t)dummyIdx, line);
            break;
    }
}

void compileUnitImplementation(AST* unit_ast, BytecodeChunk* outputChunk) {
    if (!unit_ast || unit_ast->type != AST_UNIT) {
        return;
    }
    // The implementation block is stored in the 'extra' child of the AST_UNIT node
    AST* impl_block = unit_ast->extra;
    if (!impl_block || impl_block->type != AST_COMPOUND) {
        return;
    }

    #ifdef DEBUG
    fprintf(stderr, "[Compiler] Compiling IMPLEMENTATION section for unit '%s'.\n",
            unit_ast->token ? unit_ast->token->value : "?");
    fflush(stderr);
    #endif

    // The implementation block is a compound node containing procedure/function declarations.
    // We must iterate through them and compile each one individually.
    for (int i = 0; i < impl_block->child_count; i++) {
        AST* decl_node = impl_block->children[i];
        if (decl_node && (decl_node->type == AST_PROCEDURE_DECL || decl_node->type == AST_FUNCTION_DECL)) {
            // This call will create a jump over the body, compile the body,
            // and patch the jump, correctly adding the function's bytecode.
            compileNode(decl_node, outputChunk, getLine(decl_node));
        }
    }
}
