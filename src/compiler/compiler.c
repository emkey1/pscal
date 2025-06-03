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
#include "backend_ast/interpreter.h" // For makeCopyOfValue()

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

typedef struct {
    char* name;
    Value value;
    // bool is_evaluated; // Could add if constants can be expressions that need one-time eval by compiler
} CompilerConstant;

#define MAX_COMPILER_CONSTANTS 128 // Adjust as needed
CompilerConstant compilerConstants[MAX_COMPILER_CONSTANTS];
int compilerConstantCount = 0;

// Helper to add a constant during compilation
static void addCompilerConstant(const char* name, Value value, int line) {
    if (compilerConstantCount >= MAX_COMPILER_CONSTANTS) {
        fprintf(stderr, "L%d: Compiler error: Too many compile-time constants.\n", line);
        // Consider not exiting directly but setting an error flag for compileASTToBytecode to return false
        exit(1);
    }
    for (int i = 0; i < compilerConstantCount; i++) {
        if (strcmp(compilerConstants[i].name, name) == 0) {
            fprintf(stderr, "L%d: Compiler warning: Constant '%s' redefined. Using new value.\n", line, name);
            // Free old value if it was heap-allocated (e.g., string)
            freeValue(&compilerConstants[i].value);
            compilerConstants[i].value = makeCopyOfValue(&value); // Store a copy
            return;
        }
    }
    compilerConstants[compilerConstantCount].name = strdup(name);
    if (!compilerConstants[compilerConstantCount].name) { /* Malloc error */ exit(1); }
    compilerConstants[compilerConstantCount].value = makeCopyOfValue(&value); // Store a copy
    compilerConstantCount++;
}

/*
// Helper to find a compile-time constant
static Value* findCompilerConstant(const char* name) {
    for (int i = 0; i < compilerConstantCount; i++) {
        if (strcmp(compilerConstants[i].name, name) == 0) {
            return &compilerConstants[i].value;
        }
    }
    return NULL;
} */

