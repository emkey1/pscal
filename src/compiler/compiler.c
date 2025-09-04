#include <stdio.h>
#include <stdlib.h>
#include <string.h> // For strcmp, strdup, atoll
#include <math.h>
#include <ctype.h>

#include "compiler/compiler.h"
#include "backend_ast/builtin.h" // For isBuiltin
#include "core/utils.h"
#include "core/types.h"
#include "ast/ast.h"
#include "symbol/symbol.h" // For access to the main global symbol table, if needed,
                           // though for bytecode compilation, we often build our own tables/mappings.
#include "vm/vm.h"         // For HostFunctionID
#include "compiler/bytecode.h"

#define MAX_GLOBALS 256 // Define a reasonable limit for global variables for now

static bool compiler_had_error = false;
static const char* current_compilation_unit_name = NULL;

typedef struct {
    char* name;
    int depth; // Scope depth
    bool is_ref;
    bool is_captured;
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
    uint8_t index;
    bool isLocal;
    bool is_ref;
} CompilerUpvalue;

#define MAX_UPVALUES 256

typedef struct FunctionCompilerState {
    CompilerLocal locals[MAX_GLOBALS]; // Re-use MAX_GLOBALS for max locals per function
    int local_count;
    int scope_depth;
    const char* name;
    struct FunctionCompilerState* enclosing;
    Symbol* function_symbol;
    CompilerUpvalue upvalues[MAX_UPVALUES];
    int upvalue_count;
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

static void emitConstant(BytecodeChunk* chunk, int constant_index, int line) {
    if (constant_index < 0) {
        fprintf(stderr, "L%d: Compiler error: negative constant index.\n", line);
        compiler_had_error = true;
        return;
    }
    if (constant_index <= 0xFF) {
        writeBytecodeChunk(chunk, OP_CONSTANT, line);
        writeBytecodeChunk(chunk, (uint8_t)constant_index, line);
    } else if (constant_index <= 0xFFFF) {
        writeBytecodeChunk(chunk, OP_CONSTANT16, line);
        emitShort(chunk, (uint16_t)constant_index, line);
    } else {
        fprintf(stderr, "L%d: Compiler error: too many constants (%d). Limit is 65535.\n",
                line, constant_index);
        compiler_had_error = true;
    }
}

// Emits a 16-bit constant pool index without selecting an opcode.
static void emitConstantIndex16(BytecodeChunk* chunk, int constant_index, int line) {
    if (constant_index < 0 || constant_index > 0xFFFF) {
        fprintf(stderr, "L%d: Compiler error: constant index out of range (%d).\n", line, constant_index);
        compiler_had_error = true;
        return;
    }
    emitShort(chunk, (uint16_t)constant_index, line);
}

// Helper to emit global-variable opcodes that take a name index operand.
// Selects 8-bit or 16-bit variants based on the index value.
static void emitGlobalNameIdx(BytecodeChunk* chunk, OpCode op8, OpCode op16,
                              int name_idx, int line) {
    if (name_idx < 0) {
        fprintf(stderr, "L%d: Compiler error: negative name index.\n", line);
        compiler_had_error = true;
        return;
    }
    if (name_idx <= 0xFF) {
        writeBytecodeChunk(chunk, op8, line);
        writeBytecodeChunk(chunk, (uint8_t)name_idx, line);
    } else if (name_idx <= 0xFFFF) {
        writeBytecodeChunk(chunk, op16, line);
        emitShort(chunk, (uint16_t)name_idx, line);
    } else {
        fprintf(stderr, "L%d: Compiler error: too many constants (%d). Limit is 65535.\n",
                line, name_idx);
        compiler_had_error = true;
    }
}

// Helper to emit OP_DEFINE_GLOBAL or OP_DEFINE_GLOBAL16 depending on index size.
static void emitDefineGlobal(BytecodeChunk* chunk, int name_idx, int line) {
    emitGlobalNameIdx(chunk, OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL16, name_idx, line);
}

// Resolve type references to their concrete definitions.
static AST* resolveTypeAlias(AST* type_node) {
    while (type_node && type_node->type == AST_TYPE_REFERENCE && type_node->token && type_node->token->value) {
        AST* looked = lookupType(type_node->token->value);
        if (!looked || looked == type_node) break;
        type_node = looked;
    }
    return type_node;
}

// Compare two type AST nodes structurally.
static bool compareTypeNodes(AST* a, AST* b) {
    a = resolveTypeAlias(a);
    b = resolveTypeAlias(b);
    if (!a || !b) return a == b;
    if (a->var_type != b->var_type) return false;
    switch (a->var_type) {
        case TYPE_ARRAY:
            // If the parameter's array has unspecified bounds (no children),
            // skip comparing bounds and only ensure the element types match.
            if (a->child_count == 0 || !a->children) {
                return compareTypeNodes(a->right, b->right);
            }

            if (a->child_count != b->child_count) return false;
            for (int i = 0; i < a->child_count; i++) {
                AST* ar = a->children[i];
                AST* br = b->children[i];
                if (!ar || !br || !ar->left || !br->left || !ar->right || !br->right) return false;
                if (ar->left->i_val != br->left->i_val || ar->right->i_val != br->right->i_val) return false;
            }
            return compareTypeNodes(a->right, b->right);
        case TYPE_RECORD:
            if (a->child_count != b->child_count) return false;
            for (int i = 0; i < a->child_count; i++) {
                AST* af = a->children[i];
                AST* bf = b->children[i];
                if (!af || !bf || af->child_count == 0 || bf->child_count == 0) return false;
                const char* an = af->children[0]->token ? af->children[0]->token->value : NULL;
                const char* bn = bf->children[0]->token ? bf->children[0]->token->value : NULL;
                if ((an && bn && strcasecmp(an, bn) != 0) || (an && !bn) || (!an && bn)) return false;
                if (!compareTypeNodes(af->right, bf->right)) return false;
            }
            return true;
        case TYPE_POINTER:
            return compareTypeNodes(a->right, b->right);
        default:
            return true;
    }
}

// Determine if an argument node's type matches the full parameter type node.
//
// Both sides may reference type aliases, so we resolve them before comparison.
// `arg_node->type_def` provides the full type of the argument (including any
// array structure) which allows for structural comparisons, especially when
// checking VAR parameters that are themselves arrays.
static bool typesMatch(AST* param_type, AST* arg_node, bool allow_coercion) {
    if (!param_type || !arg_node) return false;

    AST* param_actual = resolveTypeAlias(param_type);
    if (!param_actual) return false;

    // Resolve the argument's actual type as well.  The argument node carries a
    // full type definition in `type_def`, which may itself be a type alias.
    AST* arg_actual = resolveTypeAlias(arg_node->type_def);
    VarType arg_vt = arg_actual ? arg_actual->var_type : arg_node->var_type;

    // When coercion is not allowed, require an exact match of the base types
    // before proceeding with any structural comparisons. Allow NIL for pointer
    // parameters as a special case.
    if (!allow_coercion) {
        if (param_actual->var_type != arg_vt) {
            if (param_actual->var_type == TYPE_POINTER && arg_vt == TYPE_NIL) {
                return true;
            }
            // Treat CHAR and STRING as interchangeable without requiring
            // coercion.  A CHAR is effectively a single-character STRING.
            if ((param_actual->var_type == TYPE_STRING && arg_vt == TYPE_CHAR) ||
                (param_actual->var_type == TYPE_CHAR   && arg_vt == TYPE_STRING)) {
                return true;
            }
            // Allow implicit narrowing from wider ordinal types to BYTE.
            if (param_actual->var_type == TYPE_BYTE &&
                (arg_vt == TYPE_INTEGER || arg_vt == TYPE_WORD ||
                 arg_vt == TYPE_ENUM    || arg_vt == TYPE_CHAR)) {
                return true;
            }
            // Permit wider integer arguments (e.g. LONGINT/INT64, CARDINAL/UINT32)
            // to match INTEGER parameters.  Traditional Pascal routinely allows
            // these values to be passed to routines expecting INTEGER so long as
            // the caller explicitly requests it.  Our VM stores the value's
            // actual type in the Value struct, so the callee will still receive
            // the full precision.
            if (param_actual->var_type == TYPE_INTEGER &&
                (arg_vt == TYPE_INT64 || arg_vt == TYPE_UINT64 ||
                 arg_vt == TYPE_UINT32)) {
                return true;
            }
            if (isRealType(param_actual->var_type) && isIntlikeType(arg_vt)) {
                return true;
            }
            return false;
        }
    } else if (!arg_actual) {
        /*
         * Many argument nodes – particularly literals and computed
         * expressions like `n + 1` – do not carry a full type
         * definition in `type_def`.  In those cases we can fall back to
         * the simple `var_type` annotation that `annotateTypes` already
         * provides.  This is sufficient for primitive types such as
         * integers, reals, booleans, chars and strings.  Reject more
         * complex types (arrays, records, pointers, sets) if we lack a
         * structural type definition to compare against.
         */
        switch (param_actual->var_type) {
            case TYPE_INTEGER:
                return arg_vt == TYPE_INTEGER || arg_vt == TYPE_BYTE ||
                       arg_vt == TYPE_WORD    || arg_vt == TYPE_ENUM ||
                       arg_vt == TYPE_CHAR;
            case TYPE_REAL:
                return arg_vt == TYPE_REAL   || arg_vt == TYPE_INTEGER ||
                       arg_vt == TYPE_BYTE   || arg_vt == TYPE_WORD    ||
                       arg_vt == TYPE_ENUM   || arg_vt == TYPE_CHAR;
            case TYPE_CHAR:
                return arg_vt == TYPE_CHAR   || arg_vt == TYPE_INTEGER ||
                       arg_vt == TYPE_BYTE   || arg_vt == TYPE_WORD;
            case TYPE_POINTER:
                if (arg_vt != TYPE_POINTER && arg_vt != TYPE_NIL) return false;
                // If the parameter specifies no referenced subtype, accept any pointer.
                return param_actual->right == NULL;
            case TYPE_STRING:
                return arg_vt == TYPE_STRING || arg_vt == TYPE_CHAR;
            case TYPE_BOOLEAN:
            case TYPE_BYTE:
            case TYPE_ENUM:
            case TYPE_FILE:
            case TYPE_MEMORYSTREAM:
            case TYPE_NIL:
                return param_actual->var_type == arg_vt;
            case TYPE_WORD:
                return arg_vt == TYPE_WORD || arg_vt == TYPE_INTEGER ||
                       arg_vt == TYPE_BYTE  || arg_vt == TYPE_ENUM   ||
                       arg_vt == TYPE_CHAR;
            default:
                return false; // Need structural info for arrays/records/etc.
        }
    }

    // Arrays require structural comparison via compareTypeNodes. This allows
    // open-array parameters (with unspecified bounds) to accept arrays of any
    // bound as long as the element types match.
    if (param_actual->var_type == TYPE_ARRAY) {
        if (arg_vt != TYPE_ARRAY) return false;
        return compareTypeNodes(param_actual, arg_actual);
    }

    if (param_actual->var_type == TYPE_RECORD) {
        if (arg_vt != TYPE_RECORD) return false;
        return compareTypeNodes(param_actual, arg_actual);
    }

    if (param_actual->var_type == TYPE_POINTER) {
        if (arg_vt != TYPE_POINTER && arg_vt != TYPE_NIL) return false;
        if (!param_actual->right) return true; // Generic pointer accepts any pointer
        if (!arg_actual) return false;
        return compareTypeNodes(param_actual, arg_actual);
    }

    if (param_actual->var_type == TYPE_ENUM && arg_vt == TYPE_ENUM) {
        /*
         * Both sides are enums.  Ensure they refer to the same declared
         * enumeration type.  `resolveTypeAlias` gives us the underlying
         * AST node for each enum definition, so pointer comparison (or
         * name comparison as a fallback) suffices.
         */
        AST* param_enum = resolveTypeAlias(param_actual);
        AST* arg_enum   = resolveTypeAlias(arg_actual);
        if (param_enum && arg_enum) {
            if (param_enum == arg_enum) return true;
            const char* pname = param_enum->token ? param_enum->token->value : NULL;
            const char* aname = arg_enum->token ? arg_enum->token->value : NULL;
            if (pname && aname && strcasecmp(pname, aname) == 0) return true;
            return false;
        }
        return true; // If we lack type defs, fall back to base match
    }

    if (allow_coercion) {
        // Apply basic promotion rules when both sides have concrete types.
        switch (param_actual->var_type) {
            case TYPE_INTEGER:
                if (arg_vt == TYPE_BYTE || arg_vt == TYPE_WORD ||
                    arg_vt == TYPE_ENUM || arg_vt == TYPE_CHAR)
                    return true;
                break;
            case TYPE_REAL:
                if (arg_vt == TYPE_INTEGER || arg_vt == TYPE_BYTE ||
                    arg_vt == TYPE_WORD || arg_vt == TYPE_ENUM ||
                    arg_vt == TYPE_CHAR)
                    return true;
                break;
            case TYPE_CHAR:
                if (arg_vt == TYPE_BYTE || arg_vt == TYPE_WORD)
                    return true;
                break;
            case TYPE_STRING:
                if (arg_vt == TYPE_CHAR)
                    return true;
                break;
            default:
                break;
        }
    }

    return param_actual->var_type == arg_vt;
}

// --- Forward Declarations for Recursive Compilation ---
static void compileNode(AST* node, BytecodeChunk* chunk, int current_line_approx);
static void compileRValue(AST* node, BytecodeChunk* chunk, int current_line_approx);
static void compileStatement(AST* node, BytecodeChunk* chunk, int current_line_approx);
static void compileLValue(AST* node, BytecodeChunk* chunk, int current_line_approx);
static void compileDefinedFunction(AST* func_decl_node, BytecodeChunk* chunk, int line);
static void compileInlineRoutine(Symbol* proc_symbol, AST* call_node, BytecodeChunk* chunk, int line, bool push_result);
static void compilePrintf(AST* callNode, BytecodeChunk* chunk, int line);

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
    fc->enclosing = NULL;
    fc->function_symbol = NULL;
    fc->upvalue_count = 0;
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
    int* temp = realloc(current_loop->break_jumps, sizeof(int) * current_loop->break_count);
    if (!temp) {
        fprintf(stderr, "L%d: Compiler error: memory allocation failed for break jumps.\\n", line);
        compiler_had_error = true;
        return;
    }
    current_loop->break_jumps = temp;

    writeBytecodeChunk(chunk, OP_JUMP, line);
    current_loop->break_jumps[current_loop->break_count - 1] = chunk->count; // Store offset of the operand
    emitShort(chunk, 0xFFFF, line); // Placeholder
}

