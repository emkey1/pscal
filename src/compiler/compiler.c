// src/compiler/compiler.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // For strcmp, strdup, atoll

#include "compiler/compiler.h"
#include "compiler/bytecode.h"
#include "backend_ast/builtin.h" // For isBuiltin
#include "core/utils.h"
#include "core/types.h"
#include "frontend/ast.h"
#include "symbol/symbol.h" // For access to the main global symbol table, if needed,
                           // though for bytecode compilation, we often build our own tables/mappings.
#include "vm/vm.h"         // For HostFunctionID


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

CompilerGlobalVarInfo compilerGlobals[MAX_GLOBALS];
int compilerGlobalCount = 0;


CompilerGlobalVarInfo compilerGlobals[MAX_GLOBALS]; // MAX_GLOBALS from an appropriate header or defined here

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

// --- Main Compilation Function ---
bool compileASTToBytecode(AST* rootNode, BytecodeChunk* outputChunk) {
    // ... (Your existing implementation, ensure compilerGlobals is cleared) ...
    if (!rootNode || !outputChunk) { /* error */ return false; }
    
    initBytecodeChunk(outputChunk);
    compilerGlobalCount = 0;
    for(int i=0; i < MAX_GLOBALS; ++i) {
        if (compilerGlobals[i].name) {
            free(compilerGlobals[i].name);
            compilerGlobals[i].name = NULL;
        }
    }

    if (rootNode->type == AST_PROGRAM && rootNode->right && rootNode->right->type == AST_BLOCK) {
        AST* mainBlock = rootNode->right;
        compileNode(mainBlock, outputChunk, getLine(rootNode));
    } else if (rootNode->type == AST_BLOCK) {
        compileNode(rootNode, outputChunk, getLine(rootNode));
    } else {
        fprintf(stderr, "Compiler error: Expected AST_PROGRAM or AST_BLOCK node as root.\n");
        // freeBytecodeChunk(outputChunk); // Clean up partially initialized chunk on error
        return false;
    }
    writeBytecodeChunk(outputChunk, OP_HALT, getLine(rootNode));
    // Consider returning !compiler.hadError if you add an error flag to the compiler pass
    return true;
}