// Reset for each compilation
static void resetCompilerConstants(void) {
    for (int i = 0; i < compilerConstantCount; i++) {
        if (compilerConstants[i].name) {
            free(compilerConstants[i].name);
            compilerConstants[i].name = NULL;
        }
        freeValue(&compilerConstants[i].value); // Free any heap-allocated data within the Value
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
    // ... (initial null checks, initBytecodeChunk, reset compilerGlobalCount) ...
    if (!rootNode || !outputChunk) { /* ... */ return false; }
    
    initBytecodeChunk(outputChunk);
    compilerGlobalCount = 0; // For global variable indexing by compiler
    // compiler_had_error = false; // If you use a global error flag

    resetCompilerConstants();

    // This ensures constants are known before they are used in other declarations or the main body.
    // This simplified pre-pass assumes global constants are direct children of the main program block's declaration section.
    if (rootNode->type == AST_PROGRAM && rootNode->right && rootNode->right->type == AST_BLOCK) {
        AST* mainBlock = rootNode->right;
        // mainBlock->children[0] is usually the AST_COMPOUND node for all declarations (const, type, var, proc/func)
        if (mainBlock->child_count > 0 && mainBlock->children[0] && mainBlock->children[0]->type == AST_COMPOUND) {
            AST* declarations_compound_node = mainBlock->children[0];
            for (int i = 0; i < declarations_compound_node->child_count; i++) {
                AST* declNode = declarations_compound_node->children[i];
                if (declNode && declNode->type == AST_CONST_DECL) {
                    // Call compileNode specifically for AST_CONST_DECL.
                    // It won't emit bytecode but will call addCompilerConstant.
                    // Pass NULL for 'chunk' as we are not emitting to the main chunk in this pre-pass.
                    // Or, ensure compileNode's AST_CONST_DECL case handles a NULL chunk gracefully.
                    // For simplicity here, let's make sure the AST_CONST_DECL case in compileNode
                    // *only* calls addCompilerConstant and doesn't write to the chunk.
                    compileNode(declNode, NULL /* No bytecode emission during this pre-pass */, getLine(declNode));
                }
            }
        }
    }

    // Main compilation pass (unchanged from your structure)
    if (rootNode->type == AST_PROGRAM && rootNode->right && rootNode->right->type == AST_BLOCK) {
        AST* mainBlock = rootNode->right;
        compileNode(mainBlock, outputChunk, getLine(rootNode));
    } else if (rootNode->type == AST_BLOCK) {
        compileNode(rootNode, outputChunk, getLine(rootNode));
    } else {
        fprintf(stderr, "Compiler error: Expected AST_PROGRAM or AST_BLOCK node as root for compilation.\n");
        return false;
    }

    writeBytecodeChunk(outputChunk, OP_HALT, getLine(rootNode));
    // return !compiler_had_error; // If using a global error flag
    return true;
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
            // This is called by the pre-pass in compileASTToBytecode for global constants
            // And could be called for local constants if a similar pre-pass is done for procedures.
            if (node->token && node->left) { // Const name and value node
                // The parser should have evaluated constant expressions already.
                // node->left should be an AST_NUMBER, AST_STRING, AST_BOOLEAN, or AST_NIL
                // For this example, we assume simple literal constants.
                // A more robust solution would use eval_constant_expression from the parser if needed.
                Value constantVal;
                bool val_set = false;
                if (node->left->type == AST_NUMBER) {
                    if (node->left->var_type == TYPE_REAL || (node->left->token && strchr(node->left->token->value, '.'))) {
                        constantVal = makeReal(atof(node->left->token->value));
                    } else {
                        constantVal = makeInt(atoll(node->left->token->value));
                    }
                    val_set = true;
                } else if (node->left->type == AST_STRING) {
                    constantVal = makeString(node->left->token->value); // makeString copies
                    val_set = true;
                } else if (node->left->type == AST_BOOLEAN) {
                    constantVal = makeBoolean(node->left->i_val);
                    val_set = true;
                } else if (node->left->type == AST_NIL) {
                    constantVal = makeNil();
                    val_set = true;
                }
                 else if (node->left->type == AST_VARIABLE && node->left->token && (node->left->token->value[0] == '\'' || node->left->token->value[0] == '#')) {
                    // Handle char constants like '#65' or 'A' (which parser might make AST_VARIABLE with string value)
                    if (node->left->token->value[0] == '#') { // e.g. #65
                        long long char_ord = atoll(node->left->token->value + 1);
                        constantVal = makeChar((char)char_ord);
                    } else { // e.g. 'A'
                         constantVal = makeChar(node->left->token->value[1]);
                    }
                    val_set = true;
                }
                // TODO: Handle constant expressions if parser doesn't fully resolve them to literals in AST_CONST_DECL's value node
                // For now, we assume node->left is a literal or already evaluated by parser.

                if (val_set) {
                    addCompilerConstant(node->token->value, constantVal, getLine(node));
                    // No bytecode for CONST declaration itself, values are inlined/pushed when used.
                    // Crucially, do not free constantVal here if it's a string, as makeCopyOfValue was used.
                    if(constantVal.type == TYPE_STRING) freeValue(&constantVal); // Free the temporary string if makeString was used directly
                } else {
                     fprintf(stderr, "L%d: Compiler Info: CONST_DECL for '%s' has non-literal value (Type: %s). Complex const eval TBD.\n",
                             getLine(node), node->token->value, astTypeToString(node->left->type));
                }
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
                fprintf(stderr, "L%d: Compiler Error: Procedure/Function declaration node missing name token(compileNode).\n", line);
                compiler_had_error = true;
                break;
            }
            #ifdef DEBUG
            if(dumpExec) fprintf(stderr, "L%d: Compiler: Processing definition of %s '%s'. Emitting jump over body.\n",
                    line, astTypeToString(node->type), node->token->value);
            #endif

            // Emit a jump to skip over the body of the function/procedure.
            // The actual calls will jump to func_bytecode_start_address.
            writeBytecodeChunk(chunk, OP_JUMP, line);
            int jump_over_body_operand_offset = chunk->count; // Store offset of the placeholder
            emitShort(chunk, 0xFFFF, line); // Placeholder for jump offset

            // Compile the function/procedure body itself.
            compileDefinedFunction(node, chunk, line); // This sets proc_symbol->bytecode_address and is_defined

            // Backpatch the jump: calculate offset to jump from after placeholder to current end of chunk.
            uint16_t offset_to_skip_body = (uint16_t)(chunk->count - (jump_over_body_operand_offset + 2)); // +2 for the short itself
            patchShort(chunk, jump_over_body_operand_offset, offset_to_skip_body);
            
            #ifdef DEBUG
            if(dumpExec) fprintf(stderr, "L%d: Compiler: Patched jump over body for '%s'. Jump offset: %u. Current chunk count: %d\n",
                    line, node->token->value, offset_to_skip_body, chunk->count);
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

            // 2. Handle the call itself based on whether it's a void procedure or a function.
            //    This relies on node->var_type being correctly set by annotateTypes.
            if (node->var_type == TYPE_VOID) {
                // It's a true procedure (returns void).
                if (node->token && isBuiltin(node->token->value)) {
                    int builtinId = getBuiltinIDForCompiler(node->token->value); // From builtin.c/h
                    if (builtinId != -1) {
                        writeBytecodeChunk(chunk, OP_CALL_BUILTIN_PROC, call_line);
                        writeBytecodeChunk(chunk, (uint8_t)builtinId, call_line);      // Operand 1: builtin_id
                        writeBytecodeChunk(chunk, (uint8_t)node->child_count, call_line); // Operand 2: arg_count
                        // fprintf(stderr, "L%d: Compiler: Emitted OP_CALL_BUILTIN_PROC for '%s' (ID: %d, Args: %d).\n", call_line, node->token->value, builtinId, node->child_count);
                    } else {
                        fprintf(stderr, "L%d: Compiler Error: Built-in procedure '%s' (marked void) not found by getBuiltinIDForCompiler(). Review builtin table and ID generation.\n", call_line, node->token->value);
                        // This indicates an inconsistency or missing entry in your builtin ID mapping.
                    }
                } else if (node->token) { // User-defined procedure
                    int nameConstIdx = addConstantToChunk(chunk, makeString(node->token->value));
                    writeBytecodeChunk(chunk, OP_CALL_USER_PROC, call_line);
                    writeBytecodeChunk(chunk, (uint8_t)nameConstIdx, call_line);   // Operand 1: name_const_idx
                    writeBytecodeChunk(chunk, (uint8_t)node->child_count, call_line); // Operand 2: arg_count
                    // fprintf(stderr, "L%d: Compiler: Emitted OP_CALL_USER_PROC for '%s' (NameIdx: %d, Args: %d).\n", call_line, node->token->value, nameConstIdx, node->child_count);
                } else {
                     fprintf(stderr, "L%d: Compiler Error: AST_PROCEDURE_CALL node missing token for procedure name (void path).\n", call_line);
                }
            } else {
                // It's a function call used as a statement, so its result must be popped.
                // compileExpression is responsible for emitting the function call opcodes
                // and ensuring the result is left on the stack.
                // fprintf(stderr, "L%d: Compiler: Function '%s' (type %s) called as statement. Compiling call and popping result.\n", call_line, node->token ? node->token->value : "unknown_func", varTypeToString(node->var_type));

                // IMPORTANT: compileExpression for AST_PROCEDURE_CALL needs to be fully implemented
                // to handle function calls correctly. The current placeholder in your shared code
                // just pushes a dummy constant.
                compileExpression(node, chunk, call_line); // This should compile the function call.
                                                           // Arguments were already compiled above.
                                                           // compileExpression for AST_PROCEDURE_CALL should ideally just emit the CALL part.

                writeBytecodeChunk(chunk, OP_POP, call_line); // Pop the ignored result.
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