static void patchBreaks(BytecodeChunk* chunk) {
    if (loop_depth < 0) return;
    Loop* current_loop = &loop_stack[loop_depth];
    int jump_target = chunk->count;

    for (int i = 0; i < current_loop->break_count; i++) {
        int jump_offset = current_loop->break_jumps[i];
        patchShort(chunk, jump_offset, (uint16_t)(jump_target - (jump_offset + 2)));
    }

    if (current_loop->break_jumps) {
        free(current_loop->break_jumps);
        
        current_loop->break_jumps = NULL;
    }
}

static void endLoop(void) {
    if (loop_depth < 0) return;

    // This function now only manages the loop depth.
    // The patching and freeing of break_jumps is handled entirely by patchBreaks().
    // A check has been added to catch logic errors where endLoop is called
    // without a preceding patchBreaks() call.
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
    local->is_captured = false;
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

static int addUpvalue(FunctionCompilerState* fc, uint8_t index, bool isLocal, bool is_ref) {
    for (int i = 0; i < fc->upvalue_count; i++) {
        CompilerUpvalue* up = &fc->upvalues[i];
        if (up->index == index && up->isLocal == isLocal) {
            return i;
        }
    }
    if (fc->upvalue_count >= MAX_UPVALUES) {
        fprintf(stderr, "Compiler error: Too many upvalues in function.\n");
        compiler_had_error = true;
        return 0;
    }
    fc->upvalues[fc->upvalue_count].index = index;
    fc->upvalues[fc->upvalue_count].isLocal = isLocal;
    fc->upvalues[fc->upvalue_count].is_ref = is_ref;
    return fc->upvalue_count++;
}

static int resolveUpvalue(FunctionCompilerState* fc, const char* name) {
    if (!fc->enclosing) return -1;

    int localIndex = resolveLocal(fc->enclosing, name);
    if (localIndex != -1) {
        fc->enclosing->locals[localIndex].is_captured = true;
        bool is_ref = fc->enclosing->locals[localIndex].is_ref;
        return addUpvalue(fc, (uint8_t)localIndex, true, is_ref);
    }

    int upvalueIndex = resolveUpvalue(fc->enclosing, name);
    if (upvalueIndex != -1) {
        bool is_ref = fc->enclosing->upvalues[upvalueIndex].is_ref;
        return addUpvalue(fc, (uint8_t)upvalueIndex, false, is_ref);
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
            if (node->token && strlen(node->token->value) == 1)
                return makeChar((unsigned char)node->token->value[0]);
            if (node->token) return makeString(node->token->value);
            break;
        case AST_BOOLEAN:
            return makeBoolean(node->i_val);
        case AST_NIL:
            return makeNil();
        case AST_VARIABLE: // Reference to another constant or an enum
            if (node->token && node->token->value) {
                // First, check if it's a defined compile-time constant
                Value* const_val_ptr = findCompilerConstant(node->token->value);
                if (const_val_ptr) {
                    return makeCopyOfValue(const_val_ptr); // Return a copy
                }

                // If not, check the global symbol table for enums
                Symbol* sym = lookupGlobalSymbol(node->token->value);
                if (sym && sym->type == TYPE_ENUM && sym->value) {
                    return makeCopyOfValue(sym->value);
                }
                
                return makeVoid();
            }
            break;
        case AST_PROCEDURE_CALL: {
            if (node->token) {
                char callee_lower[MAX_SYMBOL_LENGTH];
                strncpy(callee_lower, node->token->value, sizeof(callee_lower) - 1);
                callee_lower[sizeof(callee_lower) - 1] = '\0';
                toLowerString(callee_lower);
                if (!lookupProcedure(callee_lower) && isBuiltin(node->token->value)) {
                    const char* funcName = node->token->value;

                    if ((strcasecmp(funcName, "low") == 0 || strcasecmp(funcName, "high") == 0) &&
                        node->child_count == 1 && node->children[0]->type == AST_VARIABLE) {

                        const char* typeName = node->children[0]->token->value;
                        AST* typeDef = lookupType(typeName);

                        if (typeDef) {
                            if (typeDef->type == AST_TYPE_REFERENCE) typeDef = typeDef->right;

                            if (typeDef->type == AST_ENUM_TYPE) {
                                if (strcasecmp(funcName, "low") == 0) {
                                    return makeEnum(typeName, 0);
                                } else { // high
                                    return makeEnum(typeName, typeDef->child_count > 0 ? typeDef->child_count - 1 : 0);
                                }
                            }
                        }
                    } else if (strcasecmp(funcName, "chr") == 0 && node->child_count == 1) {
                        Value arg = evaluateCompileTimeValue(node->children[0]);
                        if (arg.type == TYPE_INTEGER) {
                            Value result = makeChar(arg.i_val);
                            freeValue(&arg);
                            return result;
                        }
                        freeValue(&arg);
                    } else if (strcasecmp(funcName, "ord") == 0 && node->child_count == 1) {
                        Value arg = evaluateCompileTimeValue(node->children[0]);
                        Value result = makeVoid();
                        if (arg.type == TYPE_CHAR) {
                            result = makeInt(arg.c_val);
                        } else if (arg.type == TYPE_BOOLEAN) {
                            result = makeInt(arg.i_val ? 1 : 0);
                        } else if (arg.type == TYPE_ENUM) {
                            result = makeInt(arg.enum_val.ordinal);
                        }
                        freeValue(&arg);
                        if (result.type != TYPE_VOID) return result;
                    }
                }
            }
            break; // Fall through to makeVoid if not a recognized compile-time function
        }
        case AST_BINARY_OP:
            if (node->left && node->right && node->token) {
                Value left_val = evaluateCompileTimeValue(node->left);
                Value right_val = evaluateCompileTimeValue(node->right);

                if (left_val.type == TYPE_VOID || left_val.type == TYPE_UNKNOWN ||
                    right_val.type == TYPE_VOID || right_val.type == TYPE_UNKNOWN) {
                    freeValue(&left_val);
                    freeValue(&right_val);
                    return makeVoid();
                }

                Value result = makeVoid();

                if (isRealType(left_val.type) && isRealType(right_val.type)) {
                    double a = (double)AS_REAL(left_val);
                    double b = (double)AS_REAL(right_val);
                    switch (node->token->type) {
                        case TOKEN_PLUS:
                            result = makeReal(a + b);
                            break;
                        case TOKEN_MINUS:
                            result = makeReal(a - b);
                            break;
                        case TOKEN_MUL:
                            result = makeReal(a * b);
                            break;
                        case TOKEN_SLASH:
                            if (b == 0.0) {
                                fprintf(stderr, "Compile-time Error: Division by zero in constant expression.\n");
                            } else {
                                result = makeReal(a / b);
                            }
                            break;
        case TOKEN_MOD:
                            if (b == 0.0) {
                                fprintf(stderr, "Compile-time Error: Division by zero in constant expression.\n");
                            } else {
                                result = makeReal(fmod(a, b));
                            }
                            break;
                        default:
                            break;
                    }
                } else if (isRealType(left_val.type) || isRealType(right_val.type)) {
                    fprintf(stderr, "Compile-time Error: Mixing real and integer in constant expression.\n");
                } else { // Both operands are integers
                    long long a = left_val.i_val;
                    long long b = right_val.i_val;
                    switch (node->token->type) {
                        case TOKEN_PLUS:
                            result = makeInt(a + b);
                            break;
                        case TOKEN_MINUS:
                            result = makeInt(a - b);
                            break;
                        case TOKEN_MUL:
                            result = makeInt(a * b);
                            break;
                        case TOKEN_SLASH:
                            if (b == 0) {
                                fprintf(stderr, "Compile-time Error: Division by zero in constant expression.\n");
                            } else {
                                result = makeInt(a / b);
                            }
                            break;
                        case TOKEN_INT_DIV:
                            if (b == 0) {
                                fprintf(stderr, "Compile-time Error: Division by zero in constant expression.\n");
                            } else {
                                result = makeInt(a / b);
                            }
                            break;
                        case TOKEN_MOD:
                            if (b == 0) {
                                fprintf(stderr, "Compile-time Error: Division by zero in constant expression.\n");
                            } else {
                                result = makeInt(a % b);
                            }
                            break;
                        default:
                            break;
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
                    } else if (isRealType(operand_val.type)) {
                        double tmp = -(double)AS_REAL(operand_val);
                        freeValue(&operand_val);
                        return makeReal(tmp);
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
            EXIT_FAILURE_HANDLER();
            return -1;
        }
        return compilerGlobalCount++;
    }
    fprintf(stderr, "L%d: Compiler error: Too many global variables.\n", line);
    EXIT_FAILURE_HANDLER();
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
                int upvalue_slot = -1;
                if (current_function_compiler) {
                    upvalue_slot = resolveUpvalue(current_function_compiler, varName);
                }
                if (upvalue_slot != -1) {
                    bool up_is_ref = current_function_compiler->upvalues[upvalue_slot].is_ref;
                    if (up_is_ref) {
                        writeBytecodeChunk(chunk, OP_GET_UPVALUE, line);
                        writeBytecodeChunk(chunk, (uint8_t)upvalue_slot, line);
                    } else {
                        writeBytecodeChunk(chunk, OP_GET_UPVALUE_ADDRESS, line);
                        writeBytecodeChunk(chunk, (uint8_t)upvalue_slot, line);
                    }
                } else {
                    int nameIndex =  addStringConstant(chunk, varName);
                    emitGlobalNameIdx(chunk, OP_GET_GLOBAL_ADDRESS, OP_GET_GLOBAL_ADDRESS16,
                                       nameIndex, line);
                }
            }
            break;
        }
        case AST_FIELD_ACCESS: {
            // Recursively compile the L-Value of the base (e.g., myRec or p^)
            compileLValue(node->left, chunk, getLine(node->left));

            // Now, get the address of the specific field.
            int fieldNameIndex = addStringConstant(chunk, node->token->value);
            if (fieldNameIndex <= 0xFF) {
                writeBytecodeChunk(chunk, OP_GET_FIELD_ADDRESS, line);
                writeBytecodeChunk(chunk, (uint8_t)fieldNameIndex, line);
            } else {
                writeBytecodeChunk(chunk, OP_GET_FIELD_ADDRESS16, line);
                emitShort(chunk, (uint16_t)fieldNameIndex, line);
            }
            break;
        }
        case AST_ARRAY_ACCESS: {
            // Check if the base is a string for special handling
            if (node->left && node->left->var_type == TYPE_STRING) {
                // This is an L-Value access for assignment, like s[i] := 'c'.
                // We need the address of the string variable, then the index.
                compileLValue(node->left, chunk, getLine(node->left));      // Push address of the string variable
                compileRValue(node->children[0], chunk, getLine(node->children[0])); // Push the index value
                writeBytecodeChunk(chunk, OP_GET_CHAR_ADDRESS, line); // CORRECT: Pops both, pushes address of the character
                break; // We are done with this case
            } else {
                // Standard array access: push index expressions first so the
                // array base address ends up on top of the stack.  This order
                // matches OP_GET_ELEMENT_ADDRESS's expectation (it pops the base
                // first, then each index).

                // Compile all index expressions. Their values will be on the stack
                // below the array base.
                for (int i = 0; i < node->child_count; i++) {
                    compileRValue(node->children[i], chunk, getLine(node->children[i]));
                }

                // Finally, push the address of the array variable (Value*).
                compileLValue(node->left, chunk, getLine(node->left));

                // If the base resolves to an upvalue, ensure no extra temporary
                // values remain above the array pointer.  We want the stack to be
                // [..., index, array] before emitting OP_GET_ELEMENT_ADDRESS.
                if (current_function_compiler && node->left &&
                    node->left->type == AST_VARIABLE && node->left->token) {
                    const char* base_name = node->left->token->value;
                    int base_local = resolveLocal(current_function_compiler, base_name);
                    if (base_local == -1) {
                        int base_up = resolveUpvalue(current_function_compiler, base_name);
                        if (base_up != -1) {
                            bool up_is_ref = current_function_compiler->upvalues[base_up].is_ref;
                            if (!up_is_ref) {
                                // Drop any temporary left behind when accessing the upvalue
                                // so only the index and the array pointer remain.
                                writeBytecodeChunk(chunk, OP_SWAP, line);
                                writeBytecodeChunk(chunk, OP_POP, line);
                            }
                        }
                    }
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
    
    current_procedure_table = procedure_table;

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
                // Pass 1: Compile constant and variable declarations from the declaration block.
                for (int i = 0; i < declarations->child_count; i++) {
                    AST* decl_child = declarations->children[i];
                    if (decl_child &&
                        (decl_child->type == AST_VAR_DECL || decl_child->type == AST_CONST_DECL)) {
                        compileNode(decl_child, chunk, getLine(decl_child));
                    }
                }
                // Pass 2: Compile routines from the declaration block.
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
                        emitDefineGlobal(chunk, var_name_idx, getLine(varNameNode));
                        writeBytecodeChunk(chunk, (uint8_t)node->var_type, getLine(varNameNode)); // The overall type (e.g., TYPE_ARRAY)

                        if (node->var_type == TYPE_ARRAY) {
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
                                        emitConstantIndex16(chunk, addIntConstant(chunk, lower_b.i_val), getLine(varNameNode));
                                    } else {
                                        fprintf(stderr, "L%d: Compiler error: Array bound did not evaluate to a constant integer.\n", getLine(varNameNode));
                                        compiler_had_error = true;
                                    }
                                    freeValue(&lower_b);
                                    
                                    // Use the new helper for the upper bound
                                    if (upper_b.type == TYPE_INTEGER) {
                                        emitConstantIndex16(chunk, addIntConstant(chunk, upper_b.i_val), getLine(varNameNode));
                                    } else {
                                        fprintf(stderr, "L%d: Compiler error: Array bound did not evaluate to a constant integer.\n", getLine(varNameNode));
                                        compiler_had_error = true;
                                    }
                                    freeValue(&upper_b);
                                    
                                } else {
                                    fprintf(stderr, "L%d: Compiler error: Malformed array definition for '%s'.\n", getLine(varNameNode), varNameNode->token->value);
                                    compiler_had_error = true;
                                    emitShort(chunk, 0, getLine(varNameNode));
                                    emitShort(chunk, 0, getLine(varNameNode));
                                }
                            }

                            AST* elem_type = actual_type_def_node->right;
                            writeBytecodeChunk(chunk, (uint8_t)elem_type->var_type, getLine(varNameNode));
                            const char* elem_type_name = (elem_type && elem_type->token) ? elem_type->token->value : "";
                            
                            writeBytecodeChunk(chunk, (uint8_t)addStringConstant(chunk, elem_type_name), getLine(varNameNode));

                        } else {
                            // This now correctly handles ALL non-array types, including simple types,
                            // records, dynamic strings, and fixed-length strings.
                            const char* type_name = "";
                            if (node->var_type == TYPE_POINTER) {
                                AST* ptr_ast = type_specifier_node ? type_specifier_node : actual_type_def_node;
                                if (ptr_ast && ptr_ast->type == AST_POINTER_TYPE && ptr_ast->right && ptr_ast->right->token) {
                                    type_name = ptr_ast->right->token->value; // emit base type name
                                }
                            }
                            if (type_name[0] == '\0' && type_specifier_node && type_specifier_node->token && type_specifier_node->token->value) {
                                type_name = type_specifier_node->token->value;
                            } else if (type_name[0] == '\0' && actual_type_def_node && actual_type_def_node->token && actual_type_def_node->token->value) {
                                /*
                                 * For user-defined enum types the copied type specifier may not carry
                                 * a token with the enum's name.  Falling back to the resolved type
                                 * definition guarantees that the enum's identifier is embedded in the
                                 * bytecode so that OP_DEFINE_GLOBAL can later reconstruct the type.
                                 */
                                type_name = actual_type_def_node->token->value;
                            }
                            emitConstantIndex16(chunk, addStringConstant(chunk, type_name), getLine(varNameNode));

                            if (node->var_type == TYPE_STRING) {
                                int max_len = 0;
                                if (actual_type_def_node && actual_type_def_node->right) {
                                    Value len_val = evaluateCompileTimeValue(actual_type_def_node->right);
                                    if (len_val.type == TYPE_INTEGER) {
                                        max_len = (int)len_val.i_val;
                                    }
                                    freeValue(&len_val);
                                }
                                emitConstantIndex16(chunk, addIntConstant(chunk, max_len), getLine(varNameNode));
                            }
                        }
                        resolveGlobalVariableIndex(chunk, varNameNode->token->value, getLine(varNameNode));

                        // Handle optional initializer for global variables
                        if (node->left) {
                            if (node->var_type == TYPE_ARRAY && node->left->type == AST_ARRAY_LITERAL) {
                                AST* array_type = actual_type_def_node;
                                int dimension_count = array_type->child_count;
                                if (dimension_count == 1) {
                                    AST* sub = array_type->children[0];
                                    Value low_v = evaluateCompileTimeValue(sub->left);
                                    Value high_v = evaluateCompileTimeValue(sub->right);
                                    int low = (low_v.type == TYPE_INTEGER) ? (int)low_v.i_val : 0;
                                    int high = (high_v.type == TYPE_INTEGER) ? (int)high_v.i_val : -1;
                                    freeValue(&low_v); freeValue(&high_v);
                                    int lb[1] = { low };
                                    int ub[1] = { high };
                                    AST* elem_type_node = array_type->right;
                                    VarType elem_type = elem_type_node->var_type;
                                    Value arr_val = makeArrayND(1, lb, ub, elem_type, elem_type_node);
                                    int total = calculateArrayTotalSize(&arr_val);
                                    for (int j = 0; j < total && j < node->left->child_count; j++) {
                                        Value ev = evaluateCompileTimeValue(node->left->children[j]);
                                        freeValue(&arr_val.array_val[j]);
                                        arr_val.array_val[j] = makeCopyOfValue(&ev);
                                        freeValue(&ev);
                                    }
                                    int constIdx = addConstantToChunk(chunk, &arr_val);
                                    freeValue(&arr_val);
                                    emitConstant(chunk, constIdx, getLine(node));
                                } else {
                                    compileRValue(node->left, chunk, getLine(node->left));
                                }
                            } else {
                                compileRValue(node->left, chunk, getLine(node->left));
                            }
                            int name_idx_set = addStringConstant(chunk, varNameNode->token->value);
                            emitGlobalNameIdx(chunk, OP_SET_GLOBAL, OP_SET_GLOBAL16, name_idx_set, getLine(varNameNode));
                        }
                    }
                }
            } else { // Local variables
                AST* type_specifier_node = node->right;

                // Resolve type alias if necessary
                AST* actual_type_def_node = type_specifier_node;
                if (actual_type_def_node && actual_type_def_node->type == AST_TYPE_REFERENCE) {
                    AST* resolved_node = lookupType(actual_type_def_node->token->value);
                    if (resolved_node) {
                        actual_type_def_node = resolved_node;
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

                for (int i = 0; i < node->child_count; i++) {
                    AST* varNameNode = node->children[i];
                    if (!varNameNode || !varNameNode->token) continue;
                    int slot = resolveLocal(current_function_compiler, varNameNode->token->value);
                    if (slot < 0) {
                        fprintf(stderr, "L%d: Compiler error: Local variable '%s' not found in scope.\n", getLine(varNameNode), varNameNode->token->value);
                        compiler_had_error = true;
                        continue;
                    }

                    if (node->var_type == TYPE_ARRAY) {
                        int dimension_count = actual_type_def_node->child_count;
                        if (dimension_count > 255) {
                            fprintf(stderr, "L%d: Compiler error: Maximum array dimensions (255) exceeded.\n", getLine(varNameNode));
                            compiler_had_error = true;
                            break;
                        }

                        writeBytecodeChunk(chunk, OP_INIT_LOCAL_ARRAY, getLine(varNameNode));
                        writeBytecodeChunk(chunk, (uint8_t)slot, getLine(varNameNode));
                        writeBytecodeChunk(chunk, (uint8_t)dimension_count, getLine(varNameNode));

                        for (int dim = 0; dim < dimension_count; dim++) {
                            AST* subrange = actual_type_def_node->children[dim];
                            if (subrange && subrange->type == AST_SUBRANGE) {
                                Value lower_b = evaluateCompileTimeValue(subrange->left);
                                Value upper_b = evaluateCompileTimeValue(subrange->right);

                                if (lower_b.type == TYPE_INTEGER) {
                                    emitConstantIndex16(chunk, addIntConstant(chunk, lower_b.i_val), getLine(varNameNode));
                                } else {
                                    fprintf(stderr, "L%d: Compiler error: Array bound did not evaluate to a constant integer.\n", getLine(varNameNode));
                                    compiler_had_error = true;
                                }
                                freeValue(&lower_b);

                                if (upper_b.type == TYPE_INTEGER) {
                                    emitConstantIndex16(chunk, addIntConstant(chunk, upper_b.i_val), getLine(varNameNode));
                                } else {
                                    fprintf(stderr, "L%d: Compiler error: Array bound did not evaluate to a constant integer.\n", getLine(varNameNode));
                                    compiler_had_error = true;
                                }
                                freeValue(&upper_b);

                            } else {
                                fprintf(stderr, "L%d: Compiler error: Malformed array definition for '%s'.\n", getLine(varNameNode), varNameNode->token->value);
                                compiler_had_error = true;
                                emitShort(chunk, 0, getLine(varNameNode));
                                emitShort(chunk, 0, getLine(varNameNode));
                            }
                        }

                        AST* elem_type = actual_type_def_node->right;
                        writeBytecodeChunk(chunk, (uint8_t)elem_type->var_type, getLine(varNameNode));
                        const char* elem_type_name = (elem_type && elem_type->token) ? elem_type->token->value : "";
                        writeBytecodeChunk(chunk, (uint8_t)addStringConstant(chunk, elem_type_name), getLine(varNameNode));
                    } else if (node->var_type == TYPE_FILE) {
                        writeBytecodeChunk(chunk, OP_INIT_LOCAL_FILE, getLine(varNameNode));
                        writeBytecodeChunk(chunk, (uint8_t)slot, getLine(varNameNode));
                    } else if (node->var_type == TYPE_POINTER) {
                        writeBytecodeChunk(chunk, OP_INIT_LOCAL_POINTER, getLine(varNameNode));
                        writeBytecodeChunk(chunk, (uint8_t)slot, getLine(varNameNode));

                        const char* type_name = "";
                        // Prefer the base type name for pointer types so the VM can resolve it.
                        AST* ptr_ast = type_specifier_node ? type_specifier_node : actual_type_def_node;
                        if (ptr_ast && ptr_ast->type == AST_POINTER_TYPE) {
                            AST* base = ptr_ast->right;
                            if (base && base->token && base->token->value) {
                                type_name = base->token->value;
                            }
                        }
                        if (type_name[0] == '\0') {
                            if (type_specifier_node && type_specifier_node->token && type_specifier_node->token->value) {
                                type_name = type_specifier_node->token->value;
                            } else if (actual_type_def_node && actual_type_def_node->token && actual_type_def_node->token->value) {
                                type_name = actual_type_def_node->token->value;
                            }
                        }
                        emitConstantIndex16(chunk, addStringConstant(chunk, type_name), getLine(varNameNode));
                    }

                    // Handle optional initializer for local variables
                    if (node->left) {
                        if (node->var_type == TYPE_ARRAY && node->left->type == AST_ARRAY_LITERAL) {
                            AST* array_type = actual_type_def_node;
                            int dimension_count = array_type->child_count;
                            if (dimension_count == 1) {
                                AST* sub = array_type->children[0];
                                Value low_v = evaluateCompileTimeValue(sub->left);
                                Value high_v = evaluateCompileTimeValue(sub->right);
                                int low = (low_v.type == TYPE_INTEGER) ? (int)low_v.i_val : 0;
                                int high = (high_v.type == TYPE_INTEGER) ? (int)high_v.i_val : -1;
                                freeValue(&low_v); freeValue(&high_v);
                                int lb[1] = { low };
                                int ub[1] = { high };
                                AST* elem_type_node = array_type->right;
                                VarType elem_type = elem_type_node->var_type;
                                Value arr_val = makeArrayND(1, lb, ub, elem_type, elem_type_node);
                                int total = calculateArrayTotalSize(&arr_val);
                                for (int j = 0; j < total && j < node->left->child_count; j++) {
                                    Value ev = evaluateCompileTimeValue(node->left->children[j]);
                                    freeValue(&arr_val.array_val[j]);
                                    arr_val.array_val[j] = makeCopyOfValue(&ev);
                                    freeValue(&ev);
                                }
                                int constIdx = addConstantToChunk(chunk, &arr_val);
                                freeValue(&arr_val);
                                emitConstant(chunk, constIdx, getLine(node));
                            } else {
                                compileRValue(node->left, chunk, getLine(node->left));
                            }
                        } else {
                            compileRValue(node->left, chunk, getLine(node->left));
                        }
                        writeBytecodeChunk(chunk, OP_SET_LOCAL, getLine(varNameNode));
                        writeBytecodeChunk(chunk, (uint8_t)slot, getLine(varNameNode));
                    }
                }
            }
            break;
        }
        case AST_CONST_DECL: {
            if (current_function_compiler == NULL && node->token) {
                Value const_val = makeVoid();
                AST* type_specifier_node = node->right;
                AST* actual_type_def_node = type_specifier_node;
                if (actual_type_def_node && actual_type_def_node->type == AST_TYPE_REFERENCE) {
                    AST* resolved = lookupType(actual_type_def_node->token->value);
                    if (resolved) actual_type_def_node = resolved;
                }

                if (node->var_type == TYPE_ARRAY && node->left && node->left->type == AST_ARRAY_LITERAL && actual_type_def_node) {
                    if (actual_type_def_node && actual_type_def_node->type == AST_ARRAY_TYPE) {
                        int dimension_count = actual_type_def_node->child_count;
                        if (dimension_count == 1) {
                            AST* sub = actual_type_def_node->children[0];
                            Value low_v = evaluateCompileTimeValue(sub->left);
                            Value high_v = evaluateCompileTimeValue(sub->right);
                            int low = (low_v.type == TYPE_INTEGER) ? (int)low_v.i_val : 0;
                            int high = (high_v.type == TYPE_INTEGER) ? (int)high_v.i_val : -1;
                            freeValue(&low_v);
                            freeValue(&high_v);
                            int lb[1] = { low };
                            int ub[1] = { high };
                            AST* elem_type_node = actual_type_def_node->right;
                            VarType elem_type = elem_type_node->var_type;
                            Value arr_val = makeArrayND(1, lb, ub, elem_type, elem_type_node);
                            int total = calculateArrayTotalSize(&arr_val);
                            for (int j = 0; j < total && j < node->left->child_count; j++) {
                                Value ev = evaluateCompileTimeValue(node->left->children[j]);
                                freeValue(&arr_val.array_val[j]);
                                arr_val.array_val[j] = makeCopyOfValue(&ev);
                                freeValue(&ev);
                            }
                            const_val = arr_val;
                        } else {
                            const_val = evaluateCompileTimeValue(node->left);
                        }
                    } else {
                        const_val = evaluateCompileTimeValue(node->left);
                    }
                } else {
                    const_val = evaluateCompileTimeValue(node->left);
                }

                // Insert into global symbol table so subsequent declarations can reference it.
                insertGlobalSymbol(node->token->value, const_val.type, actual_type_def_node);
                Symbol* sym = lookupGlobalSymbol(node->token->value);
                if (sym && sym->value) {
                    freeValue(sym->value);
                    *(sym->value) = makeCopyOfValue(&const_val);
                    sym->is_const = true;
                }

                insertConstGlobalSymbol(node->token->value, const_val);

                // Constants are resolved at compile time, so no bytecode emission is needed.
                freeValue(&const_val);
            }
            break;
        }
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
    fc.enclosing = current_function_compiler;
    current_function_compiler = &fc;

    // --- FIX: Declare all variables at the top of the function ---
    const char* func_name = func_decl_node->token->value;
    int return_value_slot = -1;
    Symbol* proc_symbol = NULL;
    char name_for_lookup[MAX_SYMBOL_LENGTH * 2 + 2];
    AST* blockNode = NULL;

    fc.name = func_name;

    int func_bytecode_start_address = chunk->count;

    // --- FIX: Look up the symbol *before* trying to use it ---
    if (current_compilation_unit_name) {
        snprintf(name_for_lookup, sizeof(name_for_lookup), "%s.%s", current_compilation_unit_name, func_name);
        toLowerString(name_for_lookup);
    } else {
        strncpy(name_for_lookup, func_name, sizeof(name_for_lookup) - 1);
        name_for_lookup[sizeof(name_for_lookup) - 1] = '\0';
        toLowerString(name_for_lookup);
    }
    
    proc_symbol = lookupProcedure(name_for_lookup);

    if (!proc_symbol) {
        fprintf(stderr, "L%d: Compiler Error: Procedure implementation for '%s' (looked up as '%s') does not have a corresponding interface declaration.\n", line, func_name, name_for_lookup);
        compiler_had_error = true;
        current_function_compiler = NULL;
        return;
    }

    proc_symbol->bytecode_address = func_bytecode_start_address;
    proc_symbol->is_defined = true;
    fc.function_symbol = proc_symbol;
    proc_symbol->enclosing = fc.enclosing ? fc.enclosing->function_symbol : NULL;

    if (current_procedure_table != procedure_table) {
        if (!hashTableLookup(procedure_table, proc_symbol->name)) {
            Symbol* alias = malloc(sizeof(Symbol));
            *alias = *proc_symbol;
            alias->name = strdup(proc_symbol->name);
            alias->is_alias = true;
            alias->real_symbol = proc_symbol;
            alias->next = NULL;
            hashTableInsert(procedure_table, alias);
        }
    }

    // Step 1: Add parameters to the local scope FIRST.
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

    // Step 2: If it's a function, add its name and 'result' as local variables.
    if (func_decl_node->type == AST_FUNCTION_DECL) {
        addLocal(&fc, func_name, line, false);
        return_value_slot = fc.local_count - 1;

        addLocal(&fc, "result", line, false);
    }
    
    // Step 3: Add all other local variables.
    blockNode = (func_decl_node->type == AST_PROCEDURE_DECL) ? func_decl_node->right : func_decl_node->extra;
    if (blockNode && blockNode->type == AST_BLOCK && blockNode->child_count > 0 && blockNode->children[0]->type == AST_COMPOUND) {
        AST* decls = blockNode->children[0];
        for (int i = 0; i < decls->child_count; i++) {
            if (decls->children[i] && decls->children[i]->type == AST_VAR_DECL) {
                AST* var_decl_group = decls->children[i];
                for (int j = 0; j < var_decl_group->child_count; j++) {
                    AST* var_name_node = var_decl_group->children[j];
                    if (var_name_node && var_name_node->token) {
                        addLocal(&fc, var_name_node->token->value, getLine(var_name_node), false);
                    }
                }
            }
        }
    }

    // Step 4: Compile the function body.
    HashTable *saved_table = current_procedure_table;
    if (func_decl_node->symbol_table) {
        current_procedure_table = (HashTable*)func_decl_node->symbol_table;
    }
    if (blockNode) {
        compileNode(blockNode, chunk, getLine(blockNode));
    }
    current_procedure_table = saved_table;

    // Update locals_count in case new locals were declared during body compilation.
    proc_symbol->locals_count = fc.local_count - proc_symbol->arity;

    // Step 5: Emit the return instruction.
    if (func_decl_node->type == AST_FUNCTION_DECL) {
        writeBytecodeChunk(chunk, OP_GET_LOCAL, line);
        writeBytecodeChunk(chunk, (uint8_t)return_value_slot, line);
    }
    writeBytecodeChunk(chunk, OP_RETURN, line);
    
    // Step 6: Cleanup.
    if (proc_symbol) {
        proc_symbol->upvalue_count = fc.upvalue_count;
        for (int i = 0; i < fc.upvalue_count; i++) {
            proc_symbol->upvalues[i].index = fc.upvalues[i].index;
            proc_symbol->upvalues[i].isLocal = fc.upvalues[i].isLocal;
            proc_symbol->upvalues[i].is_ref = fc.upvalues[i].is_ref;
        }
    }

    for(int i = 0; i < fc.local_count; i++) {
        free(fc.locals[i].name);
    }
    current_function_compiler = fc.enclosing;
}