// --- Recursive Compilation Dispatcher ---
static void compileNode(AST* node, BytecodeChunk* chunk, int current_line_approx) {
    if (!node) return;
    int line = getLine(node); // Use the updated getLine which uses current_line_approx
    if (line <= 0) line = current_line_approx;


    switch (node->type) {
        case AST_BLOCK: {
            // First pass for procedure/function declarations to define them (get their start address)
            for (int i = 0; i < node->child_count; i++) {
                AST* child = node->children[i];
                if (!child) continue;
                if (child->type == AST_PROCEDURE_DECL || child->type == AST_FUNCTION_DECL) {
                    // This call should lead to compileDefinedFunction, which sets bytecode_address and is_defined
                    compileNode(child, chunk, getLine(child) > 0 ? getLine(child) : line);
                } else if (child->type == AST_COMPOUND) {
                    // Check if this compound ONLY contains procedure/function declarations (e.g. from a unit's implementation block)
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

            // Second pass for VAR_DECLs and the main body / other statements
            AST* main_body_compound = NULL;
            for (int i = 0; i < node->child_count; i++) {
                AST* child = node->children[i];
                if (!child) continue;
                int child_line = getLine(child) > 0 ? getLine(child) : line;

                switch(child->type) {
                    case AST_VAR_DECL:
                    case AST_CONST_DECL: // Assuming consts might be needed before main body
                    case AST_TYPE_DECL:  // Assuming types might be needed before main body
                        compileNode(child, chunk, child_line);
                        break;
                    case AST_COMPOUND: {
                        // Determine if this is the main program body or a compound of var_decls
                        bool is_main_body_candidate = true; // Assume it's main body unless it's clearly just decls
                        if (child->child_count > 0) {
                            for(int k=0; k < child->child_count; ++k) {
                                 // If it contains anything other than var/const/type decls, it's not purely a decl block.
                                if (child->children[k] &&
                                    (child->children[k]->type == AST_PROCEDURE_DECL || child->children[k]->type == AST_FUNCTION_DECL)) {
                                    // This was handled in the first pass.
                                    is_main_body_candidate = false; // This specific compound was for procs.
                                    break;
                                }
                                if (child->children[k] &&
                                    !(child->children[k]->type == AST_VAR_DECL || child->children[k]->type == AST_CONST_DECL || child->children[k]->type == AST_TYPE_DECL)) {
                                    // Contains actual statements, so it could be the main body
                                    break;
                                }
                                // If all children are var/const/type decls, it's not the main exec body
                                if (k == child->child_count - 1) is_main_body_candidate = false;
                            }
                        } else { // Empty compound statement could be a (noop) main body
                            is_main_body_candidate = true;
                        }

                        if (is_main_body_candidate && !main_body_compound) {
                            main_body_compound = child; // Likely the main executable part of the block
                        } else if (!is_main_body_candidate) {
                            // This is a compound of VAR_DECLs etc., compile them now
                             for(int k=0; k < child->child_count; ++k) {
                                if(child->children[k] && (child->children[k]->type == AST_VAR_DECL || child->children[k]->type == AST_CONST_DECL || child->children[k]->type == AST_TYPE_DECL))
                                    compileNode(child->children[k], chunk, getLine(child->children[k]) > 0 ? getLine(child->children[k]) : child_line);
                            }
                        } else if (is_main_body_candidate && main_body_compound) {
                            // This means a second executable compound statement was found in the block.
                            // This could be an error or part of an IF/WHILE etc. if not at the top level of procedure block.
                            // For now, let's assume the parser structure means only one such main compound at block level.
                            // If it's not the main_body_compound, it's an inner structure, let compileStatement handle it.
                            fprintf(stderr, "L%d: Warning: Additional compound statement found in block, compiling as statements.\n", child_line);
                            compileNode(child, chunk, child_line);
                        }
                        break;
                    }
                    case AST_PROCEDURE_DECL: // Already handled
                    case AST_FUNCTION_DECL:  // Already handled
                        break;
                    default: // Other statements
                        compileStatement(child, chunk, child_line);
                        break;
                }
            }

            if (main_body_compound) {
                compileNode(main_body_compound, chunk, getLine(main_body_compound) > 0 ? getLine(main_body_compound) : line);
            } else if (node->parent && (node->parent->type == AST_PROGRAM || node->parent->type == AST_PROCEDURE_DECL || node->parent->type == AST_FUNCTION_DECL) ) {
                // It's okay for a procedure/function to have no executable main body if it only contains declarations.
                // For a program, it's an error if there's no executable part.
                if(node->parent->type == AST_PROGRAM) {
                     fprintf(stderr, "L%d: Compiler Error: No main compound statement (BEGIN...END block) found in program.\n", line);
                }
            }
            break;
        }
        // ... (cases for AST_VAR_DECL, AST_PROCEDURE_DECL/FUNCTION_DECL, AST_COMPOUND from previous response) ...
        // Ensure AST_COMPOUND_STATEMENT is changed to AST_COMPOUND in those cases too.
        case AST_PROCEDURE_DECL:
        case AST_FUNCTION_DECL: {
            if (!node->token || !node->token->value) { /* error */ break; }
            #ifdef DEBUG
            if(dumpExec) fprintf(stderr, "L%d: Compiler: Encountered %s '%s'. Jumping over body, then compiling body.\n",
                    line, astTypeToString(node->type), node->token->value);
            #endif

            writeBytecodeChunk(chunk, OP_JUMP, line);
            int jump_over_body_operand_offset = chunk->count;
            emitShort(chunk, 0xFFFF, line);

            compileDefinedFunction(node, chunk, line);

            uint16_t offset_to_skip_body = (uint16_t)(chunk->count - (jump_over_body_operand_offset + 2));
            patchShort(chunk, jump_over_body_operand_offset, offset_to_skip_body);
            break;
        }

        case AST_COMPOUND: // Body of loops, if/then/else, procedures
            for (int i = 0; i < node->child_count; i++) {
                compileStatement(node->children[i], chunk, getLine(node->children[i]) > 0 ? getLine(node->children[i]) : line);
            }
            break;
        // ... (other cases from your compileNode) ...
        default: // Fallback to statement or expression compilation
            if (node->type >= AST_ASSIGN && node->type <= AST_FOR_DOWNTO) { // Heuristic for statements
                 compileStatement(node, chunk, line);
            } else if (node->type >= AST_NUMBER && node->type <= AST_NIL) { // Heuristic for expressions
                 compileExpression(node, chunk, line);
                 // Expressions compiled via compileNode directly might leave value on stack; decide if it needs OP_POP
            } else {
                 fprintf(stderr, "L%d: Compiler warning: Unhandled AST node type %s in compileNode.\n", line, astTypeToString(node->type));
            }
            break;
    }
}


// --- Compile Defined Function/Procedure Body ---
static void compileDefinedFunction(AST* func_decl_node, BytecodeChunk* chunk, int line) {
    const char* func_name = func_decl_node->token->value;
    int func_bytecode_start_address = chunk->count;
    uint8_t arity = 0;
    uint8_t locals_in_body_count = 0;

    Symbol* proc_symbol = lookupSymbolIn(procedure_table, func_name);
    if (!proc_symbol) {
        fprintf(stderr, "L%d: Compiler INTERNAL ERROR: Procedure/function '%s' not found in procedure_table for body compilation.\n", line, func_name);
    } else {
        proc_symbol->bytecode_address = func_bytecode_start_address;
        proc_symbol->is_defined = true;
        
        AST* params_node = NULL;
        // Child 0 of AST_PROCEDURE_DECL or AST_FUNCTION_DECL is parameters
        // Your parser creates AST_LIST for parameter lists.
        if (func_decl_node->child_count > 0 && func_decl_node->children[0] &&
            func_decl_node->children[0]->type == AST_LIST) { // Check for AST_LIST for parameters
            params_node = func_decl_node->children[0];
            if(params_node) {
                for (int i = 0; i < params_node->child_count; ++i) {
                    AST* param_decl_group_node = params_node->children[i]; // This is AST_VAR_DECL
                    if (param_decl_group_node && param_decl_group_node->type == AST_VAR_DECL) { // AST_VAR_DECL is in types.h
                        if (param_decl_group_node->child_count > 1) {
                             arity += (param_decl_group_node->child_count - 1);
                        }
                    }
                }
            }
        }
        proc_symbol->arity = arity;
        proc_symbol->locals_count = 0;
    }
    #ifdef DEBUG
    if(dumpExec) fprintf(stderr, "L%d: Compiler: Compiling body of %s '%s' at address %d, Arity: %d\n",
            line, astTypeToString(func_decl_node->type), func_name, func_bytecode_start_address, arity);
    #endif

    AST* body_compound_node = NULL;
    AST* proc_main_block_node = NULL;
    int block_child_index = (func_decl_node->type == AST_PROCEDURE_DECL && func_decl_node->child_count > 1) ? 1 :
                            (func_decl_node->type == AST_FUNCTION_DECL && func_decl_node->child_count > 2) ? 2 : -1;
    
    if (block_child_index != -1 && func_decl_node->children[block_child_index] &&
        func_decl_node->children[block_child_index]->type == AST_BLOCK) {
        proc_main_block_node = func_decl_node->children[block_child_index];
    }

    if (proc_main_block_node) {
        AST* local_var_decl_compound = NULL; // This is an AST_COMPOUND grouping AST_VAR_DECLs
        if (proc_main_block_node->child_count > 0 && proc_main_block_node->children[0] &&
            proc_main_block_node->children[0]->type == AST_COMPOUND) {
            bool only_vars = proc_main_block_node->children[0]->child_count > 0;
            for(int k=0; k < proc_main_block_node->children[0]->child_count; ++k) {
                if(!proc_main_block_node->children[0]->children[k] || proc_main_block_node->children[0]->children[k]->type != AST_VAR_DECL) {
                    only_vars = false; break;
                }
            }
            if (only_vars) local_var_decl_compound = proc_main_block_node->children[0];
        }
        
        if (local_var_decl_compound) {
            for (int k=0; k < local_var_decl_compound->child_count; ++k) {
                 AST* var_decl_node = local_var_decl_compound->children[k];
                 if (var_decl_node && var_decl_node->type == AST_VAR_DECL && var_decl_node->token) {
                     locals_in_body_count++;
                 }
            }
            if (proc_symbol) proc_symbol->locals_count = locals_in_body_count;
            #ifdef DEBUG
            if(dumpExec) fprintf(stderr, "L%d: Compiler: Found %d local var declarations for '%s' (TODO: compile as locals).\n",
                    getLine(local_var_decl_compound), locals_in_body_count, func_name);
            #endif
        }

        int body_idx_in_block = (local_var_decl_compound != NULL && proc_main_block_node->child_count > 1) ? 1 : 0;
        if (proc_main_block_node->child_count > body_idx_in_block &&
            proc_main_block_node->children[body_idx_in_block] &&
            proc_main_block_node->children[body_idx_in_block]->type == AST_COMPOUND) {
            body_compound_node = proc_main_block_node->children[body_idx_in_block];
        }
    }

    if (body_compound_node) {
        compileNode(body_compound_node, chunk, getLine(body_compound_node));
    } else { /* warning */ }

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
            }
            break;
        }
        case AST_WHILE: { // From your compiler.c, adapted
            if (!node->left || !node->right) {
                fprintf(stderr, "L%d: Compiler error: WHILE node is missing condition or body.\n", line);
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

        // AST_PROCEDURE_CALL from your original compiler.c might be redundant if AST_PROCEDURE_CALL handles all calls.
        // If AST_PROCEDURE_CALL is specifically for procedures identified by the parser *differently* from functions in expressions:
        case AST_PROCEDURE_CALL: { // From all.txt [cite: 2275]
            bool isQualifiedCall = false; // DECLARE and initialize
            const char* functionName = NULL;
            // AST* calleeNode = node->left; // This was from an older thought, parser uses node->token and node->left for qualifier

            if (node->token && node->token->value) {
                functionName = node->token->value; // This is the procedure/function name itself
                // Check if there's a qualifier (e.g., unit name) in node->left
                if (node->left && node->left->type == AST_VARIABLE && node->left->token) {
                    isQualifiedCall = true; // <--- CORRECTLY SET HERE if qualified
                }
            } else {
                 // This case implies an AST_PROCEDURE_CALL node without a primary token,
                 // which would be unusual for how the parser likely builds it.
                 // The parser, even for qualified calls unit.func, should put "func" in node->token.
                 fprintf(stderr, "L%d: Compiler error: Invalid callee in AST_PROCEDURE_CALL (missing token).\n", line);
                 writeBytecodeChunk(chunk, OP_CONSTANT, line);
                 writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk,makeNil()), line);
                 break;
            }
            
            uint8_t arg_count = 0;
            // Arguments for AST_PROCEDURE_CALL are direct children
            if (node->child_count > 0 && node->children) {
                 for (int i = 0; i < node->child_count; i++) {
                    if (node->children[i]) {
                        compileExpression(node->children[i], chunk, getLine(node->children[i]));
                        arg_count++;
                    }
                }
            }

            if (strcasecmp(functionName, "quitrequested") == 0 && !isQualifiedCall) { // quitrequested is not qualified
                if (arg_count > 0) { fprintf(stderr, "L%d: Compiler error: QuitRequested() expects no arguments.\n", line); }
                writeBytecodeChunk(chunk, OP_CALL_HOST, line);
                writeBytecodeChunk(chunk, (uint8_t)HOST_FN_QUIT_REQUESTED, line);
            } else if (isBuiltin(functionName) && !isQualifiedCall) { // built-ins are typically not qualified in user code
                int nameIndex = addConstantToChunk(chunk, makeString(functionName));
                writeBytecodeChunk(chunk, OP_CALL_BUILTIN, line);
                writeBytecodeChunk(chunk, (uint8_t)nameIndex, line);
                writeBytecodeChunk(chunk, arg_count, line);
            } else { // User-defined function call (can be qualified or unqualified)
                char lookup_name[MAX_SYMBOL_LENGTH * 2 + 2]; // Buffer for "unit.proc" or "proc"
                if (isQualifiedCall) {
                    // node->left is AST_VARIABLE "unit", node->token->value is "proc"
                    snprintf(lookup_name, sizeof(lookup_name), "%s.%s", node->left->token->value, functionName);
                } else {
                    strncpy(lookup_name, functionName, sizeof(lookup_name) -1);
                    lookup_name[sizeof(lookup_name)-1] = '\0';
                }
                toLowerString(lookup_name); // Normalize for lookup

                Symbol* func_symbol = lookupSymbolIn(procedure_table, lookup_name);
                
                if (func_symbol && func_symbol->is_defined) {
                    if (func_symbol->type == TYPE_VOID && !isQualifiedCall) { // Procedure used as function (error if unqualified, units might have procs returning implicit results sometimes)
                        fprintf(stderr, "L%d: Compiler Error: Procedure '%s' cannot be used as a function in an expression.\n", line, functionName);
                        for(uint8_t i=0; i<arg_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                        writeBytecodeChunk(chunk, OP_CONSTANT, line);
                        writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk, makeNil()), line);
                    } else {
                        // If OP_CALL is not yet defined in bytecode.h, this will error later.
                        // Assuming OP_CALL has been added to OpCode enum as per previous discussion.
                        writeBytecodeChunk(chunk, OP_CALL, line);
                        emitShort(chunk, (uint16_t)func_symbol->bytecode_address, line);
                        writeBytecodeChunk(chunk, func_symbol->arity, line); // Send declared arity
                    }
                } else {
                    fprintf(stderr, "L%d: Compiler Error: Undefined function '%s' (lookup: '%s') called in expression.\n", line, functionName, lookup_name);
                    for(uint8_t i=0; i<arg_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                    writeBytecodeChunk(chunk, OP_CONSTANT, line);
                    writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk, makeNil()), line);
                }
            }
            break;
        }
        
        default:
            // Fallback for unhandled statement types. Could try compiling as expression and popping.
            // This section needs to be robust based on what AST types can appear as statements.
            if (node->type >= AST_NUMBER && node->type <= AST_NIL) { // Heuristic for expression types
                 fprintf(stderr, "L%d: Compiler Info: Treating AST node %s as expression statement.\n", line, astTypeToString(node->type));
                 compileExpression(node, chunk, line);
                 writeBytecodeChunk(chunk, OP_POP, line); // Pop result of expression used as statement
            } else {
                 fprintf(stderr, "L%d: Compiler warning: Unhandled AST node type %s in compileStatement.\n", line, astTypeToString(node->type));
            }
            break;
    }
}

static void compileExpression(AST* node, BytecodeChunk* chunk, int current_line_approx) {
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
                // Check if it's a known parameterless function first (like QuitRequested, if syntax allows no parens)
                // This depends on how your parser and type annotator identify such calls.
                // For now, assume QuitRequested always parsed as AST_PROCEDURE_CALL.
                
                int nameIndex = addConstantToChunk(chunk, makeString(varName));
                writeBytecodeChunk(chunk, OP_GET_GLOBAL, line);
                writeBytecodeChunk(chunk, (uint8_t)nameIndex, line);
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
                 break;
            }
             // node->i_val should be set by parser: 1 for true, 0 for false
            Value boolConst = makeBoolean(node->i_val != 0);
            int constIndex = addConstantToChunk(chunk, boolConst);
            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, (uint8_t)constIndex, line);
            break;
        }
        
        case AST_PROCEDURE_CALL: { // AST_PROCEDURE_CALL  is in your types.h
            AST* calleeNode = node->left;
            const char* functionName = NULL;

            if (calleeNode && calleeNode->type == AST_VARIABLE && calleeNode->token) {
                functionName = calleeNode->token->value;
            } else {
                fprintf(stderr, "L%d: Compiler error: Invalid callee in AST_PROCEDURE_CALL.\n", line);
                writeBytecodeChunk(chunk, OP_CONSTANT, line); writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk,makeNil()), line);
                break;
            }
            
            uint8_t arg_count = 0;
            AST* arg_list_node = node->right; // AST_COMPOUND node
            if (arg_list_node && arg_list_node->type == AST_COMPOUND) { // AST_COMPOUND is in your types.h
                 for (int i = 0; i < arg_list_node->child_count; i++) {
                    compileExpression(arg_list_node->children[i], chunk, getLine(arg_list_node->children[i]));
                    arg_count++;
                }
            } else if (arg_list_node != NULL) {
                 fprintf(stderr, "L%d: Compiler warning: Expected AST_COMPOUND for call to '%s', got %s.\n", line, functionName, astTypeToString(arg_list_node->type));
            }

            if (strcasecmp(functionName, "quitrequested") == 0) {
                if (arg_count > 0) { fprintf(stderr, "L%d: Compiler error: QuitRequested() expects no arguments.\n", line); }
                writeBytecodeChunk(chunk, OP_CALL_HOST, line);
                writeBytecodeChunk(chunk, (uint8_t)HOST_FN_QUIT_REQUESTED, line);
            } else if (isBuiltin(functionName)) {
                int nameIndex = addConstantToChunk(chunk, makeString(functionName));
                writeBytecodeChunk(chunk, OP_CALL_BUILTIN, line);
                writeBytecodeChunk(chunk, (uint8_t)nameIndex, line);
                writeBytecodeChunk(chunk, arg_count, line);
            } else {
                fprintf(stderr, "L%d: Compiler: User-defined call to function '%s' (args: %d) - OP_CALL emission placeholder.\n", line, functionName, arg_count);
                // Push a NIL placeholder for the function's return value.
                // This is essential for expressions expecting a value.
                writeBytecodeChunk(chunk, OP_CONSTANT, line);
                writeBytecodeChunk(chunk, (uint8_t)addConstantToChunk(chunk, makeNil()), line);
            }
            break;
        }
        // AST_PROCEDURE_CALL is typically a statement. If your parser also uses it for functions in expressions,
        // its logic would be very similar to AST_PROCEDURE_CALL above.
        // For now, assuming functions in expressions are AST_PROCEDURE_CALL.
        
        default:
            fprintf(stderr, "L%d: Compiler warning: Unhandled AST node type %s in compileExpression.\n", line, astTypeToString(node->type));
            int dummyIdx = addConstantToChunk(chunk, makeInt(0)); // Push dummy 0 for expression context
            writeBytecodeChunk(chunk, OP_CONSTANT, line);
            writeBytecodeChunk(chunk, (uint8_t)dummyIdx, line);
            break;
    }
}