static void compileInlineRoutine(Symbol* proc_symbol, AST* call_node, BytecodeChunk* chunk, int line, bool push_result) {
    if (!proc_symbol || !proc_symbol->type_def) {
        // Fallback to normal call semantics handled by caller
        return;
    }

    AST* decl = proc_symbol->type_def;

    // If we're in the top-level program (no active FunctionCompilerState),
    // create a temporary one so the inliner can allocate locals and emit
    // OP_GET_LOCAL/OP_SET_LOCAL instructions as usual. This mirrors how
    // other compilers conceptually treat the main program body as a routine.
    FunctionCompilerState temp_fc;
    FunctionCompilerState* saved_fc = current_function_compiler;
    if (!current_function_compiler) {
        initFunctionCompiler(&temp_fc);
        current_function_compiler = &temp_fc;
        temp_fc.name = proc_symbol->name ? proc_symbol->name :
                       (decl->token ? decl->token->value : NULL);
        temp_fc.function_symbol = proc_symbol;
    }
    AST* blockNode = (decl->type == AST_PROCEDURE_DECL) ? decl->right : decl->extra;
    if (!blockNode) return;

    int starting_local_count = current_function_compiler->local_count;

    // Map arguments to parameters
    int arg_index = 0;
    for (int i = 0; i < decl->child_count && arg_index < call_node->child_count; i++) {
        AST* param_group = decl->children[i];
        bool by_ref = param_group->by_ref;
        for (int j = 0; j < param_group->child_count && arg_index < call_node->child_count; j++, arg_index++) {
            AST* param_name_node = param_group->children[j];
            const char* pname = param_name_node->token ? param_name_node->token->value : NULL;
            if (!pname) continue;
            addLocal(current_function_compiler, pname, line, by_ref);
            int slot = current_function_compiler->local_count - 1;
            AST* arg_node = call_node->children[arg_index];
            if (by_ref) {
                compileLValue(arg_node, chunk, getLine(arg_node));
            } else {
                compileRValue(arg_node, chunk, getLine(arg_node));
            }
            writeBytecodeChunk(chunk, OP_SET_LOCAL, line);
            writeBytecodeChunk(chunk, (uint8_t)slot, line);
        }
    }

    int result_slot = -1;
    if (decl->type == AST_FUNCTION_DECL) {
        // Allocate a slot for the function's result. Assignments to the
        // function name will target this slot.
        addLocal(current_function_compiler, decl->token->value, line, false);
        result_slot = current_function_compiler->local_count - 1;
    }

    HashTable* saved_table = current_procedure_table;
    if (decl->symbol_table) {
        current_procedure_table = (HashTable*)decl->symbol_table;
    }
    compileNode(blockNode, chunk, getLine(blockNode));
    current_procedure_table = saved_table;

    if (push_result && decl->type == AST_FUNCTION_DECL) {
        if (result_slot != -1) {
            writeBytecodeChunk(chunk, OP_GET_LOCAL, line);
            writeBytecodeChunk(chunk, (uint8_t)result_slot, line);
        } else {
            emitConstant(chunk, addNilConstant(chunk), line);
        }
    }

    // Clean up locals added during inlining
    for (int i = current_function_compiler->local_count - 1; i >= starting_local_count; i--) {
        free(current_function_compiler->locals[i].name);
    }
    current_function_compiler->local_count = starting_local_count;

    // Restore previous compiler state if we created a temporary one
    if (!saved_fc) {
        current_function_compiler = NULL;
    } else {
        current_function_compiler = saved_fc;
    }
}

static void compilePrintf(AST* callNode, BytecodeChunk* chunk, int line) {
    int argIndex = 0;
    int writeArgCount = 0;
    if (callNode->child_count > 0 && callNode->children[0]->type == AST_STRING) {
        AST* fmtNode = callNode->children[0];
        const char* fmt = fmtNode->token && fmtNode->token->value ? fmtNode->token->value : "";
        size_t flen = strlen(fmt);
        char* seg = malloc(flen + 1);
        size_t seglen = 0;
        argIndex = 1;
        for (size_t i = 0; i < flen; ++i) {
            if (fmt[i] == '%' && i + 1 < flen) {
                if (fmt[i + 1] == '%') {
                    seg[seglen++] = '%';
                    i++; // skip second %
                } else {
                    if (seglen > 0) {
                        seg[seglen] = '\0';
                        Value strv = makeString(seg);
                        int cidx = addConstantToChunk(chunk, &strv);
                        freeValue(&strv);
                        emitConstant(chunk, cidx, line);
                        writeArgCount++;
                        seglen = 0;
                    }
                    if (argIndex < callNode->child_count) {
                        compileRValue(callNode->children[argIndex], chunk, getLine(callNode->children[argIndex]));
                        writeArgCount++;
                        argIndex++;
                    }
                    // Skip over flags, width, precision, and length modifiers.
                    size_t j = i + 1;
                    // Flags
                    while (j < flen && strchr("-+ #0'", fmt[j])) j++;
                    // Width
                    while (j < flen && isdigit((unsigned char)fmt[j])) j++;
                    // Precision
                    if (j < flen && fmt[j] == '.') {
                        j++;
                        while (j < flen && isdigit((unsigned char)fmt[j])) j++;
                    }
                    // Length modifiers
                    if (j < flen) {
                        if ((fmt[j] == 'h' || fmt[j] == 'l') && j + 1 < flen && fmt[j + 1] == fmt[j]) {
                            j += 2; // hh or ll
                        } else if (strchr("hlLjzt", fmt[j])) {
                            j++;
                        }
                    }
                    if (j < flen) {
                        i = j; // Skip the conversion specifier
                    } else {
                        i = flen; // bail out if malformed
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
            emitConstant(chunk, cidx, line);
            writeArgCount++;
        }
        free(seg);
    }
    for (; argIndex < callNode->child_count; ++argIndex) {
        compileRValue(callNode->children[argIndex], chunk, getLine(callNode->children[argIndex]));
        writeArgCount++;
    }
    writeBytecodeChunk(chunk, OP_WRITE, line);
    writeBytecodeChunk(chunk, (uint8_t)writeArgCount, line);
}

static void compileStatement(AST* node, BytecodeChunk* chunk, int current_line_approx) {
    if (!node) return;
    int line = getLine(node);
    if (line <= 0) line = current_line_approx;

    switch (node->type) {
        case AST_BREAK: {
            addBreakJump(chunk, line);
            break;
        }
        case AST_THREAD_SPAWN: {
            compileRValue(node, chunk, line);
            writeBytecodeChunk(chunk, OP_POP, line);
            break;
        }
        case AST_THREAD_JOIN: {
            if (node->left) {
                compileRValue(node->left, chunk, getLine(node->left));
            }
            writeBytecodeChunk(chunk, OP_THREAD_JOIN, line);
            break;
        }
        case AST_EXPR_STMT: {
            if (node->left) {
                if (node->left->type == AST_PROCEDURE_CALL && node->left->token &&
                    strcasecmp(node->left->token->value, "printf") == 0) {
                    compilePrintf(node->left, chunk, getLine(node->left));
                } else {
                    compileRValue(node->left, chunk, getLine(node->left));
                    writeBytecodeChunk(chunk, OP_POP, line);
                }
            }
            break;
        }
        case AST_VAR_DECL: {
            if (current_function_compiler != NULL) {
                for (int i = 0; i < node->child_count; i++) {
                    AST *varNameNode = node->children[i];
                    if (varNameNode && varNameNode->token) {
                        addLocal(current_function_compiler,
                                 varNameNode->token->value,
                                 getLine(varNameNode), false);
                    }
                }
            }
            compileNode(node, chunk, line);
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

            int *end_jumps = NULL;
            int end_jumps_count = 0;
            int fallthrough_jump = -1;

            // 2. Iterate through each CASE branch
            for (int i = 0; i < node->child_count; i++) {
                AST* branch = node->children[i];
                if (!branch || branch->type != AST_CASE_BRANCH) continue;

                if (fallthrough_jump != -1) {
                    patchShort(chunk, fallthrough_jump, chunk->count - (fallthrough_jump + 2));
                    fallthrough_jump = -1;
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
                    
                    writeBytecodeChunk(chunk, OP_DUP, line);
                    
                    if (label->type == AST_SUBRANGE) {
                        // Logic for range: (case_val >= lower) AND (case_val <= upper)
                        // This is a more direct and correct translation.
                        
                        // Check lower bound
                        writeBytecodeChunk(chunk, OP_DUP, line);                   // Stack: [case, case]
                        compileRValue(label->left, chunk, getLine(label));      // Stack: [case, case, lower]
                        writeBytecodeChunk(chunk, OP_SWAP, line);                   // Stack: [case, lower, case]
                        writeBytecodeChunk(chunk, OP_GREATER_EQUAL, line);          // Stack: [case, case, bool1]

                        // Check upper bound
                        writeBytecodeChunk(chunk, OP_SWAP, line);                   // Stack: [case, bool1, case]
                        compileRValue(label->right, chunk, getLine(label));     // Stack: [case, bool1, case, upper]
                        writeBytecodeChunk(chunk, OP_SWAP, line);                   // Stack: [case, bool1, upper, case]
                        writeBytecodeChunk(chunk, OP_LESS_EQUAL, line);           // Stack: [case, bool1, bool2]
                        
                        // Combine the two boolean results
                        writeBytecodeChunk(chunk, OP_AND, line);                    // Stack: [case, final_bool]

                    } else {
                        // For single labels
                        compileRValue(label, chunk, getLine(label));
                        writeBytecodeChunk(chunk, OP_EQUAL, line);                  // Stack: [case, bool]
                    }
                    
                    // If the comparison is false, skip the branch body.
                    int false_jump = chunk->count;
                    writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line); emitShort(chunk, 0xFFFF, line);

                    // The branch body starts here when the label matches.
                    writeBytecodeChunk(chunk, OP_POP, line); // Pop the matched case value.
                    compileStatement(branch->right, chunk, getLine(branch->right));

                    // After body, jump to the end of the CASE.
                    end_jumps = realloc(end_jumps, (end_jumps_count + 1) * sizeof(int));
                    end_jumps[end_jumps_count++] = chunk->count;
                    writeBytecodeChunk(chunk, OP_JUMP, line); emitShort(chunk, 0xFFFF, line);

                    // Patch the false jump to point to the next label.
                    patchShort(chunk, false_jump + 1, chunk->count - (false_jump + 3));
                    fallthrough_jump = false_jump + 1;

                    // If a label in a multi-label branch matches, we jump to the body.
                    // The other labels for this branch are now irrelevant.
                    goto next_branch;
                }
                
            next_branch:;
            }

            // After all branches, if an 'else' exists, compile it.
            if (fallthrough_jump != -1) {
                patchShort(chunk, fallthrough_jump, chunk->count - (fallthrough_jump + 2));
            }
            writeBytecodeChunk(chunk, OP_POP, line); // Pop the case value if no branch was taken.
            
            if (node->extra) {
                compileStatement(node->extra, chunk, getLine(node->extra));
            }
            
            // End of the CASE. Patch all jumps from successful branches to here.
            for (int i = 0; i < end_jumps_count; i++) {
                patchShort(chunk, end_jumps[i] + 1, chunk->count - (end_jumps[i] + 3));
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
                emitConstant(chunk, falseConstIdx, line);
            }

            writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line);
            int backward_jump_offset = loopStart - (chunk->count + 2);
            emitShort(chunk, (uint16_t)backward_jump_offset, line);

            patchBreaks(chunk); // <<< MODIFIED
            endLoop(); // <<< MODIFIED
            break;
        }
        case AST_READ: {
            int line = getLine(node);

            int var_start_index = 0;
            // If first argument is a file variable, compile as R-value
            if (node->child_count > 0 && node->children[0]->var_type == TYPE_FILE) {
                compileRValue(node->children[0], chunk, getLine(node->children[0]));
                var_start_index = 1;
            }

            // Remaining arguments are destinations (L-values)
            for (int i = var_start_index; i < node->child_count; i++) {
                AST* arg_node = node->children[i];
                compileLValue(arg_node, chunk, getLine(arg_node));
            }

            // Call built-in 'read'
            int nameIndex = addStringConstant(chunk, "read");
            writeBytecodeChunk(chunk, OP_CALL_BUILTIN, line);
            emitShort(chunk, (uint16_t)nameIndex, line);
            writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
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
            emitShort(chunk, (uint16_t)nameIndex, line);
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

            if (current_function_compiler && current_function_compiler->name && lvalue->type == AST_VARIABLE &&
                lvalue->token && lvalue->token->value &&
                (strcasecmp(lvalue->token->value, current_function_compiler->name) == 0 ||
                 strcasecmp(lvalue->token->value, "result") == 0)) {
                
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
                emitGlobalNameIdx(chunk, OP_SET_GLOBAL, OP_SET_GLOBAL16,
                                   var_name_idx, line);
            }

            // 2. Setup loop context for handling 'break'
            startLoop(-1); // Start address is not needed for FOR loop's break handling

            int loopStart = chunk->count;

            // 3. The loop condition check
            if (var_slot != -1) {
                writeBytecodeChunk(chunk, OP_GET_LOCAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_slot, line);
            } else {
                emitGlobalNameIdx(chunk, OP_GET_GLOBAL, OP_GET_GLOBAL16,
                                   var_name_idx, line);
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
                emitGlobalNameIdx(chunk, OP_GET_GLOBAL, OP_GET_GLOBAL16,
                                   var_name_idx, line);
            }
            int one_const_idx = addIntConstant(chunk, 1);
            emitConstant(chunk, one_const_idx, line);
            writeBytecodeChunk(chunk, is_downto ? OP_SUBTRACT : OP_ADD, line);

            if (var_slot != -1) {
                writeBytecodeChunk(chunk, OP_SET_LOCAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_slot, line);
            } else {
                emitGlobalNameIdx(chunk, OP_SET_GLOBAL, OP_SET_GLOBAL16,
                                   var_name_idx, line);
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
            if (node->token && strcasecmp(node->token->value, "printf") == 0) {
                compilePrintf(node, chunk, line);
                break;
            }
            const char* calleeName = node->token->value;

            // --- NEW, MORE ROBUST LOOKUP LOGIC ---
            Symbol* proc_symbol_lookup = NULL;
            char callee_lower[MAX_SYMBOL_LENGTH];
            strncpy(callee_lower, calleeName, sizeof(callee_lower) - 1);
            callee_lower[sizeof(callee_lower) - 1] = '\0';
            toLowerString(callee_lower);

            // First, try direct (unqualified) lookup
            proc_symbol_lookup = lookupProcedure(callee_lower);

            // If it fails and we are inside a unit, try a qualified lookup
            if (!proc_symbol_lookup && current_compilation_unit_name) {
                char qualified_name_lower[MAX_SYMBOL_LENGTH * 2 + 2];
                snprintf(qualified_name_lower, sizeof(qualified_name_lower), "%s.%s", current_compilation_unit_name, callee_lower);
                toLowerString(qualified_name_lower);
                proc_symbol_lookup = lookupProcedure(qualified_name_lower);
            }
            
            // This is the variable that will hold the symbol we actually work with.
            Symbol* proc_symbol = proc_symbol_lookup;

            // <<<< THIS IS THE CRITICAL FIX: Follow the alias to the real symbol >>>>
            if (proc_symbol && proc_symbol->is_alias) {
                proc_symbol = proc_symbol->real_symbol;
            }

            if (strcasecmp(calleeName, "lock") == 0) {
                if (node->child_count != 1) {
                    fprintf(stderr, "L%d: Compiler Error: lock expects 1 argument.\n", line);
                } else {
                    compileRValue(node->children[0], chunk, getLine(node->children[0]));
                }
                writeBytecodeChunk(chunk, OP_MUTEX_LOCK, line);
                break;
            }
            if (strcasecmp(calleeName, "unlock") == 0) {
                if (node->child_count != 1) {
                    fprintf(stderr, "L%d: Compiler Error: unlock expects 1 argument.\n", line);
                } else {
                    compileRValue(node->children[0], chunk, getLine(node->children[0]));
                }
                writeBytecodeChunk(chunk, OP_MUTEX_UNLOCK, line);
                break;
            }
            if (strcasecmp(calleeName, "destroy") == 0) {
                if (node->child_count != 1) {
                    fprintf(stderr, "L%d: Compiler Error: destroy expects 1 argument.\n", line);

                } else {
                    compileRValue(node->children[0], chunk, getLine(node->children[0]));
                }
                writeBytecodeChunk(chunk, OP_MUTEX_DESTROY, line);
                break;
            }
            if (strcasecmp(calleeName, "mutex") == 0) {
                if (node->child_count != 0) {
                    fprintf(stderr, "L%d: Compiler Error: mutex expects no arguments.\n", line);
                }
                writeBytecodeChunk(chunk, OP_MUTEX_CREATE, line);
                break;
            }
            if (strcasecmp(calleeName, "rcmutex") == 0) {
                if (node->child_count != 0) {
                    fprintf(stderr, "L%d: Compiler Error: rcmutex expects no arguments.\n", line);
                }
                writeBytecodeChunk(chunk, OP_RCMUTEX_CREATE, line);
                break;
            }

            bool is_read_proc = (strcasecmp(calleeName, "read") == 0 || strcasecmp(calleeName, "readln") == 0);
            bool callee_is_builtin = isBuiltin(calleeName) && !proc_symbol;

            bool param_mismatch = false;
            if (proc_symbol && proc_symbol->type_def) {
                int expected = proc_symbol->type_def->child_count;
                bool is_inc_dec = (strcasecmp(calleeName, "inc") == 0 || strcasecmp(calleeName, "dec") == 0);
                bool is_halt = (strcasecmp(calleeName, "halt") == 0);
                if (is_inc_dec) {
                    if (!(node->child_count == 1 || node->child_count == 2)) {
                        fprintf(stderr, "L%d: Compiler Error: '%s' expects 1 or 2 argument(s) but %d were provided.\n",
                                line, calleeName, node->child_count);
                        compiler_had_error = true;
                        param_mismatch = true;
                    }
                } else if (is_halt) {
                    if (!(node->child_count == 0 || node->child_count == 1)) {
                        fprintf(stderr, "L%d: Compiler Error: '%s' expects 0 or 1 argument(s) but %d were provided.\n",
                                line, calleeName, node->child_count);
                        compiler_had_error = true;
                        param_mismatch = true;
                    }
                } else if (node->child_count != expected) {
                    fprintf(stderr, "L%d: Compiler Error: '%s' expects %d argument(s) but %d were provided.\n",
                            line, calleeName, expected, node->child_count);
                    compiler_had_error = true;
                    param_mismatch = true;
                }

                if (!param_mismatch) {
                    for (int i = 0; i < node->child_count; i++) {
                        AST* param_node = proc_symbol->type_def->children[i];
                        AST* arg_node = node->children[i];
                        if (!param_node || !arg_node) continue;

                        // VAR parameters preserve their full TYPE_ARRAY node so that
                        // structural comparisons (like array bounds) remain possible.
                        AST* param_type = param_node->type_def ? param_node->type_def : param_node;
                        bool match = typesMatch(param_type, arg_node, callee_is_builtin);
                        if (!match) {
                            AST* param_actual = resolveTypeAlias(param_type);
                            AST* arg_actual   = resolveTypeAlias(arg_node->type_def);
                            if (param_actual && arg_actual) {
                                if (param_actual->var_type == TYPE_ARRAY && arg_actual->var_type != TYPE_ARRAY) {
                                    fprintf(stderr,
                                            "L%d: Compiler Error: argument %d to '%s' expects an array but got %s.\n",
                                            line, i + 1, calleeName,
                                            varTypeToString(arg_actual->var_type));
                                } else if (param_actual->var_type != TYPE_ARRAY && arg_actual->var_type == TYPE_ARRAY) {
                                    fprintf(stderr,
                                            "L%d: Compiler Error: argument %d to '%s' expects %s but got an array.\n",
                                            line, i + 1, calleeName,
                                            varTypeToString(param_actual->var_type));
                                } else if (param_actual->var_type == TYPE_ARRAY && arg_actual->var_type == TYPE_ARRAY) {
                                    AST* param_elem = resolveTypeAlias(param_actual->right);
                                    AST* arg_elem   = resolveTypeAlias(arg_actual->right);
                                    const char* exp_str = param_elem ? varTypeToString(param_elem->var_type) : "UNKNOWN";
                                    const char* got_str = arg_elem ? varTypeToString(arg_elem->var_type) : "UNKNOWN";
                                    fprintf(stderr,
                                            "L%d: Compiler Error: argument %d to '%s' expects type ARRAY OF %s but got ARRAY OF %s.\n",
                                            line, i + 1, calleeName,
                                            exp_str,
                                            got_str);
                                } else {
                                    fprintf(stderr,
                                            "L%d: Compiler Error: argument %d to '%s' expects type %s but got %s.\n",
                                            line, i + 1, calleeName,
                                            varTypeToString(param_actual->var_type),
                                            varTypeToString(arg_actual->var_type));
                                }
                            } else {
                                VarType expected_vt = param_actual ? param_actual->var_type : param_type->var_type;
                                VarType actual_vt   = arg_actual ? arg_actual->var_type : arg_node->var_type;
                                fprintf(stderr,
                                        "L%d: Compiler Error: argument %d to '%s' expects type %s but got %s.\n",
                                        line, i + 1, calleeName,
                                        varTypeToString(expected_vt),
                                        varTypeToString(actual_vt));
                            }
                            compiler_had_error = true;
                            param_mismatch = true;
                            break;
                        }
                        if (param_node->by_ref) {
                            bool is_lvalue = (arg_node->type == AST_VARIABLE ||
                                              arg_node->type == AST_FIELD_ACCESS ||
                                              arg_node->type == AST_ARRAY_ACCESS ||
                                              arg_node->type == AST_DEREFERENCE);
                            if (!is_lvalue) {
                                fprintf(stderr,
                                        "L%d: Compiler Error: argument %d to '%s' must be a variable (VAR parameter).\n",
                                        line, i + 1, calleeName);
                                compiler_had_error = true;
                                param_mismatch = true;
                                break;
                            }
                        }
                    }
                }
            }

            if (param_mismatch) {
                break;
            }

            // Inline routine bodies directly when possible.
            if (proc_symbol && proc_symbol->type_def && proc_symbol->type_def->is_inline) {
                compileInlineRoutine(proc_symbol, node, chunk, line, false);
                break;
            }

            // (Argument compilation logic remains the same...)
            for (int i = 0; i < node->child_count; i++) {
                AST* arg_node = node->children[i];
                bool is_var_param = false;
                if (is_read_proc && (i > 0 || (i == 0 && arg_node->var_type != TYPE_FILE))) {
                    is_var_param = true;
                }
                else if (calleeName && (
                    (i == 0 && (strcasecmp(calleeName, "new") == 0 || strcasecmp(calleeName, "dispose") == 0 || strcasecmp(calleeName, "assign") == 0 || strcasecmp(calleeName, "reset") == 0 || strcasecmp(calleeName, "rewrite") == 0 || strcasecmp(calleeName, "append") == 0 || strcasecmp(calleeName, "close") == 0 || strcasecmp(calleeName, "rename") == 0 || strcasecmp(calleeName, "erase") == 0 || strcasecmp(calleeName, "inc") == 0 || strcasecmp(calleeName, "dec") == 0 || strcasecmp(calleeName, "setlength") == 0 || strcasecmp(calleeName, "mstreamloadfromfile") == 0 || strcasecmp(calleeName, "mstreamsavetofile") == 0 || strcasecmp(calleeName, "mstreamfree") == 0 || strcasecmp(calleeName, "eof") == 0 || strcasecmp(calleeName, "readkey") == 0)) ||
                    (strcasecmp(calleeName, "readln") == 0 && (i > 0 || (i == 0 && arg_node->var_type != TYPE_FILE))) ||
                    (strcasecmp(calleeName, "getmousestate") == 0) || // All params are VAR
                    (strcasecmp(calleeName, "gettextsize") == 0 && i > 0) || // Width and Height are VAR
                    (strcasecmp(calleeName, "str") == 0 && i == 1) ||
                    /* Date/time routines return values via VAR parameters */
                    (strcasecmp(calleeName, "dosgetdate") == 0) ||
                    (strcasecmp(calleeName, "dosgettime") == 0) ||
                    (strcasecmp(calleeName, "getdate") == 0) ||
                    (strcasecmp(calleeName, "gettime") == 0)
                )) {
                    is_var_param = true;
                }
                else if (proc_symbol && proc_symbol->type_def && i < proc_symbol->type_def->child_count) {
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


            if (isBuiltin(calleeName) && !proc_symbol) {
                if (strcasecmp(calleeName, "exit") == 0) {
                    if (node->child_count > 0) {
                        fprintf(stderr, "L%d: exit does not take arguments.\n", line);
                        compiler_had_error = true;
                    }

                    int slot = -1;
                    if (current_function_compiler) {
                        slot = resolveLocal(current_function_compiler, current_function_compiler->name);
                    }
                    if (slot != -1) {
                        writeBytecodeChunk(chunk, OP_GET_LOCAL, line);
                        writeBytecodeChunk(chunk, (uint8_t)slot, line);
                    }
                    writeBytecodeChunk(chunk, OP_EXIT, line);
                } else {
                    BuiltinRoutineType type = getBuiltinType(calleeName);
                    if (type == BUILTIN_TYPE_PROCEDURE || type == BUILTIN_TYPE_FUNCTION) {
                        char normalized_name[MAX_SYMBOL_LENGTH];
                        strncpy(normalized_name, calleeName, sizeof(normalized_name) - 1);
                        normalized_name[sizeof(normalized_name) - 1] = '\0';
                        toLowerString(normalized_name);
                        int nameIndex = addStringConstant(chunk, normalized_name);
                        writeBytecodeChunk(chunk, OP_CALL_BUILTIN, line);
                        emitShort(chunk, (uint16_t)nameIndex, line);
                        writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
                        if (type == BUILTIN_TYPE_FUNCTION) {
                            writeBytecodeChunk(chunk, OP_POP, line);
                        }
                    } else {
                        // This case handles if a name is in the isBuiltin list but not in getBuiltinType,
                        // which would be an internal inconsistency.
                        fprintf(stderr, "L%d: Compiler Error: '%s' is not a recognized built-in procedure or function.\n", line, calleeName);
                        compiler_had_error = true;
                    }
                }
            } else if (proc_symbol) { // If a symbol was found (either defined or forward-declared)
                int nameIndex = addStringConstant(chunk, calleeName);
                writeBytecodeChunk(chunk, OP_CALL, line);
                emitShort(chunk, (uint16_t)nameIndex, line);

                if (proc_symbol->is_defined) {
                    emitShort(chunk, (uint16_t)proc_symbol->bytecode_address, line);
                } else {
                    emitShort(chunk, 0xFFFF, line);
                }
                writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);

                // This logic for user-defined functions is already correct.
                if (proc_symbol->type != TYPE_VOID) {
                    writeBytecodeChunk(chunk, OP_POP, line);
                }
            } else {
                // Fallback: map known host-threading helpers by name
                if (strcasecmp(calleeName, "createthread") == 0) {
                    // New signature: CreateThread(procAddr, argPtr)
                    // Backward compatible: if only 1 arg given, push NIL for argPtr
                    if (node->child_count < 1 || node->child_count > 2) {
                        fprintf(stderr, "L%d: Compiler Error: CreateThread expects 1 or 2 arguments.\n", line);
                    }
                    if (node->child_count >= 1) {
                        compileRValue(node->children[0], chunk, getLine(node->children[0])); // proc addr
                    } else {
                        emitConstant(chunk, addIntConstant(chunk, 0), line); // push 0 as fallback addr
                    }
                    if (node->child_count >= 2) {
                        compileRValue(node->children[1], chunk, getLine(node->children[1])); // arg ptr/value
                    } else {
                        emitConstant(chunk, addNilConstant(chunk), line); // default arg = nil
                    }
                    writeBytecodeChunk(chunk, OP_CALL_HOST, line);
                    writeBytecodeChunk(chunk, (uint8_t)HOST_FN_CREATE_THREAD_ADDR, line);
                } else if (strcasecmp(calleeName, "waitforthread") == 0) {
                    if (node->child_count != 1) {
                        fprintf(stderr, "L%d: Compiler Error: WaitForThread expects 1 argument (thread id).\n", line);
                    } else {
                        compileRValue(node->children[0], chunk, getLine(node->children[0]));
                    }
                    writeBytecodeChunk(chunk, OP_CALL_HOST, line);
                    writeBytecodeChunk(chunk, (uint8_t)HOST_FN_WAIT_THREAD, line);
                } else {
                    // Indirect call through a procedure pointer variable: arguments are already on stack.
                    // Push callee address value and emit indirect call-for-statement.
                    AST tmpVar;
                    memset(&tmpVar, 0, sizeof(AST));
                    tmpVar.type = AST_VARIABLE;
                    tmpVar.token = node->token; // reference only; compileRValue copies token if needed
                    compileRValue(&tmpVar, chunk, line);
                    writeBytecodeChunk(chunk, OP_PROC_CALL_INDIRECT, line);
                    writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
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
            Value set_const_val;
            memset(&set_const_val, 0, sizeof(Value));
            set_const_val.type = TYPE_SET;
            set_const_val.max_length = 0;
            set_const_val.set_val.set_size = 0;
            set_const_val.set_val.set_values = NULL;

            for (int i = 0; i < node->child_count; i++) {
                AST* member = node->children[i];
                if (member->type == AST_SUBRANGE) {
                    Value start_val = evaluateCompileTimeValue(member->left);
                    Value end_val = evaluateCompileTimeValue(member->right);
                    
                    bool start_ok = (start_val.type == TYPE_INTEGER || start_val.type == TYPE_CHAR || start_val.type == TYPE_ENUM);
                    bool end_ok = (end_val.type == TYPE_INTEGER || end_val.type == TYPE_CHAR || end_val.type == TYPE_ENUM);

                    if (start_ok && end_ok) {
                        long long start_ord = (start_val.type == TYPE_ENUM) ? start_val.enum_val.ordinal : ((start_val.type == TYPE_INTEGER) ? start_val.i_val : start_val.c_val);
                        long long end_ord = (end_val.type == TYPE_ENUM) ? end_val.enum_val.ordinal : ((end_val.type == TYPE_INTEGER) ? end_val.i_val : end_val.c_val);
                        
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
                    if (elem_val.type == TYPE_INTEGER || elem_val.type == TYPE_CHAR || elem_val.type == TYPE_ENUM) {
                        long long ord = (elem_val.type == TYPE_ENUM) ? elem_val.enum_val.ordinal : ((elem_val.type == TYPE_INTEGER) ? elem_val.i_val : elem_val.c_val);
                        addOrdinalToSetValue(&set_const_val, ord);
                    } else {
                        fprintf(stderr, "L%d: Compiler error: Set elements must be constant ordinal types.\n", getLine(member));
                        compiler_had_error = true;
                    }
                    freeValue(&elem_val);
                }
            }

            int constIndex = addConstantToChunk(chunk, &set_const_val);
            freeValue(&set_const_val);

            emitConstant(chunk, constIndex, line);
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
            emitConstant(chunk, constIndex, line);
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

            // If the string literal has a length of 1, treat it as a character constant
            if (strlen(node_token->value) == 1) {
                /* Single-character string literals represent CHAR constants.
                 * Cast through unsigned char so values in the 128..255 range
                 * are preserved correctly. */
                Value val = makeChar((unsigned char)node_token->value[0]);
                int constIndex = addConstantToChunk(chunk, &val);
                emitConstant(chunk, constIndex, line);
                // The temporary char value `val` does not need `freeValue`
            } else {
                // For strings longer than 1 character, use the existing logic
                int constIndex = addStringConstant(chunk, node_token->value);
                emitConstant(chunk, constIndex, line);
            }
            break;
        }
        case AST_NIL: {
            int constIndex = addNilConstant(chunk);
            emitConstant(chunk, constIndex, line);
            break;
        }
        case AST_ADDR_OF: {
            // Address-of routines: push procedure bytecode address as integer constant
            int addr = 0;
            if (node->left && node->left->token && node->left->token->value) {
                const char* pname = node->left->token->value;
                Symbol* psym = lookupProcedure(pname);
                if (psym) addr = psym->bytecode_address;
            }
            int constIndex = addIntConstant(chunk, addr);
            emitConstant(chunk, constIndex, line);
            break;
        }
        case AST_THREAD_SPAWN: {
            AST *call = node->left;
            if (!call || call->type != AST_PROCEDURE_CALL) {
                fprintf(stderr, "L%d: Compiler error: spawn expects procedure call.\n", line);
                compiler_had_error = true;
                break;
            }
            const char *calleeName = call->token->value;
            Symbol *proc_symbol = lookupProcedure(calleeName);
            if (!proc_symbol || !proc_symbol->is_defined) {
                fprintf(stderr, "L%d: Compiler error: Undefined procedure '%s' in spawn.\n", line, calleeName);
                compiler_had_error = true;
                break;
            }
            if (call->child_count > 0) {
                fprintf(stderr, "L%d: Compiler warning: Arguments to '%s' ignored in spawn.\n", line, calleeName);
            }
            writeBytecodeChunk(chunk, OP_THREAD_CREATE, line);
            emitShort(chunk, (uint16_t)proc_symbol->bytecode_address, line);
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
                if (is_ref && node->var_type != TYPE_ARRAY) {
                    writeBytecodeChunk(chunk, OP_GET_INDIRECT, line);
                }
            } else {
                int upvalue_slot = -1;
                if (current_function_compiler) {
                    upvalue_slot = resolveUpvalue(current_function_compiler, varName);
                }
                if (upvalue_slot != -1) {
                    bool up_is_ref = current_function_compiler->upvalues[upvalue_slot].is_ref;
                    writeBytecodeChunk(chunk, OP_GET_UPVALUE, line);
                    writeBytecodeChunk(chunk, (uint8_t)upvalue_slot, line);
                    if (up_is_ref && node->var_type != TYPE_ARRAY) {
                        writeBytecodeChunk(chunk, OP_GET_INDIRECT, line);
                    }
                } else {
                    // Check if it's a compile-time constant first.
                    Value* const_val_ptr = findCompilerConstant(varName);
                    if (const_val_ptr) {
                        emitConstant(chunk, addConstantToChunk(chunk, const_val_ptr), line);
                    } else {
                        int nameIndex = addStringConstant(chunk, varName);
                        emitGlobalNameIdx(chunk, OP_GET_GLOBAL, OP_GET_GLOBAL16,
                                           nameIndex, line);
                    }
                }
            }
            break;
        }
        case AST_FIELD_ACCESS: {
            // Get the address of the field, then get the value at that address.
            compileLValue(node, chunk, getLine(node));
            writeBytecodeChunk(chunk, OP_GET_INDIRECT, line);
            break;
        }
        case AST_ARRAY_ACCESS: {
            // This logic correctly distinguishes between accessing a string/char vs. a regular array.
            if (node->left && (node->left->var_type == TYPE_STRING || node->left->var_type == TYPE_CHAR)) {
                compileRValue(node->left, chunk, getLine(node->left));      // Push the string or char
                compileRValue(node->children[0], chunk, getLine(node->children[0])); // Push the index
                writeBytecodeChunk(chunk, OP_GET_CHAR_FROM_STRING, line); // Use the specialized opcode
                break;
            }

            // Default behavior for actual arrays: get address, then get value.
            compileLValue(node, chunk, getLine(node));
            writeBytecodeChunk(chunk, OP_GET_INDIRECT, line);
            break;
        }
        case AST_ASSIGN: {
            AST* lvalue = node->left;
            AST* rvalue = node->right;

            compileRValue(rvalue, chunk, getLine(rvalue));
            writeBytecodeChunk(chunk, OP_DUP, line); // Preserve assigned value as the expression result

            if (current_function_compiler && current_function_compiler->name && lvalue->type == AST_VARIABLE &&
                lvalue->token && lvalue->token->value &&
                (strcasecmp(lvalue->token->value, current_function_compiler->name) == 0 ||
                 strcasecmp(lvalue->token->value, "result") == 0)) {

                int return_slot = resolveLocal(current_function_compiler, current_function_compiler->name);
                if (return_slot != -1) {
                    writeBytecodeChunk(chunk, OP_SET_LOCAL, line);
                    writeBytecodeChunk(chunk, (uint8_t)return_slot, line);
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
        case AST_BINARY_OP: {
            if (node_token && node_token->type == TOKEN_AND) {
                // Check annotated type to decide between bitwise and logical AND
                if (node->left && node->left->var_type == TYPE_INTEGER) {
                    // Bitwise AND for integers
                    compileRValue(node->left, chunk, getLine(node->left));
                    compileRValue(node->right, chunk, getLine(node->right));
                    writeBytecodeChunk(chunk, OP_AND, line);
                } else {
                    // Logical AND for booleans (with short-circuiting)
                    compileRValue(node->left, chunk, getLine(node->left)); // stack: [A]
                    int jump_if_false = chunk->count;
                    writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line);     // Pops A. Jumps if A is false.
                    emitShort(chunk, 0xFFFF, line);

                    // If A was true, result is B.
                    compileRValue(node->right, chunk, getLine(node->right)); // stack: [B]
                    int jump_over_false_case = chunk->count;
                    writeBytecodeChunk(chunk, OP_JUMP, line);
                    emitShort(chunk, 0xFFFF, line);
                    // If A was false, jump here and push 'false' as the result.
                    patchShort(chunk, jump_if_false + 1, chunk->count - (jump_if_false + 3));
                    int false_const_idx = addBooleanConstant(chunk, false);
                    emitConstant(chunk, false_const_idx, line); // stack: [false]

                    // End of the expression for both paths.
                    patchShort(chunk, jump_over_false_case + 1, chunk->count - (jump_over_false_case + 3));
                }
            } else if (node_token && node_token->type == TOKEN_OR) {
                // Check annotated type for bitwise vs. logical OR
                if (node->left && node->left->var_type == TYPE_INTEGER) {
                    // Bitwise OR for integers
                    compileRValue(node->left, chunk, getLine(node->left));
                    compileRValue(node->right, chunk, getLine(node->right));
                    writeBytecodeChunk(chunk, OP_OR, line);
                } else {
                    // Logical OR for booleans (with short-circuiting)
                compileRValue(node->left, chunk, getLine(node->left)); // stack: [A]
                int jump_if_false = chunk->count;
                writeBytecodeChunk(chunk, OP_JUMP_IF_FALSE, line);     // Pops A. Jumps if A is false.
                emitShort(chunk, 0xFFFF, line);

                // If we get here, A was true. Stack is empty. The result must be 'true'.
                int true_const_idx = addBooleanConstant(chunk, true);
                emitConstant(chunk, true_const_idx, line);
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
            emitConstant(chunk, addBooleanConstant(chunk, (node->i_val != 0)), line);
            break;
        }
        case AST_PROCEDURE_CALL: {
            int line = getLine(node);
            if (line <= 0) line = current_line_approx;
            if (node->token && strcasecmp(node->token->value, "printf") == 0) {
                compilePrintf(node, chunk, line);
                emitConstant(chunk, addIntConstant(chunk, 0), line);
                break;
            }
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
                emitConstant(chunk, addNilConstant(chunk), line);
                break;
            }

            if (strcasecmp(functionName, "createthread") == 0) {
                // New signature: CreateThread(procAddr, argPtr). If only 1 arg, default arg=nil.
                if (node->child_count < 1 || node->child_count > 2) {
                    fprintf(stderr, "L%d: Compiler Error: CreateThread expects 1 or 2 arguments.\n", line);
                }
                if (node->child_count >= 1) {
                    compileRValue(node->children[0], chunk, getLine(node->children[0])); // proc addr
                } else {
                    emitConstant(chunk, addIntConstant(chunk, 0), line);
                }
                if (node->child_count >= 2) {
                    compileRValue(node->children[1], chunk, getLine(node->children[1])); // arg ptr
                } else {
                    emitConstant(chunk, addNilConstant(chunk), line); // default arg
                }
                writeBytecodeChunk(chunk, OP_CALL_HOST, line);
                writeBytecodeChunk(chunk, (uint8_t)HOST_FN_CREATE_THREAD_ADDR, line);
                break;
            }
            if (strcasecmp(functionName, "waitforthread") == 0) {
                // Expects 1 arg: integer thread id
                if (node->child_count != 1) {
                    fprintf(stderr, "L%d: Compiler Error: WaitForThread expects 1 argument (thread id).\n", line);
                } else {
                    compileRValue(node->children[0], chunk, getLine(node->children[0]));
                }
                writeBytecodeChunk(chunk, OP_CALL_HOST, line);
                writeBytecodeChunk(chunk, (uint8_t)HOST_FN_WAIT_THREAD, line);
                break;
            }
            if (strcasecmp(functionName, "mutex") == 0) {
                if (node->child_count != 0) {
                    fprintf(stderr, "L%d: Compiler Error: mutex expects no arguments.\n", line);
                }
                writeBytecodeChunk(chunk, OP_MUTEX_CREATE, line);
                break;
            }
            if (strcasecmp(functionName, "rcmutex") == 0) {
                if (node->child_count != 0) {
                    fprintf(stderr, "L%d: Compiler Error: rcmutex expects no arguments.\n", line);
                }
                writeBytecodeChunk(chunk, OP_RCMUTEX_CREATE, line);
                break;
            }
            // (indirect function pointer calls are handled later in the fallback path)
            if (strcasecmp(functionName, "lock") == 0) {
                if (node->child_count != 1) {
                    fprintf(stderr, "L%d: Compiler Error: lock expects 1 argument.\n", line);
                } else {
                    compileRValue(node->children[0], chunk, getLine(node->children[0]));
                }
                writeBytecodeChunk(chunk, OP_MUTEX_LOCK, line);
                break;
            }
            if (strcasecmp(functionName, "unlock") == 0) {
                if (node->child_count != 1) {
                    fprintf(stderr, "L%d: Compiler Error: unlock expects 1 argument.\n", line);
                } else {
                    compileRValue(node->children[0], chunk, getLine(node->children[0]));
                }
                writeBytecodeChunk(chunk, OP_MUTEX_UNLOCK, line);
                break;
            }

            if (strcasecmp(functionName, "destroy") == 0) {
                if (node->child_count != 1) {
                    fprintf(stderr, "L%d: Compiler Error: destroy expects 1 argument.\n", line);
                } else {
                    compileRValue(node->children[0], chunk, getLine(node->children[0]));
                }
                writeBytecodeChunk(chunk, OP_MUTEX_DESTROY, line);
                break;
            }

            // --- NEW, MORE ROBUST LOOKUP LOGIC ---
            Symbol* func_symbol_lookup = NULL;
            char func_name_lower[MAX_SYMBOL_LENGTH];
            strncpy(func_name_lower, functionName, sizeof(func_name_lower) - 1);
            func_name_lower[sizeof(func_name_lower) - 1] = '\0';
            toLowerString(func_name_lower);

            func_symbol_lookup = lookupProcedure(func_name_lower);
            
            if (!func_symbol_lookup && current_compilation_unit_name) {
                char qualified_name_lower[MAX_SYMBOL_LENGTH * 2 + 2];
                snprintf(qualified_name_lower, sizeof(qualified_name_lower), "%s.%s", current_compilation_unit_name, func_name_lower);
                toLowerString(qualified_name_lower);
                func_symbol_lookup = lookupProcedure(qualified_name_lower);
            }
            
            Symbol* func_symbol = func_symbol_lookup;

            // <<<< THIS IS THE CRITICAL FIX: Follow the alias to the real symbol >>>>
            if (func_symbol && func_symbol->is_alias) {
                func_symbol = func_symbol->real_symbol;
            }

            // Inline function calls directly when marked inline.
            if (func_symbol && func_symbol->type_def && func_symbol->type_def->is_inline) {
                compileInlineRoutine(func_symbol, node, chunk, line, true);
                break;
            }

            if (!func_symbol && isBuiltin(functionName) && (strcasecmp(functionName, "low") == 0 || strcasecmp(functionName, "high") == 0)) {
                if (node->child_count == 1 && node->children[0]->type == AST_VARIABLE) {
                    AST* type_arg_node = node->children[0];
                    int typeNameIndex = addStringConstant(chunk, type_arg_node->token->value);
                    emitConstant(chunk, typeNameIndex, line);
                } else {
                    fprintf(stderr, "L%d: Compiler error: Argument to '%s' must be a single type identifier.\n", line, functionName);
                    compiler_had_error = true;
                }
            } else {
                for (int i = 0; i < node->child_count; i++) {
                    AST* arg_node = node->children[i];
                    if (!arg_node) continue;
                    
                    bool is_var_param = false;
                    if (func_symbol && func_symbol->type_def && i < func_symbol->type_def->child_count) {
                        AST* param_node = func_symbol->type_def->children[i];
                        if (param_node && param_node->by_ref) {
                            is_var_param = true;
                        }
                    } else if (functionName && i == 0 && strcasecmp(functionName, "eof") == 0) {
                        // Built-in EOF takes its file parameter by reference
                        is_var_param = true;
                    }

                    if (is_var_param) {
                        compileLValue(arg_node, chunk, getLine(arg_node));
                    } else {
                        compileRValue(arg_node, chunk, getLine(arg_node));
                    }
                }
            }

            if (!func_symbol && isBuiltin(functionName)) {
                BuiltinRoutineType type = getBuiltinType(functionName);
                if (type == BUILTIN_TYPE_PROCEDURE) {
                    fprintf(stderr, "L%d: Compiler Error: Built-in procedure '%s' cannot be used as a function in an expression.\n", line, functionName);
                    compiler_had_error = true;
                    for(uint8_t i = 0; i < node->child_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                    emitConstant(chunk, addNilConstant(chunk), line);
                } else if (type == BUILTIN_TYPE_FUNCTION) {
                    char normalized_name[MAX_SYMBOL_LENGTH];
                    strncpy(normalized_name, functionName, sizeof(normalized_name) - 1);
                    normalized_name[sizeof(normalized_name) - 1] = '\0';
                    toLowerString(normalized_name);
                    int nameIndex = addStringConstant(chunk, normalized_name);
                    writeBytecodeChunk(chunk, OP_CALL_BUILTIN, line);
                    emitShort(chunk, (uint16_t)nameIndex, line);
                    writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
                } else {
                    // Fallback to indirect function pointer call: use variable's value as address.
                    AST tmpVar; memset(&tmpVar, 0, sizeof(AST));
                    tmpVar.type = AST_VARIABLE; tmpVar.token = node->token;
                    // Arguments are already compiled (above) and on the stack; now push callee address
                    compileRValue(&tmpVar, chunk, line);
                    writeBytecodeChunk(chunk, OP_CALL_INDIRECT, line);
                    writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
                }
            } else {
                char original_display_name[MAX_SYMBOL_LENGTH * 2 + 2];
                if (isCallQualified) {
                    snprintf(original_display_name, sizeof(original_display_name), "%s.%s", node->left->token->value, functionName);
                } else {
                    strncpy(original_display_name, functionName, sizeof(original_display_name)-1); original_display_name[sizeof(original_display_name)-1] = '\0';
                }
                
                if (func_symbol) {
                    if (func_symbol->type == TYPE_VOID) {
                        fprintf(stderr, "L%d: Compiler Error: Procedure '%s' cannot be used as a function.\n", line, original_display_name);
                        compiler_had_error = true;
                        for(uint8_t i=0; i < node->child_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                        emitConstant(chunk, addNilConstant(chunk), line);
                    } else if ((strcasecmp(functionName, "inc") == 0 || strcasecmp(functionName, "dec") == 0)
                               ? !(node->child_count == 1 || node->child_count == 2)
                               : (func_symbol->arity != node->child_count)) {
                        if (strcasecmp(functionName, "inc") == 0 || strcasecmp(functionName, "dec") == 0) {
                            fprintf(stderr, "L%d: Compiler Error: '%s' expects 1 or 2 argument(s) but %d were provided.\n",
                                    line, original_display_name, node->child_count);
                        } else {
                            fprintf(stderr, "L%d: Compiler Error: Function '%s' expects %d arguments, got %d.\n",
                                    line, original_display_name, func_symbol->arity, node->child_count);
                        }
                        compiler_had_error = true;
                        for(uint8_t i=0; i < node->child_count; ++i) writeBytecodeChunk(chunk, OP_POP, line);
                        emitConstant(chunk, addNilConstant(chunk), line);
                    } else {
                        int nameIndex = addStringConstant(chunk, functionName);
                        writeBytecodeChunk(chunk, OP_CALL, line);
                        emitShort(chunk, (uint16_t)nameIndex, line);

                        if (func_symbol->is_defined) {
                            emitShort(chunk, (uint16_t)func_symbol->bytecode_address, line);
                        } else {
                            emitShort(chunk, 0xFFFF, line);
                        }
                        writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
                    }
                } else {
                    // Fallback to indirect function pointer call: push callee address and perform indirect call
                    AST tmpVar; memset(&tmpVar, 0, sizeof(AST));
                    tmpVar.type = AST_VARIABLE; tmpVar.token = node->token;
                    compileRValue(&tmpVar, chunk, line);
                    writeBytecodeChunk(chunk, OP_CALL_INDIRECT, line);
                    writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
                }
            }
            break;
        }
        default:
            fprintf(stderr, "L%d: Compiler warning: Unhandled AST node type %s in compileRValue.\n", line, astTypeToString(node->type));
            emitConstant(chunk, addIntConstant(chunk, 0), line); // Push dummy 0
            break;
    }
}

void compileUnitImplementation(AST* unit_ast, BytecodeChunk* outputChunk) {
    if (!unit_ast || unit_ast->type != AST_UNIT) {
        return;
    }
    AST* impl_block = unit_ast->extra;
    if (!impl_block || impl_block->type != AST_COMPOUND) {
        return;
    }


    // Set the compilation context for qualified lookups
    current_compilation_unit_name = unit_ast->token ? unit_ast->token->value : NULL;

    for (int i = 0; i < impl_block->child_count; i++) {
        AST* decl_node = impl_block->children[i];
        if (decl_node && (decl_node->type == AST_PROCEDURE_DECL || decl_node->type == AST_FUNCTION_DECL)) {
            compileNode(decl_node, outputChunk, getLine(decl_node));
        }
    }

    // Reset the context after finishing the unit
    current_compilation_unit_name = NULL;
}

void finalizeBytecode(BytecodeChunk* chunk) {

    if (!procedure_table || !chunk || !chunk->code) return;

    for (int offset = 0; offset < chunk->count; ) {
        uint8_t opcode = chunk->code[offset];

        if (opcode == OP_CALL) {
            // Ensure we can read the full OP_CALL instruction
            if (offset + 5 >= chunk->count) {
                fprintf(stderr, "Compiler Error: Malformed OP_CALL instruction at offset %d.\n", offset);
                compiler_had_error = true;
                break;
            }

            uint16_t address = (uint16_t)((chunk->code[offset + 3] << 8) |
                                          chunk->code[offset + 4]);

            // Check if this is a placeholder that needs patching.
            if (address == 0xFFFF) {
                uint16_t name_index = (uint16_t)((chunk->code[offset + 1] << 8) |
                                                 chunk->code[offset + 2]);
                if (name_index >= chunk->constants_count) {
                    fprintf(stderr, "Compiler Error: Invalid name index in OP_CALL at offset %d.\n", name_index);
                    compiler_had_error = true;
                    offset += 6; // Skip this malformed instruction
                    continue;
                }

                Value name_val = chunk->constants[name_index];
                if (name_val.type != TYPE_STRING) {
                    fprintf(stderr, "Compiler Error: Constant at index %d is not a string for OP_CALL.\n", name_index);
                    compiler_had_error = true;
                    offset += 6; // Skip
                    continue;
                }

                const char* proc_name = name_val.s_val;

                // The procedure table stores all routine names in lowercase.
                // Convert the looked-up name to lowercase before searching.
                char lookup_name[MAX_SYMBOL_LENGTH];
                strncpy(lookup_name, proc_name, sizeof(lookup_name) - 1);
                lookup_name[sizeof(lookup_name) - 1] = '\0';
                toLowerString(lookup_name);

                // Directly search the global procedure table instead of relying on
                // lookupProcedure(), which depends on current_procedure_table.
                Symbol* symbol_to_patch = hashTableLookup(procedure_table, lookup_name);
                if (symbol_to_patch && symbol_to_patch->is_alias) {
                    symbol_to_patch = symbol_to_patch->real_symbol;
                }

                if (symbol_to_patch && symbol_to_patch->is_defined) {
                    // Patch the address in place. The address occupies bytes offset+3 and offset+4.
                    patchShort(chunk, offset + 3, (uint16_t)symbol_to_patch->bytecode_address);
                } else {
                    fprintf(stderr, "Compiler Error: Procedure '%s' was called but never defined.\n", proc_name);
                    compiler_had_error = true;
                }
            }
            offset += 6; // Advance past the 6-byte OP_CALL instruction
        } else {
            // For any other instruction, use the new helper to get the correct length and advance the offset.
            offset += getInstructionLength(chunk, offset);
        }
    }
}
