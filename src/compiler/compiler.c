#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // For strcmp, strdup, atoll
#include <math.h>
#include <ctype.h>
#include <strings.h>
#include <limits.h>
#include <stdint.h>

#include "compiler/compiler.h"
#include "backend_ast/builtin.h" // For isBuiltin
#include "core/utils.h"
#include "core/types.h"
#include "Pascal/globals.h"
#include "common/frontend_kind.h"
#include "ast/ast.h"
#include "symbol/symbol.h" // For access to the main global symbol table, if needed,
                           // though for bytecode compilation, we often build our own tables/mappings.
#include "vm/vm.h"         // For HostFunctionID
#include "compiler/bytecode.h"
#include <stdlib.h>

// Debug printing gate
static int compiler_debug = 0;
#define DBG_PRINTF(...) do { if (compiler_debug) fprintf(stderr, __VA_ARGS__); } while(0)

#define MAX_GLOBALS 256 // Define a reasonable limit for global variables for now
#define NO_VTABLE_ENTRY -1

static bool compiler_had_error = false;
static const char* current_compilation_unit_name = NULL;
static AST* gCurrentProgramRoot = NULL;
static HashTable* current_class_const_table = NULL;
static AST* current_class_record_type = NULL;
static int compiler_dynamic_locals = 0;

typedef struct {
    int constant_index;
    int original_address;
    int element_index;
} AddressConstantEntry;

static VarType resolveOrdinalBuiltinTypeName(const char *name) {
    if (!name) return TYPE_UNKNOWN;

    if (strcasecmp(name, "integer") == 0) return TYPE_INT32;
    if (strcasecmp(name, "char") == 0) return TYPE_CHAR;
    if (strcasecmp(name, "boolean") == 0) return TYPE_BOOLEAN;
    if (strcasecmp(name, "byte") == 0) return TYPE_BYTE;
    if (strcasecmp(name, "word") == 0) return TYPE_WORD;

    return TYPE_UNKNOWN;
}

static AddressConstantEntry* address_constant_entries = NULL;
static int address_constant_count = 0;
static int address_constant_capacity = 0;
typedef struct {
    BytecodeChunk* chunk;
    char** classes;
    int count;
    int capacity;
} VTableTrackerState;

typedef enum InterfaceBoxingResult {
    INTERFACE_BOX_NOT_NEEDED = 0,
    INTERFACE_BOX_DONE = 1,
    INTERFACE_BOX_FAILED = -1
} InterfaceBoxingResult;

static AST* resolveTypeAlias(AST* type_node);
static AST* getRecordTypeFromExpr(AST* expr);
static AST* resolveInterfaceAST(AST* typeNode);
static InterfaceBoxingResult maybeAutoBoxInterfaceForType(AST* interfaceType,
                                                          AST* valueExpr,
                                                          BytecodeChunk* chunk,
                                                          int line,
                                                          bool recoverWithNil,
                                                          bool strictRecord);
static bool emitImplicitMyselfFieldValue(BytecodeChunk* chunk, int line, const char* fieldName);
static BytecodeChunk* tracked_vtable_chunk = NULL;
static char** emitted_vtable_classes = NULL;
static int emitted_vtable_count = 0;
static int emitted_vtable_capacity = 0;
static VTableTrackerState* vtable_tracker_stack = NULL;
static int vtable_tracker_depth = 0;
static int vtable_tracker_capacity = 0;

static void freeVTableClassList(char** list, int count) {
    if (!list) return;
    for (int i = 0; i < count; i++) {
        free(list[i]);
    }
    free(list);
}

static void clearCurrentVTableTracker(void) {
    if (emitted_vtable_classes) {
        for (int i = 0; i < emitted_vtable_count; i++) {
            free(emitted_vtable_classes[i]);
        }
        free(emitted_vtable_classes);
    }
    emitted_vtable_classes = NULL;
    emitted_vtable_count = 0;
    emitted_vtable_capacity = 0;
}

static void initializeVTableTracker(BytecodeChunk* chunk) {
    if (vtable_tracker_depth != 0) {
        return;
    }
    clearCurrentVTableTracker();
    tracked_vtable_chunk = chunk;
}

static void ensureVTableTrackerForChunk(BytecodeChunk* chunk) {
    if (tracked_vtable_chunk == chunk) {
        return;
    }
    if (vtable_tracker_depth == 0) {
        clearCurrentVTableTracker();
    }
    tracked_vtable_chunk = chunk;
}

static bool vtableTrackerHasClass(const char* class_name) {
    if (!class_name) return false;
    for (int i = 0; i < emitted_vtable_count; i++) {
        if (strcmp(emitted_vtable_classes[i], class_name) == 0) {
            return true;
        }
    }
    return false;
}

static bool vtableTrackerEnsureCapacity(int needed) {
    if (needed <= emitted_vtable_capacity) {
        return true;
    }
    int new_capacity = emitted_vtable_capacity < 8 ? 8 : emitted_vtable_capacity * 2;
    while (new_capacity < needed) {
        if (new_capacity > INT_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    char** resized = realloc(emitted_vtable_classes,
                             (size_t)new_capacity * sizeof(char*));
    if (!resized) {
        fprintf(stderr, "Compiler error: Out of memory expanding vtable tracker.\n");
        compiler_had_error = true;
        return false;
    }
    emitted_vtable_classes = resized;
    emitted_vtable_capacity = new_capacity;
    return true;
}

static void vtableTrackerRecordClass(const char* class_name) {
    if (!class_name || vtableTrackerHasClass(class_name)) {
        return;
    }
    if (!vtableTrackerEnsureCapacity(emitted_vtable_count + 1)) {
        return;
    }
    char* copy = strdup(class_name);
    if (!copy) {
        fprintf(stderr, "Compiler error: Out of memory storing vtable tracker entry.\n");
        compiler_had_error = true;
        return;
    }
    emitted_vtable_classes[emitted_vtable_count++] = copy;
}

static bool pushVTableTrackerState(BytecodeChunk* chunk) {
    if (vtable_tracker_depth == vtable_tracker_capacity) {
        int new_capacity = vtable_tracker_capacity < 4 ? 4 : vtable_tracker_capacity * 2;
        VTableTrackerState* resized = realloc(vtable_tracker_stack,
                                              (size_t)new_capacity * sizeof(VTableTrackerState));
        if (!resized) {
            fprintf(stderr, "Compiler error: Out of memory growing vtable tracker stack.\n");
            compiler_had_error = true;
            return false;
        }
        vtable_tracker_stack = resized;
        vtable_tracker_capacity = new_capacity;
    }

    VTableTrackerState saved = {
        .chunk = tracked_vtable_chunk,
        .classes = emitted_vtable_classes,
        .count = emitted_vtable_count,
        .capacity = emitted_vtable_capacity,
    };
    vtable_tracker_stack[vtable_tracker_depth++] = saved;

    tracked_vtable_chunk = chunk;
    emitted_vtable_classes = NULL;
    emitted_vtable_count = 0;
    emitted_vtable_capacity = 0;

    if (saved.chunk == chunk && saved.classes && saved.count > 0) {
        emitted_vtable_capacity = saved.count;
        emitted_vtable_classes = (char**)malloc((size_t)saved.count * sizeof(char*));
        if (!emitted_vtable_classes) {
            fprintf(stderr, "Compiler error: Out of memory copying vtable tracker.\n");
            compiler_had_error = true;
            emitted_vtable_capacity = 0;
            return true;
        }
        for (int i = 0; i < saved.count; i++) {
            emitted_vtable_classes[i] = strdup(saved.classes[i]);
            if (!emitted_vtable_classes[i]) {
                fprintf(stderr, "Compiler error: Out of memory copying vtable tracker entry.\n");
                compiler_had_error = true;
                for (int j = 0; j < i; j++) {
                    free(emitted_vtable_classes[j]);
                }
                free(emitted_vtable_classes);
                emitted_vtable_classes = NULL;
                emitted_vtable_capacity = 0;
                emitted_vtable_count = 0;
                return true;
            }
        }
        emitted_vtable_count = saved.count;
    }
    return true;
}

static void popVTableTrackerState(void) {
    VTableTrackerState child = {
        .chunk = tracked_vtable_chunk,
        .classes = emitted_vtable_classes,
        .count = emitted_vtable_count,
        .capacity = emitted_vtable_capacity,
    };

    if (vtable_tracker_depth <= 0) {
        if (child.classes) {
            freeVTableClassList(child.classes, child.count);
        }
        tracked_vtable_chunk = NULL;
        emitted_vtable_classes = NULL;
        emitted_vtable_count = 0;
        emitted_vtable_capacity = 0;
        return;
    }

    VTableTrackerState parent = vtable_tracker_stack[--vtable_tracker_depth];
    tracked_vtable_chunk = parent.chunk;
    emitted_vtable_classes = parent.classes;
    emitted_vtable_count = parent.count;
    emitted_vtable_capacity = parent.capacity;

    if (!parent.chunk && child.chunk) {
        tracked_vtable_chunk = child.chunk;
        emitted_vtable_classes = child.classes;
        emitted_vtable_count = child.count;
        emitted_vtable_capacity = child.capacity;
        return;
    }

    if (parent.chunk == child.chunk && child.classes) {
        for (int i = 0; i < child.count; i++) {
            vtableTrackerRecordClass(child.classes[i]);
        }
    }

    if (child.classes) {
        freeVTableClassList(child.classes, child.count);
    }
}

static void recordAddressConstantEntry(int constant_index, int element_index, int address) {
    if (constant_index < 0 || address < 0) return;
    if (address_constant_count >= address_constant_capacity) {
        int new_capacity = address_constant_capacity < 8 ? 8 : address_constant_capacity * 2;
        AddressConstantEntry* resized = (AddressConstantEntry*)realloc(address_constant_entries,
                                                                      (size_t)new_capacity * sizeof(AddressConstantEntry));
        if (!resized) {
            return;
        }
        address_constant_entries = resized;
        address_constant_capacity = new_capacity;
    }
    address_constant_entries[address_constant_count].constant_index = constant_index;
    address_constant_entries[address_constant_count].original_address = address;
    address_constant_entries[address_constant_count].element_index = element_index;
    address_constant_count++;
}

static void recordAddressConstant(int constant_index, int address) {
    recordAddressConstantEntry(constant_index, -1, address);
}

static void recordArrayAddressConstant(int constant_index, int element_index, int address) {
    recordAddressConstantEntry(constant_index, element_index, address);
}

static void resetAddressConstantTracking(void) {
    address_constant_count = 0;
}

void compilerEnableDynamicLocals(int enable) {
    compiler_dynamic_locals = enable ? 1 : 0;
}

void compilerSetCurrentUnitName(const char *name) {
    current_compilation_unit_name = name;
}

static bool astNodeIsDescendant(AST* ancestor, AST* node) {
    if (!ancestor || !node) return false;
    for (AST* cur = node; cur != NULL; cur = cur->parent) {
        if (cur == ancestor) return true;
    }
    return false;
}

// Forward declarations for helpers used before definition
static void emitConstant(BytecodeChunk* chunk, int constant_index, int line);
static void emitDefineGlobal(BytecodeChunk* chunk, int name_idx, int line);
static void emitConstantIndex16(BytecodeChunk* chunk, int constant_index, int line);
static void emitGlobalNameIdx(BytecodeChunk* chunk, OpCode op8, OpCode op16,
                              int name_idx, int line);
static bool recordTypeHasVTable(AST* recordType);
static void queueDeferredGlobalInitializer(AST* var_decl);
static void emitDeferredGlobalInitializers(BytecodeChunk* chunk);
static void emitGlobalInitializerForVar(AST* var_decl, AST* varNameNode,
                                       AST* actual_type_def_node,
                                       BytecodeChunk* chunk);
static void emitGlobalVarDefinition(AST* var_decl,
                                    AST* varNameNode,
                                    AST* type_specifier_node,
                                    AST* actual_type_def_node,
                                    BytecodeChunk* chunk,
                                    bool emit_initializer);
static int resolveGlobalVariableIndex(BytecodeChunk* chunk, const char* name, int line);

typedef struct {
    char* name;
    int depth; // Scope depth
    bool is_ref;
    bool is_captured;
    AST* decl_node;
} CompilerLocal;

#define MAX_LOOP_DEPTH 16 // Max nested loops

typedef struct {
    int start;          // Address of the loop's start
    int* break_jumps;   // Dynamic array of jump instructions from 'break'
    int break_count;    // Number of 'break' statements
    int* continue_jumps; // Dynamic array of jump instructions from 'continue'
    int continue_count;  // Number of 'continue' statements
    int continue_target; // If known, 'continue' jumps directly here
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
    int max_local_count;
    int max_slot_used;
    int scope_depth;
    const char* name;
    struct FunctionCompilerState* enclosing;
    Symbol* function_symbol;
    CompilerUpvalue upvalues[MAX_UPVALUES];
    int upvalue_count;
    bool returns_value;
} FunctionCompilerState;

static FunctionCompilerState* current_function_compiler = NULL;

static bool isCurrentFunctionResultIdentifier(const FunctionCompilerState* fc, const char* name) {
    if (!fc || !fc->returns_value || !name) {
        return false;
    }

    if (strcasecmp(name, "result") == 0) {
        return true;
    }

    if (fc->name && strcasecmp(name, fc->name) == 0) {
        return true;
    }

    if (fc->name) {
        const char* dot = strrchr(fc->name, '.');
        if (dot && dot[1] != '\0' && strcasecmp(name, dot + 1) == 0) {
            return true;
        }
    }

    return false;
}

// Track global objects created with NEW so their hidden
// vtable fields can be initialised after all vtables are defined.
typedef struct {
    char* var_name;
    char* class_name;
} PendingGlobalVTableInit;

static PendingGlobalVTableInit* pending_global_vtables = NULL;
static int pending_global_vtable_count = 0;

static bool postpone_global_initializers = false;
static AST** deferred_global_initializers = NULL;
static int deferred_global_initializer_count = 0;
static int deferred_global_initializer_capacity = 0;

// Flag indicating we are compiling a global variable initializer. In that
// situation vtables have not yet been emitted, so NEW expressions should not
// attempt to resolve their class vtables immediately.
static bool compiling_global_var_init = false;
// Tracks nested NEW expressions when compiling a global initializer so that
// only the outermost object defers its vtable setup. Nested NEWs must initialize
// their vtable pointers immediately.
static int global_init_new_depth = 0;

static bool compiler_defined_myself_global = false;
static int compiler_myself_global_name_idx = -1;

typedef struct {
    int offset;
    int line;
} LabelPatch;

typedef struct {
    char* name;
    int declared_line;
    int defined_line;
    int bytecode_offset;
    LabelPatch* patches;
    int patch_count;
    int patch_capacity;
} LabelInfo;

typedef struct LabelTableState {
    LabelInfo* labels;
    int label_count;
    int label_capacity;
    struct LabelTableState* enclosing;
} LabelTableState;

static LabelTableState* current_label_table = NULL;

static void initLabelTable(LabelTableState* table);
static void finalizeLabelTable(LabelTableState* table, const char* context_name);
static void registerLabelDeclarations(AST* node);
static void declareLabel(Token* token);
static void defineLabel(Token* token, BytecodeChunk* chunk, int line);
static void compileGotoStatement(AST* node, BytecodeChunk* chunk, int line);

static int addStringConstant(BytecodeChunk* chunk, const char* str) {
    Value val = makeString(str);
    int index = addConstantToChunk(chunk, &val);
    freeValue(&val); // The temporary Value's contents are freed here.
    return index;
}

static int addStringConstantLen(BytecodeChunk* chunk, const char* str, size_t len) {
    Value val = makeStringLen(str, len);
    int index = addConstantToChunk(chunk, &val);
    freeValue(&val);
    return index;
}

static int ensureBuiltinStringConstants(BytecodeChunk* chunk, const char* original_name, int* out_lower_index) {
    if (!original_name) {
        original_name = "";
    }

    int name_index = addStringConstant(chunk, original_name);
    int lower_index = getBuiltinLowercaseIndex(chunk, name_index);
    if (lower_index < 0) {
        char normalized_name[MAX_SYMBOL_LENGTH];
        strncpy(normalized_name, original_name, sizeof(normalized_name) - 1);
        normalized_name[sizeof(normalized_name) - 1] = '\0';
        toLowerString(normalized_name);
        lower_index = addStringConstant(chunk, normalized_name);
        setBuiltinLowercaseIndex(chunk, name_index, lower_index);
    }
    if (out_lower_index) {
        *out_lower_index = lower_index;
    }
    return name_index;
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

static int ensureMyselfGlobalNameIndex(BytecodeChunk* chunk) {
    if (compiler_myself_global_name_idx < 0) {
        compiler_myself_global_name_idx = addStringConstant(chunk, "myself");
    }
    return compiler_myself_global_name_idx;
}

static void emitBuiltinProcedureCall(BytecodeChunk* chunk, const char* name, uint8_t arg_count, int line) {
    if (!name) name = "";

    int name_index = ensureBuiltinStringConstants(chunk, name, NULL);
    int builtin_id = getBuiltinIDForCompiler(name);
    if (builtin_id < 0) {
        fprintf(stderr, "L%d: Compiler Error: Unknown built-in procedure '%s'.\n", line, name);
        compiler_had_error = true;

        writeBytecodeChunk(chunk, CALL_BUILTIN, line);
        emitShort(chunk, (uint16_t)name_index, line);
        writeBytecodeChunk(chunk, arg_count, line);
        return;
    }

    writeBytecodeChunk(chunk, CALL_BUILTIN_PROC, line);
    emitShort(chunk, (uint16_t)builtin_id, line);
    emitShort(chunk, (uint16_t)name_index, line);
    writeBytecodeChunk(chunk, arg_count, line);
}

static void ensureMyselfGlobalDefined(BytecodeChunk* chunk, int line) {
    if (compiler_defined_myself_global) return;
    int myself_idx = ensureMyselfGlobalNameIndex(chunk);
    emitConstant(chunk, addNilConstant(chunk), line);
    emitGlobalNameIdx(chunk, DEFINE_GLOBAL, DEFINE_GLOBAL16, myself_idx, line);
    // Declare the implicit "myself" variable as a generic pointer with a
    // placeholder type name.  The VM's DEFINE_GLOBAL handler expects every
    // global definition to include the declared VarType and an associated
    // type-name constant index.  Previously we omitted these operands, which
    // caused the VM to misinterpret the bytecode stream for the first
    // statement following the implicit declaration (often a WRITELN), leading
    // to stack underflows at runtime.
    writeBytecodeChunk(chunk, (uint8_t)TYPE_POINTER, line);
    int pointer_type_name_idx = addStringConstant(chunk, "");
    emitConstantIndex16(chunk, pointer_type_name_idx, line);
    compiler_defined_myself_global = true;
}

// Determine if a variable declaration node resides in the true global scope.
// Walks up the AST parent chain and reports "global" only if no enclosing
// function or procedure is encountered before reaching the program root.
static bool isGlobalScopeNode(AST* node) {
    AST* p = node;
    while (p) {
        if (p->type == AST_FUNCTION_DECL || p->type == AST_PROCEDURE_DECL) {
            return false; // inside routine -> not global
        }
        if (p->type == AST_PROGRAM) {
            return true; // reached program root with no routine -> global
        }
        p = p->parent;
    }
    // If we couldn't find a program node, err on the side of "local"
    // to avoid misclassifying routine locals as globals.
    return false;

}

static LabelInfo* findLabelInfo(LabelTableState* table, const char* name) {
    if (!table || !name) return NULL;
    for (int i = 0; i < table->label_count; i++) {
        LabelInfo* info = &table->labels[i];
        if (info->name && strcasecmp(info->name, name) == 0) {
            return info;
        }
    }
    return NULL;
}

static bool ensureLabelTableCapacity(LabelTableState* table, int needed) {
    if (!table) return false;
    if (needed <= table->label_capacity) return true;

    int new_capacity = table->label_capacity < 8 ? 8 : table->label_capacity * 2;
    while (new_capacity < needed) {
        if (new_capacity > INT_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }

    LabelInfo* resized = realloc(table->labels, (size_t)new_capacity * sizeof(LabelInfo));
    if (!resized) {
        fprintf(stderr, "Compiler error: Out of memory expanding label table.\n");
        compiler_had_error = true;
        return false;
    }

    for (int i = table->label_capacity; i < new_capacity; i++) {
        resized[i].name = NULL;
        resized[i].declared_line = -1;
        resized[i].defined_line = -1;
        resized[i].bytecode_offset = -1;
        resized[i].patches = NULL;
        resized[i].patch_count = 0;
        resized[i].patch_capacity = 0;
    }

    table->labels = resized;
    table->label_capacity = new_capacity;
    return true;
}

static bool ensurePatchCapacity(LabelInfo* info, int needed) {
    if (!info) return false;
    if (needed <= info->patch_capacity) return true;

    int new_capacity = info->patch_capacity < 4 ? 4 : info->patch_capacity * 2;
    while (new_capacity < needed) {
        if (new_capacity > INT_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }

    LabelPatch* resized = realloc(info->patches, (size_t)new_capacity * sizeof(LabelPatch));
    if (!resized) {
        fprintf(stderr, "Compiler error: Out of memory expanding goto patch list.\n");
        compiler_had_error = true;
        return false;
    }

    info->patches = resized;
    info->patch_capacity = new_capacity;
    return true;
}

static void initLabelTable(LabelTableState* table) {
    if (!table) return;
    table->labels = NULL;
    table->label_count = 0;
    table->label_capacity = 0;
    table->enclosing = current_label_table;
    current_label_table = table;
}

static void finalizeLabelTable(LabelTableState* table, const char* context_name) {
    if (!table) return;
    const char* ctx = context_name ? context_name : "this routine";

    for (int i = 0; i < table->label_count; i++) {
        LabelInfo* info = &table->labels[i];
        if (info->name && info->bytecode_offset < 0) {
            fprintf(stderr,
                    "Compiler Error: label '%s' declared on line %d in %s was never defined.\n",
                    info->name,
                    info->declared_line,
                    ctx);
            compiler_had_error = true;
        }
        if (info->patch_count > 0 && info->bytecode_offset < 0) {
            int report_line = info->patches[0].line > 0 ? info->patches[0].line : info->declared_line;
            fprintf(stderr,
                    "L%d: Compiler Error: goto target '%s' is not defined in %s.\n",
                    report_line,
                    info->name ? info->name : "label",
                    ctx);
            compiler_had_error = true;
        }
        if (info->patches) {
            free(info->patches);
        }
        if (info->name) {
            free(info->name);
        }
    }

    free(table->labels);
    table->labels = NULL;
    table->label_count = 0;
    table->label_capacity = 0;
    current_label_table = table->enclosing;
    table->enclosing = NULL;
}

static void declareLabel(Token* token) {
    if (!current_label_table || !token || !token->value) return;

    LabelInfo* existing = findLabelInfo(current_label_table, token->value);
    if (existing) {
        fprintf(stderr,
                "L%d: Compiler Error: label '%s' is declared more than once (first declared at line %d).\n",
                token->line,
                existing->name ? existing->name : token->value,
                existing->declared_line);
        compiler_had_error = true;
        return;
    }

    if (!ensureLabelTableCapacity(current_label_table, current_label_table->label_count + 1)) {
        return;
    }

    LabelInfo* info = &current_label_table->labels[current_label_table->label_count++];
    info->name = strdup(token->value);
    if (!info->name) {
        fprintf(stderr, "Compiler error: Out of memory storing label name.\n");
        compiler_had_error = true;
        current_label_table->label_count--;
        return;
    }
    info->declared_line = token->line;
    info->defined_line = -1;
    info->bytecode_offset = -1;
    info->patches = NULL;
    info->patch_count = 0;
    info->patch_capacity = 0;
}

static void registerLabelDeclarations(AST* node) {
    if (!current_label_table || !node) return;
    if (node->type == AST_LABEL_DECL) {
        if (node->token) {
            declareLabel(node->token);
        }
        return;
    }
    if (node->type == AST_PROCEDURE_DECL || node->type == AST_FUNCTION_DECL) {
        return;
    }
    if (node->children) {
        for (int i = 0; i < node->child_count; i++) {
            registerLabelDeclarations(node->children[i]);
        }
    }
}

static void defineLabel(Token* token, BytecodeChunk* chunk, int line) {
    if (!current_label_table || !token || !token->value) return;

    int report_line = token->line > 0 ? token->line : line;
    LabelInfo* info = findLabelInfo(current_label_table, token->value);
    if (!info) {
        fprintf(stderr,
                "L%d: Compiler Error: label '%s' is not declared in this routine.\n",
                report_line,
                token->value);
        compiler_had_error = true;
        return;
    }
    if (info->bytecode_offset >= 0) {
        fprintf(stderr,
                "L%d: Compiler Error: label '%s' is defined more than once (previous definition at line %d).\n",
                report_line,
                token->value,
                info->defined_line);
        compiler_had_error = true;
        return;
    }

    info->bytecode_offset = chunk->count;
    info->defined_line = report_line;

    for (int i = 0; i < info->patch_count; i++) {
        int operand_index = info->patches[i].offset;
        int distance = info->bytecode_offset - (operand_index + 2);
        patchShort(chunk, operand_index, (uint16_t)distance);
    }
    if (info->patches) {
        free(info->patches);
        info->patches = NULL;
    }
    info->patch_count = 0;
    info->patch_capacity = 0;
}

static void compileGotoStatement(AST* node, BytecodeChunk* chunk, int line) {
    if (!node || !node->token || !node->token->value) return;
    if (!current_label_table) {
        fprintf(stderr,
                "L%d: Compiler Error: goto statements are not permitted in this context.\n",
                line);
        compiler_had_error = true;
        return;
    }

    LabelInfo* info = findLabelInfo(current_label_table, node->token->value);
    int report_line = node->token->line > 0 ? node->token->line : line;
    if (!info) {
        fprintf(stderr,
                "L%d: Compiler Error: goto target '%s' is not declared in this routine.\n",
                report_line,
                node->token->value);
        compiler_had_error = true;
        return;
    }

    writeBytecodeChunk(chunk, JUMP, line);
    int operand_index = chunk->count;
    emitShort(chunk, 0xFFFF, line);

    if (info->bytecode_offset >= 0) {
        int distance = info->bytecode_offset - (operand_index + 2);
        patchShort(chunk, operand_index, (uint16_t)distance);
    } else {
        if (!ensurePatchCapacity(info, info->patch_count + 1)) {
            return;
        }
        info->patches[info->patch_count].offset = operand_index;
        info->patches[info->patch_count].line = report_line;
        info->patch_count++;
    }
}

typedef struct {
    char* class_name;
    int method_count;
    int capacity;
    int* addrs;
    bool merged;
    bool has_unresolved;
} VTableInfo;

/* Ensure the global procedure table exists; callers often run after the shell
 * has swapped out Pascal state, leaving the table NULL. */
static bool ensureProcedureTableInitialized(void) {
    if (!procedure_table) {
        procedure_table = createHashTable();
    }
    if (!procedure_table) {
        return false;
    }
    if (!current_procedure_table) {
        current_procedure_table = procedure_table;
    }
    return true;
}

static int findVTableIndex(VTableInfo* tables, int table_count, const char* name) {
    for (int i = 0; i < table_count; i++) {
        if (strcmp(tables[i].class_name, name) == 0) return i;
    }
    return -1;
}

static void mergeParentTable(VTableInfo* tables, int table_count, VTableInfo* vt) {
    if (!vt || vt->merged) return;
    AST* cls = lookupType(vt->class_name);
    const char* parent_name = NULL;
    if (cls && cls->extra && cls->extra->token) parent_name = cls->extra->token->value;
    if (parent_name) {
        int pidx = findVTableIndex(tables, table_count, parent_name);
        if (pidx != -1) {
            mergeParentTable(tables, table_count, &tables[pidx]);
            VTableInfo* parent = &tables[pidx];
            if (vt->capacity < parent->method_count) {
                int newcap = parent->method_count;
                vt->addrs = realloc(vt->addrs, sizeof(int) * newcap);
                for (int j = vt->capacity; j < newcap; j++) vt->addrs[j] = NO_VTABLE_ENTRY;
                vt->capacity = newcap;
            }
            for (int j = 0; j < parent->method_count; j++) {
                if (vt->addrs[j] == NO_VTABLE_ENTRY) vt->addrs[j] = parent->addrs[j];
            }
            if (parent->method_count > vt->method_count) vt->method_count = parent->method_count;
            if (parent->has_unresolved) {
                vt->has_unresolved = true;
            }
        }
    }
    vt->merged = true;
}

static void emitVTables(BytecodeChunk* chunk) {
    if (!ensureProcedureTableInitialized()) {
        return;
    }
    ensureVTableTrackerForChunk(chunk);
    VTableInfo* tables = NULL;
    int table_count = 0;
    for (int b = 0; b < HASHTABLE_SIZE; b++) {
        Symbol* sym = procedure_table->buckets[b];
        while (sym) {
            Symbol* base = sym->is_alias ? sym->real_symbol : sym;
            if (base && base->type_def && base->type_def->is_virtual && sym->name) {
                const char* dot = strchr(sym->name, '.');
                if (dot) {
                    size_t cls_len = (size_t)(dot - sym->name);
                    char cls[256];
                    if (cls_len < sizeof(cls)) {
                        memcpy(cls, sym->name, cls_len);
                        cls[cls_len] = '\0';
                        int idx = -1;
                        for (int i = 0; i < table_count; i++) {
                            if (strcmp(tables[i].class_name, cls) == 0) { idx = i; break; }
                        }
                        if (idx == -1) {
                            tables = realloc(tables, sizeof(VTableInfo) * (table_count + 1));
                            idx = table_count++;
                            tables[idx].class_name = strdup(cls);
                            tables[idx].method_count = 0;
                            tables[idx].capacity = 0;
                            tables[idx].addrs = NULL;
                            tables[idx].merged = false;
                            tables[idx].has_unresolved = false;
                        }
                        int mindex = base->type_def->i_val;
                        if (mindex >= tables[idx].capacity) {
                            int newcap = mindex + 1;
                            tables[idx].addrs = realloc(tables[idx].addrs, sizeof(int) * newcap);
                            for (int j = tables[idx].capacity; j < newcap; j++) tables[idx].addrs[j] = NO_VTABLE_ENTRY;
                            tables[idx].capacity = newcap;
                        }
                        tables[idx].addrs[mindex] = base->bytecode_address;
                        if (base->bytecode_address <= 0) {
                            tables[idx].has_unresolved = true;
                        }
                        if (mindex + 1 > tables[idx].method_count) tables[idx].method_count = mindex + 1;
                    }
                }
            }
            sym = sym->next;
        }
    }

    for (int i = 0; i < table_count; i++) {
        mergeParentTable(tables, table_count, &tables[i]);
    }

    for (int i = 0; i < table_count; i++) {
        VTableInfo* vt = &tables[i];
        if (vt->method_count == 0) {
            free(vt->class_name);
            free(vt->addrs);
            continue;
        }
        if (vt->has_unresolved) {
            free(vt->class_name);
            free(vt->addrs);
            continue;
        }
        if (vtableTrackerHasClass(vt->class_name)) {
            free(vt->class_name);
            free(vt->addrs);
            continue;
        }
        int lb = 0;
        int ub = vt->method_count - 1;
        Value arr = makeArrayND(1, &lb, &ub, TYPE_INT32, NULL);
        for (int j = 0; j < vt->method_count; j++) {
            int addr = vt->addrs[j];
            arr.array_val[j] = makeInt(addr == NO_VTABLE_ENTRY ? 0 : addr);
        }
        int cidx = addConstantToChunk(chunk, &arr);
        for (int j = 0; j < vt->method_count; j++) {
            int addr = vt->addrs[j];
            if (addr != NO_VTABLE_ENTRY) {
                recordArrayAddressConstant(cidx, j, addr);
            }
        }
        freeValue(&arr);
        emitConstant(chunk, cidx, 0);
        char gname[512];
        snprintf(gname, sizeof(gname), "%s_vtable", vt->class_name);
        int nameIdx = addStringConstant(chunk, gname);
        emitDefineGlobal(chunk, nameIdx, 0);
        writeBytecodeChunk(chunk, (uint8_t)TYPE_ARRAY, 0); // variable type
        writeBytecodeChunk(chunk, 1, 0);                   // dimension count
        int lbIdx = addIntConstant(chunk, lb);
        int ubIdx = addIntConstant(chunk, ub);
        emitConstantIndex16(chunk, lbIdx, 0);              // lower bound
        emitConstantIndex16(chunk, ubIdx, 0);              // upper bound
        writeBytecodeChunk(chunk, (uint8_t)TYPE_INT32, 0); // element type
        int elemNameIdx = addStringConstant(chunk, "integer");
        emitConstantIndex16(chunk, elemNameIdx, 0);
        emitGlobalNameIdx(chunk, SET_GLOBAL, SET_GLOBAL16, nameIdx, 0);
        vtableTrackerRecordClass(vt->class_name);
        free(vt->class_name);
        free(vt->addrs);
    }
    free(tables);
}

static int getLine(AST* node);
static void compileRValue(AST* node, BytecodeChunk* chunk, int current_line_approx);

static bool emitClosureLiteral(Symbol *psym, BytecodeChunk *chunk, int line) {
    if (!psym) {
        return false;
    }

    int capture_count = psym->upvalue_count;
    if (capture_count > 0 && !current_function_compiler) {
        fprintf(stderr, "L%d: Compiler error: capturing closure cannot escape global scope.\n", line);
        compiler_had_error = true;
        return false;
    }

    for (int i = 0; i < capture_count; i++) {
        uint8_t slot_index = psym->upvalues[i].index;
        bool isLocal = psym->upvalues[i].isLocal;
        bool is_ref = psym->upvalues[i].is_ref;

        if (isLocal) {
            if (is_ref) {
                writeBytecodeChunk(chunk, GET_LOCAL_ADDRESS, line);
            } else {
                writeBytecodeChunk(chunk, GET_LOCAL, line);
            }
            writeBytecodeChunk(chunk, slot_index, line);
        } else {
            if (is_ref) {
                writeBytecodeChunk(chunk, GET_UPVALUE_ADDRESS, line);
            } else {
                writeBytecodeChunk(chunk, GET_UPVALUE, line);
            }
            writeBytecodeChunk(chunk, slot_index, line);
        }
    }

    int countConst = addIntConstant(chunk, capture_count);
    emitConstant(chunk, countConst, line);

    int addrConst = addIntConstant(chunk, psym->bytecode_address);
    recordAddressConstant(addrConst, psym->bytecode_address);
    emitConstant(chunk, addrConst, line);

    writeBytecodeChunk(chunk, CALL_HOST, line);
    writeBytecodeChunk(chunk, (uint8_t)HOST_FN_CREATE_CLOSURE, line);
    return true;
}

static void queueDeferredGlobalInitializer(AST* var_decl) {
    if (!var_decl) return;
    for (int i = 0; i < deferred_global_initializer_count; i++) {
        if (deferred_global_initializers[i] == var_decl) {
            return;
        }
    }
    if (deferred_global_initializer_count == deferred_global_initializer_capacity) {
        int new_cap = deferred_global_initializer_capacity == 0 ? 4
                                                                  : deferred_global_initializer_capacity * 2;
        AST** resized = realloc(deferred_global_initializers, sizeof(AST*) * new_cap);
        if (!resized) {
            fprintf(stderr, "Compiler error: Out of memory deferring global initializers.\n");
            compiler_had_error = true;
            return;
        }
        deferred_global_initializers = resized;
        deferred_global_initializer_capacity = new_cap;
    }
    deferred_global_initializers[deferred_global_initializer_count++] = var_decl;
}

static void emitGlobalInitializerForVar(AST* var_decl, AST* varNameNode,
                                       AST* actual_type_def_node,
                                       BytecodeChunk* chunk) {
    if (!var_decl || !varNameNode || !varNameNode->token || !varNameNode->token->value) {
        return;
    }
    AST* initializer = var_decl->left;
    if (!initializer) {
        return;
    }

        if (var_decl->var_type == TYPE_ARRAY && initializer->type == AST_ARRAY_LITERAL) {
        AST* array_type = actual_type_def_node;
        int dimension_count = array_type ? array_type->child_count : 0;
            if (dimension_count == 1) {
            AST* sub = array_type->children[0];
            Value low_v = evaluateCompileTimeValue(sub->left);
            Value high_v = evaluateCompileTimeValue(sub->right);
            int low = (low_v.type == TYPE_INTEGER) ? (int)low_v.i_val : 0;
            int high = (high_v.type == TYPE_INTEGER) ? (int)high_v.i_val : -1;
            freeValue(&low_v);
            freeValue(&high_v);
            int lb[1] = { low };
            int ub[1] = { high };
            AST* elem_type_node = array_type->right;
            VarType elem_type = elem_type_node ? elem_type_node->var_type : TYPE_VOID;
            Value arr_val = makeArrayND(1, lb, ub, elem_type, elem_type_node);
            int total = calculateArrayTotalSize(&arr_val);
            for (int j = 0; j < total && j < initializer->child_count; j++) {
                Value ev = evaluateCompileTimeValue(initializer->children[j]);
                freeValue(&arr_val.array_val[j]);
                arr_val.array_val[j] = makeCopyOfValue(&ev);
                freeValue(&ev);
            }
            int constIdx = addConstantToChunk(chunk, &arr_val);
            freeValue(&arr_val);
            emitConstant(chunk, constIdx, getLine(var_decl));
            } else {
                compileRValue(initializer, chunk, getLine(initializer));
                maybeAutoBoxInterfaceForType(actual_type_def_node, initializer, chunk, getLine(initializer), true, false);
            }
        } else {
            bool prev_global_init = compiling_global_var_init;
            bool set_global_guard = (current_function_compiler == NULL && initializer->type == AST_NEW);
            if (set_global_guard) compiling_global_var_init = true;
            compileRValue(initializer, chunk, getLine(initializer));
            maybeAutoBoxInterfaceForType(actual_type_def_node, initializer, chunk, getLine(initializer), true, false);
            if (set_global_guard) compiling_global_var_init = prev_global_init;
        if (set_global_guard && initializer->token && initializer->token->value) {
            char* lower_cls = strdup(initializer->token->value);
            if (!lower_cls) {
                fprintf(stderr, "Compiler error: Memory allocation failed for class name.\n");
                compiler_had_error = true;
            } else {
                toLowerString(lower_cls);
                AST* clsType = lookupType(lower_cls);
                if (recordTypeHasVTable(clsType)) {
                    PendingGlobalVTableInit* resized = realloc(
                        pending_global_vtables,
                        sizeof(PendingGlobalVTableInit) * (pending_global_vtable_count + 1));
                    if (!resized) {
                        fprintf(stderr, "Compiler error: Out of memory queuing global vtable init.\n");
                        free(lower_cls);
                        compiler_had_error = true;
                    } else {
                        pending_global_vtables = resized;
                        pending_global_vtables[pending_global_vtable_count].var_name =
                            strdup(varNameNode->token->value);
                        pending_global_vtables[pending_global_vtable_count].class_name = lower_cls;
                        pending_global_vtable_count++;
                    }
                } else {
                    free(lower_cls);
                }
            }
        }
    }

    int name_idx_set = addStringConstant(chunk, varNameNode->token->value);
    emitGlobalNameIdx(chunk, SET_GLOBAL, SET_GLOBAL16, name_idx_set, getLine(varNameNode));
}

static void emitGlobalVarDefinition(AST* var_decl,
                                    AST* varNameNode,
                                    AST* type_specifier_node,
                                    AST* actual_type_def_node,
                                    BytecodeChunk* chunk,
                                    bool emit_initializer) {
    if (!var_decl || !varNameNode || !varNameNode->token) {
        return;
    }

    int line = getLine(varNameNode);
    int var_name_idx = addStringConstant(chunk, varNameNode->token->value);
    emitDefineGlobal(chunk, var_name_idx, line);
    writeBytecodeChunk(chunk, (uint8_t)var_decl->var_type, line);

    if (var_decl->var_type == TYPE_ARRAY) {
        int dimension_count = actual_type_def_node ? actual_type_def_node->child_count : 0;
        if (dimension_count > 255) {
            fprintf(stderr, "L%d: Compiler error: Maximum array dimensions (255) exceeded.\n", line);
            compiler_had_error = true;
            return;
        }
        writeBytecodeChunk(chunk, (uint8_t)dimension_count, line);

        for (int dim = 0; dim < dimension_count; dim++) {
            AST* subrange = actual_type_def_node->children[dim];
            if (subrange && subrange->type == AST_SUBRANGE) {
                Value lower_b = evaluateCompileTimeValue(subrange->left);
                Value upper_b = evaluateCompileTimeValue(subrange->right);

                if (IS_INTLIKE(lower_b)) {
                    emitConstantIndex16(chunk,
                                        addIntConstant(chunk, AS_INTEGER(lower_b)),
                                        line);
                } else {
                    fprintf(stderr,
                            "L%d: Compiler error: Array bound did not evaluate to a constant integer.\n",
                            line);
                    compiler_had_error = true;
                }
                freeValue(&lower_b);

                if (IS_INTLIKE(upper_b)) {
                    emitConstantIndex16(chunk,
                                        addIntConstant(chunk, AS_INTEGER(upper_b)),
                                        line);
                } else {
                    fprintf(stderr,
                            "L%d: Compiler error: Array bound did not evaluate to a constant integer.\n",
                            line);
                    compiler_had_error = true;
                }
                freeValue(&upper_b);

            } else {
                fprintf(stderr,
                        "L%d: Compiler error: Malformed array definition for '%s'.\n",
                        line,
                        varNameNode->token->value);
                compiler_had_error = true;
                emitShort(chunk, 0, line);
                emitShort(chunk, 0, line);
            }
        }

        AST* elem_type = actual_type_def_node ? actual_type_def_node->right : NULL;
        writeBytecodeChunk(chunk, (uint8_t)(elem_type ? elem_type->var_type : TYPE_VOID), line);
        const char* elem_type_name = (elem_type && elem_type->token) ? elem_type->token->value : "";
        emitConstantIndex16(chunk, addStringConstant(chunk, elem_type_name), line);
    } else {
        const char* type_name = "";
        if (var_decl->var_type == TYPE_POINTER) {
            AST* ptr_ast = type_specifier_node ? type_specifier_node : actual_type_def_node;
            if (ptr_ast && ptr_ast->type == AST_POINTER_TYPE) {
                if (ptr_ast->right && ptr_ast->right->token) {
                    type_name = ptr_ast->right->token->value;
                } else if (ptr_ast->token && ptr_ast->token->value) {
                    type_name = ptr_ast->token->value;
                }
            }
        }
        if (type_name[0] == '\0' && type_specifier_node && type_specifier_node->token && type_specifier_node->token->value) {
            type_name = type_specifier_node->token->value;
        } else if (type_name[0] == '\0' && actual_type_def_node && actual_type_def_node->token && actual_type_def_node->token->value) {
            type_name = actual_type_def_node->token->value;
        }
        emitConstantIndex16(chunk, addStringConstant(chunk, type_name), line);

        if (var_decl->var_type == TYPE_STRING) {
            int max_len = 0;
            if (actual_type_def_node && actual_type_def_node->right) {
                Value len_val = evaluateCompileTimeValue(actual_type_def_node->right);
                if (len_val.type == TYPE_INTEGER) {
                    max_len = (int)len_val.i_val;
                }
                freeValue(&len_val);
            }
            emitConstantIndex16(chunk, addIntConstant(chunk, max_len), line);
        } else if (var_decl->var_type == TYPE_FILE) {
            VarType file_element_type = TYPE_VOID;
            const char *file_element_name = "";
            bool is_text_file = false;

            AST *resolved_file_type = resolveTypeAlias(actual_type_def_node);
            if (resolved_file_type && resolved_file_type->type == AST_TYPE_DECL && resolved_file_type->left) {
                resolved_file_type = resolveTypeAlias(resolved_file_type->left);
            }
            if (resolved_file_type && resolved_file_type->type == AST_VAR_DECL && resolved_file_type->right) {
                resolved_file_type = resolveTypeAlias(resolved_file_type->right);
            }
            if (resolved_file_type && resolved_file_type->type == AST_VARIABLE &&
                resolved_file_type->token && resolved_file_type->token->value) {
                const char *file_token = resolved_file_type->token->value;
                if (strcasecmp(file_token, "file") == 0 && resolved_file_type->right) {
                    AST *element_node = resolveTypeAlias(resolved_file_type->right);
                    AST *source_node = element_node ? element_node : resolved_file_type->right;
                    if (source_node && source_node->var_type != TYPE_VOID && source_node->var_type != TYPE_UNKNOWN) {
                        file_element_type = source_node->var_type;
                    }
                    if (source_node && source_node->token && source_node->token->value) {
                        file_element_name = source_node->token->value;
                    }
                } else if (strcasecmp(file_token, "text") == 0) {
                    is_text_file = true;
                    file_element_type = TYPE_VOID;
                    file_element_name = "";
                }
            }

            writeBytecodeChunk(chunk, (uint8_t)file_element_type, line);
            if (!is_text_file && file_element_name && file_element_name[0]) {
                emitConstantIndex16(chunk, addStringConstant(chunk, file_element_name), line);
            } else {
                emitShort(chunk, 0xFFFF, line);
            }
        }
    }

    resolveGlobalVariableIndex(chunk, varNameNode->token->value, line);

    if (emit_initializer && var_decl->left) {
        emitGlobalInitializerForVar(var_decl, varNameNode, actual_type_def_node, chunk);
    }
}

static void emitDeferredGlobalInitializers(BytecodeChunk* chunk) {
    for (int i = 0; i < deferred_global_initializer_count; i++) {
        AST* decl = deferred_global_initializers[i];
        if (!decl) continue;
        AST* type_specifier_node = decl->right;
        AST* actual_type_def_node = type_specifier_node;
        if (actual_type_def_node && actual_type_def_node->type == AST_TYPE_REFERENCE) {
            AST* resolved_node = lookupType(actual_type_def_node->token->value);
            if (resolved_node) {
                actual_type_def_node = resolved_node;
            } else {
                fprintf(stderr,
                        "L%d: identifier '%s' not in scope.\n",
                        getLine(actual_type_def_node),
                        actual_type_def_node->token ? actual_type_def_node->token->value : "?");
                compiler_had_error = true;
                continue;
            }
        }
        if (!actual_type_def_node) {
            fprintf(stderr,
                    "L%d: Compiler error: Could not determine type definition for a variable declaration.\n",
                    getLine(decl));
            compiler_had_error = true;
            continue;
        }
        for (int j = 0; j < decl->child_count; j++) {
            AST* varNameNode = decl->children[j];
            if (!varNameNode || !varNameNode->token) continue;
            emitGlobalVarDefinition(decl,
                                    varNameNode,
                                    type_specifier_node,
                                    actual_type_def_node,
                                    chunk,
                                    decl->left != NULL);
        }
    }
    deferred_global_initializer_count = 0;
    if (deferred_global_initializers) {
        free(deferred_global_initializers);
        deferred_global_initializers = NULL;
        deferred_global_initializer_capacity = 0;
    }
}

// Return an ordinal ranking for integer-like types so we can detect
// potential narrowing conversions. Larger ranks represent wider types.
static int intTypeRank(VarType t) {
    switch (t) {
        case TYPE_INT64:
        case TYPE_UINT64:
            return 64;
        case TYPE_INT32:
        case TYPE_UINT32:
            return 32;
        case TYPE_INT16:
        case TYPE_UINT16:
        case TYPE_WORD:
            return 16;
        case TYPE_INT8:
        case TYPE_UINT8:
        case TYPE_BYTE:
        case TYPE_BOOLEAN:
        case TYPE_CHAR:
            return 8;
        default:
            return 0;
    }
}

static bool isUnsignedIntVarType(VarType t) {
    switch (t) {
        case TYPE_UINT8:
        case TYPE_UINT16:
        case TYPE_UINT32:
        case TYPE_UINT64:
        case TYPE_BYTE:
        case TYPE_WORD:
            return true;
        default:
            return false;
    }
}

static bool constantFitsInIntType(AST* expr, VarType targetType) {
    if (!expr || !isIntlikeType(targetType)) return false;

    Value const_val = evaluateCompileTimeValue(expr);
    bool fits = false;

    if (const_val.type == TYPE_VOID || const_val.type == TYPE_UNKNOWN) {
        freeValue(&const_val);
        return false;
    }

    bool hasOrdinal = false;
    long long sval = 0;
    unsigned long long uval = 0;
    bool value_is_unsigned = false;

    if (const_val.type == TYPE_ENUM) {
        sval = const_val.enum_val.ordinal;
        uval = (unsigned long long)const_val.enum_val.ordinal;
        hasOrdinal = true;
        value_is_unsigned = (const_val.enum_val.ordinal >= 0);
    } else if (isIntlikeType(const_val.type)) {
        sval = const_val.i_val;
        uval = const_val.u_val;
        hasOrdinal = true;
        value_is_unsigned = isUnsignedIntVarType(const_val.type) ||
                            const_val.type == TYPE_BOOLEAN ||
                            const_val.type == TYPE_CHAR;
    }

    if (!hasOrdinal) {
        freeValue(&const_val);
        return false;
    }

    switch (targetType) {
        case TYPE_BOOLEAN:
            fits = (sval == 0 || sval == 1);
            break;
        case TYPE_CHAR:
            fits = (sval >= 0 && uval <= UCHAR_MAX);
            break;
        case TYPE_INT8:
            fits = (sval >= INT8_MIN && sval <= INT8_MAX);
            break;
        case TYPE_UINT8:
        case TYPE_BYTE:
            fits = ((value_is_unsigned || sval >= 0) && uval <= UINT8_MAX);
            break;
        case TYPE_INT16:
            fits = (sval >= INT16_MIN && sval <= INT16_MAX);
            break;
        case TYPE_UINT16:
        case TYPE_WORD:
            fits = ((value_is_unsigned || sval >= 0) && uval <= UINT16_MAX);
            break;
        case TYPE_INT32:
            fits = (sval >= INT32_MIN && sval <= INT32_MAX);
            break;
        case TYPE_UINT32:
            fits = ((value_is_unsigned || sval >= 0) && uval <= UINT32_MAX);
            break;
        case TYPE_INT64:
            if (value_is_unsigned && uval > (unsigned long long)LLONG_MAX) {
                fits = false;
            } else {
                fits = (sval >= LLONG_MIN && sval <= LLONG_MAX);
            }
            break;
        case TYPE_UINT64:
            fits = (value_is_unsigned || sval >= 0);
            break;
        default:
            fits = false;
            break;
    }

    freeValue(&const_val);
    return fits;
}

static bool emitImmediateConstant(BytecodeChunk* chunk, int constant_index, int line) {
    if (!chunk || constant_index < 0) {
        return false;
    }
    if (constant_index >= chunk->constants_count) {
        return false;
    }
    Value* value = &chunk->constants[constant_index];
    switch (value->type) {
        case TYPE_INTEGER: {
            long long iv = value->i_val;
            if (iv == 0) {
                writeBytecodeChunk(chunk, CONST_0, line);
                return true;
            }
            if (iv == 1) {
                writeBytecodeChunk(chunk, CONST_1, line);
                return true;
            }
            if (iv >= INT8_MIN && iv <= INT8_MAX) {
                writeBytecodeChunk(chunk, PUSH_IMMEDIATE_INT8, line);
                int8_t imm = (int8_t)iv;
                writeBytecodeChunk(chunk, (uint8_t)imm, line);
                return true;
            }
            break;
        }
        case TYPE_BOOLEAN:
            writeBytecodeChunk(chunk, value->i_val ? CONST_TRUE : CONST_FALSE, line);
            return true;
        default:
            break;
    }
    return false;
}

static void emitConstant(BytecodeChunk* chunk, int constant_index, int line) {
    if (constant_index < 0) {
        fprintf(stderr, "L%d: Compiler error: negative constant index.\n", line);
        compiler_had_error = true;
        return;
    }
    if (emitImmediateConstant(chunk, constant_index, line)) {
        return;
    }
    if (constant_index <= 0xFF) {
        writeBytecodeChunk(chunk, CONSTANT, line);
        writeBytecodeChunk(chunk, (uint8_t)constant_index, line);
    } else if (constant_index <= 0xFFFF) {
        writeBytecodeChunk(chunk, CONSTANT16, line);
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
    bool needs_inline_cache = (op8 == GET_GLOBAL || op8 == SET_GLOBAL);
    if (name_idx <= 0xFF) {
        writeBytecodeChunk(chunk, op8, line);
        writeBytecodeChunk(chunk, (uint8_t)name_idx, line);
        if (needs_inline_cache) {
            writeInlineCacheSlot(chunk, line);
        }
    } else if (name_idx <= 0xFFFF) {
        writeBytecodeChunk(chunk, op16, line);
        emitShort(chunk, (uint16_t)name_idx, line);
        if (needs_inline_cache) {
            writeInlineCacheSlot(chunk, line);
        }
    } else {
        fprintf(stderr, "L%d: Compiler error: too many constants (%d). Limit is 65535.\n",
                line, name_idx);
        compiler_had_error = true;
    }
}

// Helper to emit DEFINE_GLOBAL or DEFINE_GLOBAL16 depending on index size.
static void emitDefineGlobal(BytecodeChunk* chunk, int name_idx, int line) {
    emitGlobalNameIdx(chunk, DEFINE_GLOBAL, DEFINE_GLOBAL16, name_idx, line);
}

// Resolve type references to their concrete definitions.
static AST* resolveTypeAlias(AST* type_node) {
    while (type_node &&
           (type_node->type == AST_TYPE_REFERENCE || type_node->type == AST_VARIABLE) &&
           type_node->token && type_node->token->value) {
        AST* looked = lookupType(type_node->token->value);
        if (!looked || looked == type_node) break;
        type_node = looked;
    }
    return type_node;
}

static AST* resolveProcPointerSignature(AST* type_node) {
    type_node = resolveTypeAlias(type_node);
    if (!type_node) {
        return NULL;
    }
    if (type_node->type == AST_VAR_DECL) {
        if (type_node->right) {
            type_node = type_node->right;
        } else if (type_node->type_def) {
            type_node = type_node->type_def;
        }
        type_node = resolveTypeAlias(type_node);
    }
    if (!type_node) {
        return NULL;
    }
    if (type_node->type == AST_PROC_PTR_TYPE) {
        return type_node;
    }
    if (type_node->type == AST_POINTER_TYPE && type_node->right) {
        AST* inner = resolveTypeAlias(type_node->right);
        if (inner && inner->type == AST_PROC_PTR_TYPE) {
            return inner;
        }
    }
    return NULL;
}

static AST* findProcPointerSignatureForCall(AST* call) {
    if (!call) {
        return NULL;
    }

    AST* candidate = call->type_def;
    if (!candidate && call->left) {
        candidate = call->left->type_def;
        if (!candidate && call->left->right) {
            candidate = call->left->right;
        }
    }

    if (!candidate && call->token && call->token->value) {
        Symbol* sym = lookupLocalSymbol(call->token->value);
        if (!sym) {
            sym = lookupGlobalSymbol(call->token->value);
        }
        sym = resolveSymbolAlias(sym);
        if (sym && sym->type_def) {
            candidate = sym->type_def;
        }
    }

    if (candidate && candidate->type == AST_VAR_DECL) {
        if (candidate->right) {
            candidate = candidate->right;
        } else if (candidate->type_def) {
            candidate = candidate->type_def;
        }
    }

    if (!candidate && call->token && call->token->value && gCurrentProgramRoot) {
        AST* decl = findStaticDeclarationInAST(call->token->value, call, gCurrentProgramRoot);
        if (decl) {
            if (decl->type == AST_VAR_DECL) {
                if (decl->right) {
                    candidate = decl->right;
                } else if (decl->type_def) {
                    candidate = decl->type_def;
                }
            } else if (decl->type == AST_CONST_DECL && decl->right) {
                candidate = decl->right;
            }
        }
    }

    return resolveProcPointerSignature(candidate);
}

static AST* getInterfaceTypeFromExpression(AST* expr) {
    if (!expr) {
        return NULL;
    }

    AST* type_node = resolveTypeAlias(expr->type_def);
    if (!type_node && expr->type == AST_VARIABLE && expr->token && expr->token->value) {
        Symbol* sym = lookupSymbolOptional(expr->token->value);
        if (sym && sym->type_def) {
            type_node = resolveTypeAlias(sym->type_def);
        }
    }

    if (!type_node && current_function_compiler && current_function_compiler->returns_value &&
        expr->type == AST_VARIABLE && expr->token && expr->token->value) {
        const char* varName = expr->token->value;
        bool isFunctionResult =
            isCurrentFunctionResultIdentifier(current_function_compiler, varName);
        if (isFunctionResult && current_function_compiler->function_symbol &&
            current_function_compiler->function_symbol->type_def &&
            current_function_compiler->function_symbol->type_def->type == AST_FUNCTION_DECL) {
            AST* returnDecl = current_function_compiler->function_symbol->type_def->right;
            AST* resolvedReturn = resolveInterfaceAST(returnDecl);
            if (resolvedReturn) {
                type_node = resolvedReturn;
            }
        }
    }

    if (type_node && type_node->var_type == TYPE_POINTER && type_node->right) {
        AST* pointed = resolveTypeAlias(type_node->right);
        if (pointed) {
            type_node = pointed;
        }
    }

    if (type_node && type_node->var_type == TYPE_INTERFACE) {
        return type_node;
    }

    return NULL;
}

static const char* getTypeNameFromAST(AST* typeAst);
static bool compareTypeNodes(AST* a, AST* b);

static AST* resolveInterfaceAST(AST* typeNode) {
    typeNode = resolveTypeAlias(typeNode);
    if (!typeNode) return NULL;

    if (typeNode->type == AST_TYPE_DECL && typeNode->left) {
        typeNode = resolveTypeAlias(typeNode->left);
    }

    if (typeNode && typeNode->type == AST_INTERFACE) {
        return typeNode;
    }

    return NULL;
}

static AST* resolveRecordAST(AST* typeNode) {
    typeNode = resolveTypeAlias(typeNode);
    if (!typeNode) return NULL;

    if (typeNode->type == AST_TYPE_DECL && typeNode->left) {
        typeNode = resolveTypeAlias(typeNode->left);
    }

    if (typeNode && typeNode->type == AST_RECORD_TYPE) {
        return typeNode;
    }

    return NULL;
}

static AST* findRecordMethodInHierarchy(AST* recordType, const char* methodName) {
    recordType = resolveRecordAST(recordType);
    if (!recordType || !methodName) {
        return NULL;
    }

    for (int i = 0; i < recordType->child_count; i++) {
        AST* child = recordType->children[i];
        if (!child) continue;
        if ((child->type == AST_PROCEDURE_DECL || child->type == AST_FUNCTION_DECL) &&
            child->token && child->token->value &&
            strcasecmp(child->token->value, methodName) == 0) {
            return child;
        }
    }

    AST* parentRef = recordType->extra;
    if (!parentRef) {
        return NULL;
    }

    AST* parent = resolveRecordAST(parentRef);
    if (!parent && parentRef->token && parentRef->token->value) {
        parent = resolveRecordAST(lookupType(parentRef->token->value));
    }
    if (!parent && parentRef->type == AST_TYPE_REFERENCE && parentRef->token && parentRef->token->value) {
        parent = resolveRecordAST(lookupType(parentRef->token->value));
    }
    if (!parent && parentRef->type == AST_VARIABLE && parentRef->token && parentRef->token->value) {
        parent = resolveRecordAST(lookupType(parentRef->token->value));
    }

    if (parent && parent != recordType) {
        return findRecordMethodInHierarchy(parent, methodName);
    }

    return NULL;
}

static bool addInterfaceMethod(AST*** methods, int* count, int* capacity, AST* method) {
    if (!methods || !count || !capacity || !method) {
        return false;
    }

    if (!method->token || !method->token->value) {
        return true;
    }

    for (int i = 0; i < *count; i++) {
        AST* existing = (*methods)[i];
        if (!existing || !existing->token || !existing->token->value) {
            continue;
        }
        if (strcasecmp(existing->token->value, method->token->value) == 0) {
            (*methods)[i] = method;
            return true;
        }
    }

    if (*count >= *capacity) {
        int new_capacity = (*capacity < 4) ? 4 : (*capacity * 2);
        AST** resized = realloc(*methods, (size_t)new_capacity * sizeof(AST*));
        if (!resized) {
            fprintf(stderr, "Compiler error: Out of memory gathering interface methods.\n");
            compiler_had_error = true;
            return false;
        }
        *methods = resized;
        *capacity = new_capacity;
    }

    (*methods)[(*count)++] = method;
    return true;
}

static bool collectInterfaceMethods(AST* interfaceType, AST*** methods, int* count, int* capacity, int depth) {
    if (depth > 32) {
        fprintf(stderr, "Compiler error: Interface inheritance chain too deep.\n");
        compiler_had_error = true;
        return false;
    }

    interfaceType = resolveInterfaceAST(interfaceType);
    if (!interfaceType) {
        return true;
    }

    if (interfaceType->extra) {
        AST* baseList = interfaceType->extra;
        if (baseList->type == AST_LIST) {
            for (int i = 0; i < baseList->child_count; i++) {
                if (!collectInterfaceMethods(baseList->children[i], methods, count, capacity, depth + 1)) {
                    return false;
                }
            }
        } else {
            if (!collectInterfaceMethods(baseList, methods, count, capacity, depth + 1)) {
                return false;
            }
        }
    }

    for (int i = 0; i < interfaceType->child_count; i++) {
        AST* child = interfaceType->children[i];
        if (!child) continue;
        if (child->type == AST_PROCEDURE_DECL || child->type == AST_FUNCTION_DECL) {
            if (!addInterfaceMethod(methods, count, capacity, child)) {
                return false;
            }
        }
    }

    return true;
}

static const char* getReadableTypeName(AST* typeAst) {
    const char* name = getTypeNameFromAST(typeAst);
    if (name) {
        return name;
    }
    if (typeAst && typeAst->token && typeAst->token->value) {
        return typeAst->token->value;
    }
    return "<anonymous>";
}

typedef struct {
    AST* group;
    AST* identifier;
} MethodParameter;

static bool buildMethodParameterList(AST* method, MethodParameter** outParams, int* outCount) {
    if (!outParams || !outCount) {
        return false;
    }

    *outParams = NULL;
    *outCount = 0;

    if (!method) {
        return true;
    }

    int total = 0;
    for (int i = 0; i < method->child_count; i++) {
        AST* paramGroup = method->children[i];
        if (!paramGroup) continue;

        if (paramGroup->type == AST_VAR_DECL) {
            if (paramGroup->child_count > 0) {
                total += paramGroup->child_count;
            } else {
                total++;
            }
        } else {
            total++;
        }
    }

    if (total == 0) {
        return true;
    }

    MethodParameter* params = calloc((size_t)total, sizeof(MethodParameter));
    if (!params) {
        fprintf(stderr, "Compiler error: Out of memory while validating method parameters.\n");
        compiler_had_error = true;
        return false;
    }

    int index = 0;
    for (int i = 0; i < method->child_count; i++) {
        AST* paramGroup = method->children[i];
        if (!paramGroup) continue;

        if (paramGroup->type == AST_VAR_DECL) {
            if (paramGroup->child_count == 0) {
                params[index].group = paramGroup;
                params[index].identifier = NULL;
                index++;
            } else {
                for (int j = 0; j < paramGroup->child_count && index < total; j++) {
                    params[index].group = paramGroup;
                    params[index].identifier = paramGroup->children[j];
                    index++;
                }
            }
        } else {
            params[index].group = paramGroup;
            params[index].identifier = NULL;
            index++;
        }
    }

    *outParams = params;
    *outCount = index;
    return true;
}

static bool compareMethodSignatures(AST* ifaceMethod, AST* recordMethod,
                                    const char* recordName, const char* interfaceName,
                                    int line) {
    if (!ifaceMethod || !recordMethod) {
        return false;
    }

    if (ifaceMethod->type != recordMethod->type) {
        const char* kind_iface = (ifaceMethod->type == AST_FUNCTION_DECL) ? "function" : "procedure";
        const char* kind_record = (recordMethod->type == AST_FUNCTION_DECL) ? "function" : "procedure";
        fprintf(stderr,
                "L%d: Compiler Error: Method '%s' on record '%s' must be a %s to satisfy interface '%s' (found %s).\n",
                line,
                ifaceMethod->token && ifaceMethod->token->value ? ifaceMethod->token->value : "<anonymous>",
                recordName,
                kind_iface,
                interfaceName,
                kind_record);
        compiler_had_error = true;
        return false;
    }

    MethodParameter* ifaceParams = NULL;
    MethodParameter* recordParams = NULL;
    int ifaceParamCount = 0;
    int recordParamCount = 0;

    if (!buildMethodParameterList(ifaceMethod, &ifaceParams, &ifaceParamCount) ||
        !buildMethodParameterList(recordMethod, &recordParams, &recordParamCount)) {
        free(ifaceParams);
        free(recordParams);
        return false;
    }

    if (ifaceParamCount != recordParamCount) {
        fprintf(stderr,
                "L%d: Compiler Error: Method '%s' on record '%s' must take %d parameter(s) to satisfy interface '%s' (found %d).\n",
                line,
                ifaceMethod->token && ifaceMethod->token->value ? ifaceMethod->token->value : "<anonymous>",
                recordName,
                ifaceParamCount,
                interfaceName,
                recordParamCount);
        compiler_had_error = true;
        free(ifaceParams);
        free(recordParams);
        return false;
    }

    bool success = true;
    for (int i = 0; success && i < ifaceParamCount; i++) {
        AST* ifaceGroup = ifaceParams[i].group;
        AST* recordGroup = recordParams[i].group;

        if (!ifaceGroup || !recordGroup) {
            fprintf(stderr,
                    "L%d: Compiler Error: Internal error validating parameter %d for method '%s'.\n",
                    line,
                    i + 1,
                    ifaceMethod->token && ifaceMethod->token->value ? ifaceMethod->token->value : "<anonymous>");
            compiler_had_error = true;
            success = false;
            break;
        }

        if ((ifaceGroup->by_ref ? 1 : 0) != (recordGroup->by_ref ? 1 : 0)) {
            fprintf(stderr,
                    "L%d: Compiler Error: Parameter %d of method '%s' on record '%s' must %sbe VAR to satisfy interface '%s'.\n",
                    line,
                    i + 1,
                    ifaceMethod->token && ifaceMethod->token->value ? ifaceMethod->token->value : "<anonymous>",
                    recordName,
                    ifaceGroup->by_ref ? "" : "not ",
                    interfaceName);
            compiler_had_error = true;
            success = false;
            break;
        }

        AST* ifaceType = ifaceGroup->type_def ? ifaceGroup->type_def : ifaceGroup->right;
        AST* recordType = recordGroup->type_def ? recordGroup->type_def : recordGroup->right;
        if (!compareTypeNodes(ifaceType, recordType)) {
            const char* expected = ifaceType ? varTypeToString(ifaceType->var_type) : "UNKNOWN";
            const char* got = recordType ? varTypeToString(recordType->var_type) : "UNKNOWN";
            fprintf(stderr,
                    "L%d: Compiler Error: Parameter %d of method '%s' on record '%s' must be type %s to satisfy interface '%s' (found %s).\n",
                    line,
                    i + 1,
                    ifaceMethod->token && ifaceMethod->token->value ? ifaceMethod->token->value : "<anonymous>",
                    recordName,
                    expected,
                    interfaceName,
                    got);
            compiler_had_error = true;
            success = false;
            break;
        }
    }

    if (success && ifaceMethod->type == AST_FUNCTION_DECL) {
        AST* ifaceReturn = ifaceMethod->right;
        AST* recordReturn = recordMethod->right;
        if (!compareTypeNodes(ifaceReturn, recordReturn)) {
            const char* expected = ifaceReturn ? varTypeToString(ifaceReturn->var_type) : "UNKNOWN";
            const char* got = recordReturn ? varTypeToString(recordReturn->var_type) : "UNKNOWN";
            fprintf(stderr,
                    "L%d: Compiler Error: Function '%s' on record '%s' must return %s to satisfy interface '%s' (found %s).\n",
                    line,
                    ifaceMethod->token && ifaceMethod->token->value ? ifaceMethod->token->value : "<anonymous>",
                    recordName,
                    expected,
                    interfaceName,
                    got);
            compiler_had_error = true;
            success = false;
        }
    }

    free(ifaceParams);
    free(recordParams);
    return success;
}

static void propagateMethodSlotToSymbol(AST* recordType, AST* methodNode, int slot) {
    if (!recordType || !methodNode || slot < 0 || !methodNode->token || !methodNode->token->value) {
        return;
    }

    const char* recordName = getReadableTypeName(recordType);
    const char* methodName = methodNode->token->value;
    char qualified[MAX_SYMBOL_LENGTH * 2 + 2];
    snprintf(qualified, sizeof(qualified), "%s.%s", recordName, methodName);

    char lowered[MAX_SYMBOL_LENGTH * 2 + 2];
    strncpy(lowered, qualified, sizeof(lowered) - 1);
    lowered[sizeof(lowered) - 1] = '\0';
    toLowerString(lowered);

    Symbol* sym = lookupProcedure(lowered);
    if (sym && sym->is_alias && sym->real_symbol) {
        sym = sym->real_symbol;
    }
    if (sym && sym->type_def) {
        sym->type_def->i_val = slot;
    }
}

typedef struct {
    AST* interfaceMethod;
    AST* recordMethod;
} InterfaceMethodMatch;

static bool validateInterfaceImplementation(AST* recordType, AST* interfaceType, int line) {
    interfaceType = resolveInterfaceAST(interfaceType);
    recordType = resolveRecordAST(recordType);

    if (!interfaceType || !recordType) {
        fprintf(stderr, "L%d: Compiler Error: Invalid interface or record type in cast.\n", line);
        compiler_had_error = true;
        return false;
    }

    AST** ifaceMethods = NULL;
    int methodCount = 0;
    int methodCapacity = 0;
    if (!collectInterfaceMethods(interfaceType, &ifaceMethods, &methodCount, &methodCapacity, 0)) {
        free(ifaceMethods);
        return false;
    }

    if (compiler_had_error) {
        free(ifaceMethods);
        return false;
    }

    if (methodCount == 0) {
        interfaceType->i_val = 1;
        free(ifaceMethods);
        return true;
    }

    InterfaceMethodMatch* matches = calloc((size_t)methodCount, sizeof(InterfaceMethodMatch));
    if (!matches) {
        fprintf(stderr, "Compiler error: Out of memory validating interface methods.\n");
        compiler_had_error = true;
        free(ifaceMethods);
        return false;
    }

    const char* interfaceName = getReadableTypeName(interfaceType);
    const char* recordName = getReadableTypeName(recordType);

    bool success = true;
    for (int i = 0; success && i < methodCount; i++) {
        AST* ifaceMethod = ifaceMethods[i];
        const char* methodName = (ifaceMethod && ifaceMethod->token) ? ifaceMethod->token->value : NULL;
        if (!ifaceMethod || !methodName) {
            fprintf(stderr, "L%d: Compiler Error: Interface method missing name during validation.\n", line);
            compiler_had_error = true;
            success = false;
            break;
        }

        AST* recordMethod = findRecordMethodInHierarchy(recordType, methodName);
        if (!recordMethod) {
            fprintf(stderr,
                    "L%d: Compiler Error: Record '%s' is missing virtual method '%s' required by interface '%s'.\n",
                    line,
                    recordName,
                    methodName,
                    interfaceName);
            compiler_had_error = true;
            success = false;
            break;
        }

        if (!recordMethod->is_virtual) {
            fprintf(stderr,
                    "L%d: Compiler Error: Method '%s' on record '%s' must be declared virtual to satisfy interface '%s'.\n",
                    line,
                    methodName,
                    recordName,
                    interfaceName);
            compiler_had_error = true;
            success = false;
            break;
        }

        if (!compareMethodSignatures(ifaceMethod, recordMethod, recordName, interfaceName, line)) {
            success = false;
            break;
        }

        matches[i].interfaceMethod = ifaceMethod;
        matches[i].recordMethod = recordMethod;
    }

    if (success) {
        for (int i = 0; i < methodCount; i++) {
            AST* ifaceMethod = matches[i].interfaceMethod;
            AST* recordMethod = matches[i].recordMethod;
            ifaceMethod->i_val = i;
            if (ifaceMethod->parent && ifaceMethod->parent->type == AST_INTERFACE) {
                ifaceMethod->parent->i_val = 1;
            }
            if (recordMethod) {
                recordMethod->i_val = i;
                propagateMethodSlotToSymbol(recordType, recordMethod, i);
            }
        }
        interfaceType->i_val = 1;
    }

    free(matches);
    free(ifaceMethods);
    return success;
}

static void emitInterfaceBoxingCall(BytecodeChunk* chunk,
                                    AST* recordType,
                                    AST* interfaceType,
                                    const char* fallbackInterfaceName,
                                    int line) {
    if (!chunk || !recordType || !interfaceType) {
        return;
    }

    emitConstant(chunk, addNilConstant(chunk), line);
    writeBytecodeChunk(chunk, SWAP, line);

    const char* className = getTypeNameFromAST(recordType);
    if ((!className || className[0] == '\0') && recordType->token && recordType->token->value) {
        className = recordType->token->value;
    }
    if (!className) {
        className = "";
    }
    int classNameIndex = addStringConstant(chunk, className);
    emitConstant(chunk, classNameIndex, line);

    const char* ifaceName = getTypeNameFromAST(interfaceType);
    if ((!ifaceName || ifaceName[0] == '\0') && interfaceType->token && interfaceType->token->value) {
        ifaceName = interfaceType->token->value;
    }
    if ((!ifaceName || ifaceName[0] == '\0') && fallbackInterfaceName) {
        ifaceName = fallbackInterfaceName;
    }
    if (!ifaceName) {
        ifaceName = "";
    }
    int ifaceNameIndex = addStringConstant(chunk, ifaceName);
    emitConstant(chunk, ifaceNameIndex, line);

    writeBytecodeChunk(chunk, CALL_HOST, line);
    writeBytecodeChunk(chunk, (uint8_t)HOST_FN_BOX_INTERFACE, line);
}
static InterfaceBoxingResult autoBoxInterfaceValue(AST* interfaceAst,
                                                   AST* valueExpr,
                                                   BytecodeChunk* chunk,
                                                   int line,
                                                   const char* contextName,
                                                   bool recoverWithNil,
                                                   bool strictRecord) {
    if (!interfaceAst || !valueExpr || !chunk) {
        return INTERFACE_BOX_NOT_NEEDED;
    }

    if (valueExpr->var_type == TYPE_INTERFACE || getInterfaceTypeFromExpression(valueExpr)) {
        return INTERFACE_BOX_NOT_NEEDED;
    }

    AST* interfaceType = resolveInterfaceAST(interfaceAst);
    if (!interfaceType) {
        return INTERFACE_BOX_NOT_NEEDED;
    }

    AST* recordType = getRecordTypeFromExpr(valueExpr);
    recordType = resolveRecordAST(recordType);
    if ((!recordType || recordType->type != AST_RECORD_TYPE)) {
        if (strictRecord) {
            const char* ifaceName = getReadableTypeName(interfaceType);
            fprintf(stderr,
                    "L%d: Compiler Error: Expression cannot be converted to interface '%s'.\n",
                    line,
                    ifaceName);
            compiler_had_error = true;
            if (recoverWithNil) {
                writeBytecodeChunk(chunk, POP, line);
                emitConstant(chunk, addNilConstant(chunk), line);
            }
            return INTERFACE_BOX_FAILED;
        }
        return INTERFACE_BOX_NOT_NEEDED;
    }

    if (!recordTypeHasVTable(recordType)) {
        const char* ifaceName = getReadableTypeName(interfaceType);
        const char* recordName = getReadableTypeName(recordType);
        fprintf(stderr,
                "L%d: Compiler Error: Only class records with virtual methods can be assigned to interface '%s' (record '%s').\n",
                line,
                ifaceName,
                recordName);
        compiler_had_error = true;
        if (recoverWithNil) {
            writeBytecodeChunk(chunk, POP, line);
            emitConstant(chunk, addNilConstant(chunk), line);
        }
        return INTERFACE_BOX_FAILED;
    }

    if (!validateInterfaceImplementation(recordType, interfaceType, line)) {
        if (recoverWithNil) {
            writeBytecodeChunk(chunk, POP, line);
            emitConstant(chunk, addNilConstant(chunk), line);
        }
        return INTERFACE_BOX_FAILED;
    }

    emitInterfaceBoxingCall(chunk, recordType, interfaceType, contextName, line);
    return INTERFACE_BOX_DONE;
}

static InterfaceBoxingResult maybeAutoBoxInterfaceForExpression(AST* targetExpr,
                                                                AST* valueExpr,
                                                                BytecodeChunk* chunk,
                                                                int line,
                                                                bool recoverWithNil) {
    if (!targetExpr) {
        return INTERFACE_BOX_NOT_NEEDED;
    }
    AST* interfaceType = getInterfaceTypeFromExpression(targetExpr);
    if (!interfaceType) {
        return INTERFACE_BOX_NOT_NEEDED;
    }
    const char* fallback = getReadableTypeName(interfaceType);
    return autoBoxInterfaceValue(interfaceType, valueExpr, chunk, line, fallback, recoverWithNil, false);
}

static InterfaceBoxingResult maybeAutoBoxInterfaceForType(AST* interfaceType,
                                                          AST* valueExpr,
                                                          BytecodeChunk* chunk,
                                                          int line,
                                                          bool recoverWithNil,
                                                          bool strictRecord) {
    if (!interfaceType) {
        return INTERFACE_BOX_NOT_NEEDED;
    }
    AST* resolved = resolveInterfaceAST(interfaceType);
    if (!resolved) {
        return INTERFACE_BOX_NOT_NEEDED;
    }
    const char* fallback = getReadableTypeName(resolved);
    return autoBoxInterfaceValue(resolved, valueExpr, chunk, line, fallback, recoverWithNil, strictRecord);
}

static AST* getParameterTypeAST(AST* param_node) {
    if (!param_node) {
        return NULL;
    }
    if (param_node->type_def) {
        return param_node->type_def;
    }
    if (param_node->right) {
        return param_node->right;
    }
    AST* parent = param_node->parent;
    if (parent && parent->type == AST_VAR_DECL) {
        if (parent->type_def) {
            return parent->type_def;
        }
        if (parent->right) {
            return parent->right;
        }
        return parent;
    }
    return param_node;
}

static AST* getInterfaceASTForParam(AST* param_node, AST* param_type) {
    if (param_type) {
        AST* candidate = resolveInterfaceAST(param_type);
        if (candidate) {
            return candidate;
        }
    }

    if (param_node) {
        AST* candidate = resolveInterfaceAST(param_node);
        if (candidate) {
            return candidate;
        }
        if (param_node->type_def) {
            candidate = resolveInterfaceAST(param_node->type_def);
            if (candidate) {
                return candidate;
            }
        }
        if (param_node->right) {
            candidate = resolveInterfaceAST(param_node->right);
            if (candidate) {
                return candidate;
            }
        }
        AST* parent = param_node->parent;
        if (parent) {
            candidate = resolveInterfaceAST(parent);
            if (candidate) {
                return candidate;
            }
            if (parent->type_def) {
                candidate = resolveInterfaceAST(parent->type_def);
                if (candidate) {
                    return candidate;
                }
            }
            if (parent->right) {
                candidate = resolveInterfaceAST(parent->right);
                if (candidate) {
                    return candidate;
                }
            }
        }
    }
    return NULL;
}

static bool isInterfaceParameterNode(AST* param_node, AST* param_type) {
    if ((param_type && param_type->var_type == TYPE_INTERFACE) ||
        (param_node && param_node->var_type == TYPE_INTERFACE) ||
        (param_node && param_node->parent && param_node->parent->var_type == TYPE_INTERFACE)) {
        return true;
    }
    return getInterfaceASTForParam(param_node, param_type) != NULL;
}

static int ensureInterfaceMethodSlot(AST* interfaceType, const char* methodName) {
    interfaceType = resolveInterfaceAST(interfaceType);
    if (!interfaceType || interfaceType->var_type != TYPE_INTERFACE || !methodName) {
        return -1;
    }

    if (interfaceType->i_val == 0) {
        return -1;
    }

    for (int i = 0; i < interfaceType->child_count; i++) {
        AST* child = interfaceType->children[i];
        if (!child) continue;
        if ((child->type == AST_PROCEDURE_DECL || child->type == AST_FUNCTION_DECL) &&
            child->token && child->token->value &&
            strcasecmp(child->token->value, methodName) == 0) {
            if (child->i_val >= 0) {
                return child->i_val;
            }
        }
    }

    if (interfaceType->extra) {
        AST* baseList = interfaceType->extra;
        if (baseList->type == AST_LIST) {
            for (int i = 0; i < baseList->child_count; i++) {
                int slot = ensureInterfaceMethodSlot(baseList->children[i], methodName);
                if (slot >= 0) {
                    return slot;
                }
            }
        } else {
            return ensureInterfaceMethodSlot(baseList, methodName);
        }
    }

    return -1;
}

// --- Object layout helpers -------------------------------------------------

// Recursively count fields in a record, including inherited ones.
static int getRecordFieldCount(AST* recordType) {
    recordType = resolveTypeAlias(recordType);
    if (!recordType || recordType->type != AST_RECORD_TYPE) return 0;

    int count = 0;
    for (int i = 0; i < recordType->child_count; i++) {
        AST* decl = recordType->children[i];
        if (!decl) continue;
        if (decl->type == AST_VAR_DECL) {
            count += decl->child_count; // each child is a field name
        } else if (decl->token && decl->type != AST_PROCEDURE_DECL && decl->type != AST_FUNCTION_DECL) {
            // Some passes may have flattened fields directly into the record.
            count++;
        }
    }

    if (recordType->extra && recordType->extra->token && recordType->extra->token->value) {
        AST* parent = lookupType(recordType->extra->token->value);
        count += getRecordFieldCount(parent);
    }
    return count;
}

// Retrieve zero-based offset of a field within a record hierarchy.
static int getRecordFieldOffset(AST* recordType, const char* fieldName) {
    recordType = resolveTypeAlias(recordType);
    if (!recordType || recordType->type != AST_RECORD_TYPE || !fieldName) return -1;

    int parentCount = 0;
    if (recordType->extra && recordType->extra->token && recordType->extra->token->value) {
        AST* parent = lookupType(recordType->extra->token->value);
        int parentOffset = getRecordFieldOffset(parent, fieldName);
        if (parentOffset != -1) return parentOffset;
        parentCount = getRecordFieldCount(parent);
    }

    int offset = parentCount;
    for (int i = 0; i < recordType->child_count; i++) {
        AST* decl = recordType->children[i];
        if (!decl) continue;
        if (decl->type == AST_VAR_DECL) {
            for (int j = 0; j < decl->child_count; j++) {
                AST* var = decl->children[j];
                if (var && var->token && strcmp(var->token->value, fieldName) == 0) {
                    return offset;
                }
                offset++;
            }
        } else if (decl->token && decl->type != AST_PROCEDURE_DECL && decl->type != AST_FUNCTION_DECL) {
            if (strcmp(decl->token->value, fieldName) == 0) {
                return offset;
            }
            offset++;
        }
    }
    return -1;
}

// Find a record type in the global type table that defines the given field.
static AST* findRecordTypeByFieldName(const char* fieldName) {
    if (!fieldName) return NULL;
    for (TypeEntry* entry = type_table; entry; entry = entry->next) {
        AST* rec = resolveTypeAlias(entry->typeAST);
        if (!rec || rec->type != AST_RECORD_TYPE) continue;
        int offset = getRecordFieldOffset(rec, fieldName);
        if (offset >= 0) return rec;
        // Case-insensitive scan
        for (int i = 0; i < rec->child_count; i++) {
            AST* decl = rec->children[i];
            if (!decl) continue;
            if (decl->type == AST_VAR_DECL) {
                for (int j = 0; j < decl->child_count; j++) {
                    AST* var = decl->children[j];
                    if (var && var->token && strcasecmp(var->token->value, fieldName) == 0) {
                        return rec;
                    }
                }
            } else if (decl->token && decl->type != AST_PROCEDURE_DECL && decl->type != AST_FUNCTION_DECL) {
                if (strcasecmp(decl->token->value, fieldName) == 0) return rec;
            }
        }
    }
    return NULL;
}

// Emit initialization code for array fields within a record/class.
// Assumes the object instance is on top of the VM stack.
static void emitArrayFieldInitializers(AST* recordType, BytecodeChunk* chunk, int line, bool hasVTable) {
    recordType = resolveTypeAlias(recordType);
    if (!recordType || recordType->type != AST_RECORD_TYPE) return;

    if (recordType->extra && recordType->extra->token && recordType->extra->token->value) {
        AST* parent = lookupType(recordType->extra->token->value);
        emitArrayFieldInitializers(parent, chunk, line, recordTypeHasVTable(parent));
    }

    for (int i = 0; i < recordType->child_count; i++) {
        AST* decl = recordType->children[i];
        if (!decl || decl->type != AST_VAR_DECL) continue;

        AST* type_node = decl->right;
        AST* actual_type = type_node;
        if (actual_type && actual_type->type == AST_TYPE_REFERENCE) {
            AST* resolved = lookupType(actual_type->token->value);
            if (resolved) actual_type = resolved;
        }

        if (actual_type && actual_type->type == AST_ARRAY_TYPE) {
            int dim_count = actual_type->child_count;
            for (int j = 0; j < decl->child_count; j++) {
                AST* varNode = decl->children[j];
                if (!varNode || !varNode->token) continue;
                int offset = getRecordFieldOffset(recordType, varNode->token->value);
                if (hasVTable) offset++;
                if (offset < 0) continue;
                writeBytecodeChunk(chunk, INIT_FIELD_ARRAY, line);
                writeBytecodeChunk(chunk, (uint8_t)offset, line);
                writeBytecodeChunk(chunk, (uint8_t)dim_count, line);
                for (int d = 0; d < dim_count; d++) {
                    AST* sub = actual_type->children[d];
                    if (sub && sub->type == AST_SUBRANGE) {
                        Value low_v = evaluateCompileTimeValue(sub->left);
                        Value high_v = evaluateCompileTimeValue(sub->right);
                        int lb = isIntlikeType(low_v.type) ? (int)AS_INTEGER(low_v) : 0;
                        int ub = isIntlikeType(high_v.type) ? (int)AS_INTEGER(high_v) : -1;
                        emitConstantIndex16(chunk, addIntConstant(chunk, lb), line);
                        emitConstantIndex16(chunk, addIntConstant(chunk, ub), line);
                        freeValue(&low_v); freeValue(&high_v);
                    } else {
                        emitShort(chunk, 0, line);
                        emitShort(chunk, 0, line);
                    }
                }
                AST* elem_type = actual_type->right;
                VarType elem_var_type = elem_type->var_type;
                writeBytecodeChunk(chunk, (uint8_t)elem_var_type, line);
                const char* elem_name = (elem_type && elem_type->token) ? elem_type->token->value : "";
                emitConstantIndex16(chunk, addStringConstant(chunk, elem_name), line);
            }
        }
    }
}

// Determine the record type for an expression used as an object base.
static AST* getRecordTypeFromExpr(AST* expr) {
    if (!expr) return NULL;
    if (expr->type == AST_VARIABLE && expr->token && expr->token->value &&
        (strcasecmp(expr->token->value, "myself") == 0 ||
         strcasecmp(expr->token->value, "my") == 0) &&
        current_class_record_type && current_class_record_type->type == AST_RECORD_TYPE) {
        return current_class_record_type;
    }
    if (expr->type == AST_ARRAY_ACCESS) {
        AST* baseType = getRecordTypeFromExpr(expr->left);
        if (!baseType) return NULL;
        baseType = resolveTypeAlias(baseType);
        if (baseType && baseType->type == AST_ARRAY_TYPE) {
            AST* elem = resolveTypeAlias(baseType->right);
            if (elem && elem->type == AST_POINTER_TYPE) {
                return resolveTypeAlias(elem->right);
            }
            return elem;
        }
        if (baseType && baseType->type == AST_POINTER_TYPE) {
            AST* arr = resolveTypeAlias(baseType->right);
            if (arr && arr->type == AST_ARRAY_TYPE) {
                AST* elem = resolveTypeAlias(arr->right);
                if (elem && elem->type == AST_POINTER_TYPE) {
                    return resolveTypeAlias(elem->right);
                }
                return elem;
            }
        }
        return NULL;
    }
    if (expr->type == AST_DEREFERENCE) {
        AST* ptr_type = resolveTypeAlias(expr->left->type_def);
        if (ptr_type && ptr_type->type == AST_POINTER_TYPE) {
            return resolveTypeAlias(ptr_type->right);
        }
        return NULL;
    }
    AST* t = resolveTypeAlias(expr->type_def);
    if (!t && expr->token && expr->token->value && gCurrentProgramRoot) {
        AST* decl = findStaticDeclarationInAST(expr->token->value, expr, gCurrentProgramRoot);
        if (decl && decl->right) {
            t = resolveTypeAlias(decl->right);
        } else if (current_function_compiler && current_function_compiler->function_symbol &&
                   current_function_compiler->function_symbol->name) {
            const char* fname = current_function_compiler->function_symbol->name;
            const char* dot = strchr(fname, '.');
            if (dot) {
                size_t len = (size_t)(dot - fname);
                char cls[MAX_SYMBOL_LENGTH];
                if (len >= sizeof(cls)) len = sizeof(cls) - 1;
                memcpy(cls, fname, len);
                cls[len] = '\0';
                AST* classType = lookupType(cls);
                classType = resolveTypeAlias(classType);
                if (classType && classType->type == AST_RECORD_TYPE) {
                    for (int i = 0; i < classType->child_count && !t; i++) {
                        AST* f = classType->children[i];
                        if (!f || f->type != AST_VAR_DECL) continue;
                        for (int j = 0; j < f->child_count; j++) {
                            AST* v = f->children[j];
                            if (v && v->token && strcmp(v->token->value, expr->token->value) == 0) {
                                if (f->right) t = resolveTypeAlias(f->right);
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    if (t && t->type == AST_POINTER_TYPE) {
        return resolveTypeAlias(t->right);
    }
    return t;
}

// Find the canonical name for a type AST node.
static const char* getTypeNameFromAST(AST* typeAst) {
    for (TypeEntry* entry = type_table; entry; entry = entry->next) {
        if (entry->typeAST == typeAst) return entry->name;
    }
    return NULL;
}

// Check if a record type defines methods and therefore reserves a vtable slot.
static bool recordTypeHasVTable(AST* recordType) {
    recordType = resolveTypeAlias(recordType);
    if (!recordType) return false;

    if (recordType->type == AST_TYPE_DECL && recordType->left) {
        recordType = resolveTypeAlias(recordType->left);
    }

    if (!recordType || recordType->type != AST_RECORD_TYPE) return false;

    if (!ensureProcedureTableInitialized()) {
        return false;
    }

    for (int i = 0; i < recordType->child_count; i++) {
        AST* member = recordType->children[i];
        if (!member) continue;
        if ((member->type == AST_PROCEDURE_DECL || member->type == AST_FUNCTION_DECL) &&
            member->is_virtual) {
            return true;
        }
    }

    const char* name = getTypeNameFromAST(recordType);
    if (!name) return false;
    size_t len = strlen(name);
    for (int b = 0; b < HASHTABLE_SIZE; b++) {
        Symbol* sym = procedure_table->buckets[b];
        while (sym) {
            Symbol* base = sym->is_alias ? sym->real_symbol : sym;
            if (base && base->name && base->type_def && base->type_def->is_virtual &&
                strncasecmp(base->name, name, len) == 0 && base->name[len] == '.') {
                AST* func = base->type_def;
                if (func->child_count > 0) {
                    AST* firstParam = func->children[0];
                    AST* paramType = resolveTypeAlias(firstParam ? firstParam->right : NULL);
                    if (paramType && paramType->type == AST_POINTER_TYPE) {
                        AST* target = resolveTypeAlias(paramType->right);
                        const char* targetName = getTypeNameFromAST(target);
                        if (targetName && strcasecmp(targetName, name) == 0) {
                            return true;
                        }
                    }
                }
            }
            sym = sym->next;
        }
    }
    return false;
}

static bool emitImplicitMyselfFieldValue(BytecodeChunk* chunk, int line, const char* fieldName) {
    if (!chunk || !fieldName || !current_class_record_type) {
        return false;
    }

    AST* recordType = resolveTypeAlias(current_class_record_type);
    if (!recordType || recordType->type != AST_RECORD_TYPE) {
        return false;
    }

    int fieldOffset = getRecordFieldOffset(recordType, fieldName);
    if (fieldOffset < 0) {
        return false;
    }
    if (recordTypeHasVTable(recordType)) {
        fieldOffset++;
    }

    int myself_idx = ensureMyselfGlobalNameIndex(chunk);
    emitGlobalNameIdx(chunk, GET_GLOBAL, GET_GLOBAL16, myself_idx, line);
    if (fieldOffset <= UINT8_MAX) {
        writeBytecodeChunk(chunk, LOAD_FIELD_VALUE, line);
        writeBytecodeChunk(chunk, (uint8_t)fieldOffset, line);
    } else {
        writeBytecodeChunk(chunk, LOAD_FIELD_VALUE16, line);
        emitShort(chunk, (uint16_t)fieldOffset, line);
    }
    return true;
}

// Compare two type AST nodes structurally.
static bool compareTypeNodes(AST* a, AST* b) {
    a = resolveTypeAlias(a);
    b = resolveTypeAlias(b);
    if (!a || !b) return a == b;
    if (a == b) return true;
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

// Return true if `sub` record type inherits from `base` record type.
static bool isSubclassOf(AST* sub, AST* base) {
    sub = resolveTypeAlias(sub);
    base = resolveTypeAlias(base);
    while (sub) {
        if (compareTypeNodes(sub, base)) return true;
        if (sub->extra) {
            sub = resolveTypeAlias(sub->extra);
        } else {
            break;
        }
    }
    return false;
}

// Determine if an argument node's type matches the full parameter type node.
//
// Both sides may reference type aliases, so we resolve them before comparison.
// `arg_node->type_def` provides the full type of the argument (including any
// array structure) which allows for structural comparisons, especially when
// checking VAR parameters that are themselves arrays.
static bool typesMatch(AST* param_type, AST* arg_node, bool allow_coercion) {
    if (!param_type || !arg_node) return false;

    if (param_type->var_type == TYPE_INTERFACE) {
        return true;
    }

    AST* param_actual = resolveTypeAlias(param_type);
    if (!param_actual) return false;

    bool interfaceParam = false;
    AST* param_interface_candidate = param_actual;
    if ((!param_interface_candidate || param_interface_candidate->type == AST_VAR_DECL) &&
        param_type->type == AST_VAR_DECL && param_type->right) {
        param_interface_candidate = resolveTypeAlias(param_type->right);
    }
    if (resolveInterfaceAST(param_interface_candidate)) {
        interfaceParam = true;
    } else if (param_type->var_type == TYPE_INTERFACE) {
        interfaceParam = true;
    } else if (param_type->type == AST_TYPE_REFERENCE) {
        AST* alias = resolveTypeAlias(param_type);
        if (resolveInterfaceAST(alias)) {
            interfaceParam = true;
        }
    }
    if (interfaceParam) {
        if (arg_node->var_type == TYPE_INTERFACE) {
            return true;
        }
        if (arg_node->var_type == TYPE_RECORD) {
            return true;
        }
        AST* recordType = getRecordTypeFromExpr(arg_node);
        if (recordType && resolveRecordAST(recordType)) {
            return true;
        }
    }

    // Resolve the argument's actual type as well.  The argument node carries a
    // full type definition in `type_def`, which may itself be a type alias.
    AST* arg_actual = resolveTypeAlias(arg_node->type_def);
    VarType arg_vt = arg_actual ? arg_actual->var_type : arg_node->var_type;

    if (arg_node->type == AST_PROCEDURE_CALL && arg_node->token && arg_node->token->value) {
        const char* callee = arg_node->token->value;
        if (strcasecmp(callee, "low") == 0 || strcasecmp(callee, "high") == 0) {
            AST* value_node = (arg_node->child_count > 0) ? arg_node->children[0] : NULL;
            AST* value_type = value_node ? resolveTypeAlias(value_node->type_def) : NULL;
            VarType source_vt = value_type ? value_type->var_type
                                           : (value_node ? value_node->var_type : TYPE_UNKNOWN);

            if (source_vt == TYPE_POINTER && value_type && value_type->right) {
                AST* pointed = resolveTypeAlias(value_type->right);
                if (pointed && pointed->var_type == TYPE_ARRAY) {
                    source_vt = TYPE_ARRAY;
                }
            }

            if (source_vt == TYPE_ENUM) {
                if (value_type) {
                    arg_actual = value_type;
                }
                arg_vt = TYPE_ENUM;
            } else if (source_vt == TYPE_ARRAY || source_vt == TYPE_STRING ||
                       source_vt == TYPE_UNKNOWN || source_vt == TYPE_VOID) {
                arg_vt = TYPE_INTEGER;
            } else {
                arg_vt = source_vt;
            }

            if (arg_node->var_type == TYPE_UNKNOWN || arg_node->var_type == TYPE_ARRAY ||
                arg_node->var_type == TYPE_VOID) {
                arg_node->var_type = arg_vt;
            }
        }
    }

    // If the argument still has no concrete type (VOID), attempt to resolve it
    // via a static declaration lookup rooted at the current program AST. This
    // helps when declarations and statements are interleaved (e.g., Rea) and
    // the annotation pass has not yet attributed the identifier use.
    if ((arg_vt == TYPE_VOID || arg_vt == TYPE_UNKNOWN) && arg_node->type == AST_VARIABLE && arg_node->token && arg_node->token->value) {
        if (gCurrentProgramRoot) {
            AST* decl = findStaticDeclarationInAST(arg_node->token->value, arg_node, gCurrentProgramRoot);
            if (decl) {
                AST* t = decl->right ? resolveTypeAlias(decl->right) : NULL;
                if (t) {
                    arg_actual = t;
                    arg_vt = t->var_type;
                }
            }
        }
    }

    // When coercion is not allowed, require an exact match of the base types
    // before proceeding with any structural comparisons. Allow NIL for pointer
    // parameters as a special case.
    if (!allow_coercion) {
        if (param_actual->var_type != arg_vt) {
            if (param_actual->var_type == TYPE_POINTER && arg_vt == TYPE_NIL) {
                return true;
            }
            // Treat unresolved identifiers (VOID) as compatible when the
            // parameter expects an integer. This aligns with frontends where
            // declaration attribution may occur later in the pipeline.
            if ((param_actual->var_type == TYPE_INT64 || param_actual->var_type == TYPE_INT32) &&
                (arg_vt == TYPE_VOID || arg_vt == TYPE_UNKNOWN)) {
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
            // Permit ordinal arguments to satisfy INTEGER parameters.  This
            // keeps BYTE/WORD and the extended integer family compatible with
            // routines that declare INTEGER parameters, mirroring traditional
            // Pascal's treatment of these types as interchangeable integer
            // values.
            if (param_actual->var_type == TYPE_INTEGER &&
                isIntegerFamilyType(arg_vt)) {
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
        if (!arg_actual) return true;          // Unknown pointer treated as compatible
        if (!compareTypeNodes(param_actual, arg_actual)) {
            AST* pa = resolveTypeAlias(param_actual->right);
            AST* aa = resolveTypeAlias(arg_actual->right);
            const char* pn = getTypeNameFromAST(pa);
            const char* an = getTypeNameFromAST(aa);
            if (!pn && pa && pa->token) pn = pa->token->value;
            if (!an && aa && aa->token) an = aa->token->value;
            if (pn && an && strcasecmp(pn, an) == 0) return true;
            if (isSubclassOf(aa, pa)) return true;
            return false;
        }
        return true;
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
static void compilePrintf(AST* node, BytecodeChunk* chunk, int line);

// --- Global/Module State for Compiler ---
// For mapping global variable names to an index during this compilation pass.
// This is a simplified approach for global variables.
typedef struct {
    char* name;
} CompilerGlobalVarInfo;

static int compilerGlobalCount = 0;

static CompilerGlobalVarInfo compilerGlobals[MAX_GLOBALS];

static CompilerConstant compilerConstants[MAX_COMPILER_CONSTANTS];
static int compilerConstantCount = 0;

static void initFunctionCompiler(FunctionCompilerState* fc) {
    fc->local_count = 0;
    fc->max_local_count = 0;
    fc->max_slot_used = 0;
    fc->scope_depth = 0;
    fc->name = NULL;
    fc->enclosing = NULL;
    fc->function_symbol = NULL;
    fc->upvalue_count = 0;
    fc->returns_value = false;
}

static void compilerBeginScope(FunctionCompilerState* fc) {
    if (!fc) return;
    fc->scope_depth++;
}

static void compilerEndScope(FunctionCompilerState* fc) {
    if (!fc) return;
    if (fc->scope_depth > 0) {
        fc->scope_depth--;
    }
}

static int findLocalByName(FunctionCompilerState* fc, const char* name) {
    if (!fc || !name) return -1;
    for (int i = fc->local_count - 1; i >= 0; i--) {
        CompilerLocal* local = &fc->locals[i];
        if (local->name && strcasecmp(local->name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void addLocal(FunctionCompilerState* fc, const char* name, int line, bool is_ref);

static void registerVarDeclLocals(AST* varDecl, bool emitError) {
    if (!current_function_compiler || !varDecl) return;
    for (int i = 0; i < varDecl->child_count; i++) {
        AST* varNameNode = varDecl->children[i];
        if (!varNameNode || !varNameNode->token || !varNameNode->token->value) continue;
        const char* name = varNameNode->token->value;
        int idx = findLocalByName(current_function_compiler, name);
        if (idx >= 0) {
        CompilerLocal* existing = &current_function_compiler->locals[idx];
        if (existing->depth < 0) {
            existing->depth = current_function_compiler->scope_depth;
            existing->is_ref = false;
            existing->is_captured = false;
            existing->decl_node = varDecl;
            continue;
        }
        if (existing->depth == current_function_compiler->scope_depth) {
            if (existing->decl_node == varDecl) {
                continue;
            }
            if (emitError) {
                fprintf(stderr, "L%d: duplicate variable '%s' in this scope.\n",
                        getLine(varNameNode), name);
                compiler_had_error = true;
            }
        } else {
            addLocal(current_function_compiler, name, getLine(varNameNode), false);
            CompilerLocal* fresh = &current_function_compiler->locals[current_function_compiler->local_count - 1];
            fresh->decl_node = varDecl;
        }
    } else {
        addLocal(current_function_compiler, name, getLine(varNameNode), false);
        CompilerLocal* fresh = &current_function_compiler->locals[current_function_compiler->local_count - 1];
        fresh->decl_node = varDecl;
    }
}
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
    loop_stack[loop_depth].continue_jumps = NULL;
    loop_stack[loop_depth].continue_count = 0;
    loop_stack[loop_depth].continue_target = -1;
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

    writeBytecodeChunk(chunk, JUMP, line);
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

static void addContinueJump(BytecodeChunk* chunk, int line) {
    if (loop_depth < 0) {
        fprintf(stderr, "L%d: Compiler error: 'continue' statement outside of a loop.\n", line);
        compiler_had_error = true;
        return;
    }
    Loop* current_loop = &loop_stack[loop_depth];
    writeBytecodeChunk(chunk, JUMP, line);
    if (current_loop->continue_target >= 0) {
        int from = chunk->count + 2; // after operand
        int to = current_loop->continue_target;
        int16_t rel = (int16_t)(to - from);
        emitShort(chunk, (uint16_t)rel, line);
    } else {
        current_loop->continue_count++;
        int* temp = realloc(current_loop->continue_jumps, sizeof(int) * current_loop->continue_count);
        if (!temp) {
            fprintf(stderr, "L%d: Compiler error: memory allocation failed for continue jumps.\\n", line);
            compiler_had_error = true;
            return;
        }
        current_loop->continue_jumps = temp;
        current_loop->continue_jumps[current_loop->continue_count - 1] = chunk->count; // operand offset
        emitShort(chunk, 0xFFFF, line);
    }
}

static void patchContinuesTo(BytecodeChunk* chunk, int targetAddress) {
    if (loop_depth < 0) return;
    Loop* current_loop = &loop_stack[loop_depth];
    for (int i = 0; i < current_loop->continue_count; i++) {
        int jump_offset = current_loop->continue_jumps[i];
        patchShort(chunk, jump_offset, (uint16_t)(targetAddress - (jump_offset + 2)));
    }
    if (current_loop->continue_jumps) {
        free(current_loop->continue_jumps);
        current_loop->continue_jumps = NULL;
    }
    current_loop->continue_count = 0;
}

static void endLoop(void) {
    if (loop_depth < 0) return;

    // This function now only manages the loop depth.
    // The patching and freeing of break_jumps is handled entirely by patchBreaks().
    // A check has been added to catch logic errors where endLoop is called
    // without a preceding patchBreaks() call.
    if (loop_stack[loop_depth].break_jumps != NULL || loop_stack[loop_depth].continue_jumps != NULL) {
        fprintf(stderr, "Compiler internal warning: endLoop called but break_jumps was not freed. Indicates missing patchBreaks() call.\n");
        // Safeguard free, though the call site is the real issue.
        free(loop_stack[loop_depth].break_jumps);
        loop_stack[loop_depth].break_jumps = NULL;
        if (loop_stack[loop_depth].continue_jumps) {
            free(loop_stack[loop_depth].continue_jumps);
            loop_stack[loop_depth].continue_jumps = NULL;
        }
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
    if (fc->local_count > fc->max_local_count) {
        fc->max_local_count = fc->local_count;
    }
    if (fc->local_count > fc->max_slot_used) {
        fc->max_slot_used = fc->local_count;
    }
    local->name = strdup(name);
    local->depth = fc->scope_depth;
    local->is_ref = is_ref;
    local->is_captured = false;
    local->decl_node = NULL;
}

static int resolveLocal(FunctionCompilerState* fc, const char* name) {
    if (!fc) return -1;
    for (int i = fc->local_count - 1; i >= 0; i--) {
        CompilerLocal* local = &fc->locals[i];
        if (local->depth < 0) continue;
        if (strcasecmp(name, local->name) == 0) {
            return i;
        }
    }
    return -1;
}

static void noteLocalSlotUse(FunctionCompilerState* fc, int slot) {
    if (!fc || slot < 0) return;
    int needed = slot + 1;
    if (needed > fc->max_slot_used) {
        fc->max_slot_used = needed;
    }
}

static void updateMaxSlotFromBytecode(FunctionCompilerState* fc, BytecodeChunk* chunk,
                                      int start_offset, int end_offset) {
    if (!fc || !chunk) return;
    int offset = start_offset;
    while (offset < end_offset) {
        uint8_t opcode = chunk->code[offset];
        int slot = -1;
        switch (opcode) {
            case GET_LOCAL:
            case SET_LOCAL:
            case GET_LOCAL_ADDRESS:
            case INC_LOCAL:
            case DEC_LOCAL:
            case INIT_LOCAL_ARRAY:
            case INIT_LOCAL_FILE:
            case INIT_LOCAL_POINTER:
            case INIT_LOCAL_STRING:
                if (offset + 1 < end_offset) {
                    slot = chunk->code[offset + 1];
                }
                break;
            default:
                break;
        }
        if (slot >= 0) {
            if (slot + 1 > fc->max_slot_used) {
                fc->max_slot_used = slot + 1;
            }
        }
        int instr_len = getInstructionLength(chunk, offset);
        if (instr_len <= 0) {
            instr_len = 1;
        }
        offset += instr_len;
    }
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
    if (current_class_const_table) {
        Symbol* sym = hashTableLookup(current_class_const_table, canonical_name);
        if (sym && sym->value) {
            return sym->value;
        }
    }
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
                } else if (node->token->type == TOKEN_HEX_CONST) {
                    // Parse hex literal (value string contains only hex digits, no '$')
                    if (node->var_type == TYPE_INT64 || node->var_type == TYPE_UINT64) {
                        unsigned long long v = strtoull(node->token->value, NULL, 16);
                        return makeInt64((long long)v);
                    } else {
                        unsigned long v = strtoul(node->token->value, NULL, 16);
                        return makeInt((long long)v);
                    }
                } else if (node->var_type == TYPE_INT64 || node->var_type == TYPE_UINT64) {
                    /*
                     * REA treats plain integer literals as 64-bit values.  The old
                     * implementation always produced a TYPE_INT32 Value which caused
                     * a runtime type mismatch when assigning to INT64 variables.
                     * Use makeInt64 so the literal's type matches the AST node.
                     */
                    return makeInt64(atoll(node->token->value));
                } else {
                    return makeInt(atoll(node->token->value));
                }
            }
            break;
        case AST_STRING:
            if (node->token && node->token->value) {
                size_t len = (node->i_val > 0) ? (size_t)node->i_val
                                               : strlen(node->token->value);
                if (len == 1) {
                    return makeChar((unsigned char)node->token->value[0]);
                }
                return makeStringLen(node->token->value, len);
            }
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
                            long long code = arg.i_val;
                            if (code >= 0 && code <= PASCAL_CHAR_MAX) {
                                Value result = makeChar((int)code);
                                freeValue(&arg);
                                return result;
                            }
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

                bool left_is_real = isRealType(left_val.type);
                bool right_is_real = isRealType(right_val.type);

                bool op_is_int_div = (node->token->type == TOKEN_INT_DIV);
                bool op_is_mod = (node->token->type == TOKEN_MOD);
                bool op_requires_int = op_is_int_div || op_is_mod;

                if (op_requires_int && (left_is_real || right_is_real)) {
                    fprintf(stderr, "Compile-time Error: '%s' operands must be integers in constant expressions.\n",
                            op_is_int_div ? "div" : "mod");
                } else if (left_is_real && right_is_real) {
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
                        default:
                            break;
                    }
                } else if (left_is_real || right_is_real) {
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

// Check whether a global variable with the given name has already been declared.
static bool globalVariableExists(const char* name) {
    for (int i = 0; i < compilerGlobalCount; i++) {
        if (compilerGlobals[i].name && strcmp(compilerGlobals[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static bool resolveUnitQualifiedGlobal(AST* node, char* outName, size_t outSize, Symbol** outSymbol) {
    if (!node || node->type != AST_FIELD_ACCESS || !outName || outSize == 0) {
        return false;
    }

    AST* base = node->left;
    if (!base || base->type != AST_VARIABLE || !base->token || !base->token->value) {
        return false;
    }

    if (!node->token || !node->token->value) {
        return false;
    }

    char qualified_buf[MAX_SYMBOL_LENGTH * 2 + 2];
    int written = snprintf(qualified_buf, sizeof(qualified_buf), "%s.%s", base->token->value, node->token->value);
    if (written < 0 || written >= (int)sizeof(qualified_buf)) {
        return false;
    }
    toLowerString(qualified_buf);

    Symbol* sym = lookupGlobalSymbol(qualified_buf);
    if (!sym) {
        return false;
    }

    sym = resolveSymbolAlias(sym);
    if (!sym) {
        return false;
    }

    strncpy(outName, qualified_buf, outSize - 1);
    outName[outSize - 1] = '\0';

    node->var_type = sym->type;
    node->type_def = sym->type_def;

    if (outSymbol) {
        *outSymbol = sym;
    }

    return true;
}

typedef struct {
    long long index;
    long long lower;
    long long upper;
} ConstArrayDimInfo;

typedef struct {
    AST* base_expr;
    AST* element_type;
    long long offset;
    int dim_count;
} ConstArrayAccessInfo;

static bool isValidConstArrayBase(AST* expr) {
    if (!expr) return false;
    switch (expr->type) {
        case AST_VARIABLE:
        case AST_FIELD_ACCESS:
        case AST_DEREFERENCE:
            return true;
        default:
            return false;
    }
}

static bool valueToOrdinal(const Value* value, long long* out) {
    if (!value || !out) return false;
    if (isIntlikeType(value->type)) {
        *out = value->i_val;
        return true;
    }
    if (value->type == TYPE_CHAR) {
        *out = (unsigned char)value->c_val;
        return true;
    }
    if (value->type == TYPE_BOOLEAN) {
        *out = value->i_val ? 1 : 0;
        return true;
    }
    if (value->type == TYPE_ENUM) {
        *out = value->enum_val.ordinal;
        return true;
    }
    return false;
}

static AST* resolveArrayTypeForExpression(AST* expr) {
    if (!expr) return NULL;

    AST* type_node = resolveTypeAlias(expr->type_def);

    if (!type_node && expr->type == AST_VARIABLE && expr->token && expr->token->value) {
        Symbol* sym = lookupLocalSymbol(expr->token->value);
        if (!sym) sym = lookupGlobalSymbol(expr->token->value);
        if (sym && sym->type_def) {
            type_node = resolveTypeAlias(sym->type_def);
        }
    }

    while (type_node && type_node->type == AST_POINTER_TYPE) {
        type_node = resolveTypeAlias(type_node->right);
    }

    return type_node;
}

static bool appendConstArrayDim(ConstArrayDimInfo** dims, int* count, int* capacity,
                                long long index, long long lower, long long upper) {
    if (!dims || !count || !capacity) return false;
    if (*count >= *capacity) {
        int new_capacity = (*capacity < 8) ? 8 : (*capacity * 2);
        ConstArrayDimInfo* resized = realloc(*dims, (size_t)new_capacity * sizeof(ConstArrayDimInfo));
        if (!resized) {
            return false;
        }
        *dims = resized;
        *capacity = new_capacity;
    }
    (*dims)[*count].index = index;
    (*dims)[*count].lower = lower;
    (*dims)[*count].upper = upper;
    (*count)++;
    return true;
}

static bool computeConstantArrayAccess(AST* node, ConstArrayAccessInfo* info) {
    if (!node || node->type != AST_ARRAY_ACCESS || !info) return false;

    AST* chain[64];
    int chain_len = 0;
    AST* current = node;
    while (current && current->type == AST_ARRAY_ACCESS) {
        if (chain_len >= (int)(sizeof(chain) / sizeof(chain[0]))) {
            return false;
        }
        chain[chain_len++] = current;
        current = current->left;
    }

    if (chain_len == 0 || !current) {
        return false;
    }

    AST* base_expr = current;
    if (!isValidConstArrayBase(base_expr)) {
        return false;
    }

    AST* current_type = resolveArrayTypeForExpression(base_expr);
    if (!current_type) {
        return false;
    }

    ConstArrayDimInfo* dims = NULL;
    int dims_count = 0;
    int dims_capacity = 0;
    bool success = true;

    for (int seg = chain_len - 1; seg >= 0 && success; --seg) {
        AST* segment = chain[seg];
        AST* array_type = resolveTypeAlias(current_type);
        if (!array_type || array_type->type != AST_ARRAY_TYPE) {
            success = false;
            break;
        }

        for (int idx = 0; idx < segment->child_count; ++idx) {
            AST* idx_expr = segment->children[idx];
            Value idx_val = evaluateCompileTimeValue(idx_expr);
            long long idx_num = 0;
            bool have_index = valueToOrdinal(&idx_val, &idx_num);
            freeValue(&idx_val);
            if (!have_index) {
                success = false;
                break;
            }

            if (idx >= array_type->child_count) {
                success = false;
                break;
            }

            AST* subrange = resolveTypeAlias(array_type->children[idx]);
            if (!subrange || subrange->type != AST_SUBRANGE || !subrange->left || !subrange->right) {
                success = false;
                break;
            }

            Value low_v = evaluateCompileTimeValue(subrange->left);
            Value high_v = evaluateCompileTimeValue(subrange->right);
            long long lower = 0, upper = -1;
            bool have_lower = valueToOrdinal(&low_v, &lower);
            bool have_upper = valueToOrdinal(&high_v, &upper);
            freeValue(&low_v);
            freeValue(&high_v);
            if (!have_lower || !have_upper) {
                success = false;
                break;
            }

            if (idx_num < lower || idx_num > upper) {
                success = false;
                break;
            }

            if (!appendConstArrayDim(&dims, &dims_count, &dims_capacity, idx_num, lower, upper)) {
                success = false;
                break;
            }
        }

        current_type = resolveTypeAlias(array_type->right);
    }

    if (!success || dims_count == 0) {
        free(dims);
        return false;
    }

    long long offset = 0;
    long long multiplier = 1;
    for (int i = dims_count - 1; i >= 0; --i) {
        long long span = dims[i].upper - dims[i].lower + 1;
        offset += (dims[i].index - dims[i].lower) * multiplier;
        multiplier *= span;
    }

    free(dims);

    if (offset < 0 || offset > (long long)UINT32_MAX) {
        return false;
    }

    info->base_expr = base_expr;
    info->element_type = current_type;
    info->offset = offset;
    info->dim_count = dims_count;
    return true;
}

// Returns true if the given const declaration node is nested inside a class
// definition (represented as a RECORD_TYPE within a TYPE_DECL).
static bool constIsClassMember(AST* node) {
    AST* p = node ? node->parent : NULL;
    while (p) {
        if (p->type == AST_RECORD_TYPE && p->parent && p->parent->type == AST_TYPE_DECL) {
            return true;
        }
        p = p->parent;
    }
    return false;
}

static bool pushFieldBaseAndResolveOffset(AST* node, BytecodeChunk* chunk, int line, int* outFieldOffset) {
    if (!node || node->type != AST_FIELD_ACCESS) {
        fprintf(stderr, "L%d: Compiler error: Invalid field access expression.\n", line);
        compiler_had_error = true;
        return false;
    }

    AST* base = node->left;
    if (!base) {
        fprintf(stderr, "L%d: Compiler error: Field access missing base expression.\n", line);
        compiler_had_error = true;
        return false;
    }

    if (base->var_type == TYPE_POINTER) {
        compileRValue(base, chunk, getLine(base));
    } else {
        compileLValue(base, chunk, getLine(base));
    }

    AST* recType = getRecordTypeFromExpr(base);
    if ((!recType || recType->type != AST_RECORD_TYPE) &&
        base->type == AST_VARIABLE && base->token && base->token->value &&
        (strcasecmp(base->token->value, "myself") == 0 ||
         strcasecmp(base->token->value, "my") == 0)) {
        if (!recType && current_class_record_type && current_class_record_type->type == AST_RECORD_TYPE) {
            recType = current_class_record_type;
        }
        if ((!recType || recType->type != AST_RECORD_TYPE) &&
            current_function_compiler && current_function_compiler->function_symbol &&
            current_function_compiler->function_symbol->name) {
            const char* fname = current_function_compiler->function_symbol->name;
            const char* dot = strchr(fname, '.');
            if (dot) {
                size_t len = (size_t)(dot - fname);
                char cls[MAX_SYMBOL_LENGTH];
                if (len >= sizeof(cls)) len = sizeof(cls) - 1;
                memcpy(cls, fname, len);
                cls[len] = '\0';
                for (size_t i = 0; i < len; i++) cls[i] = (char)tolower(cls[i]);
                recType = lookupType(cls);
                recType = resolveTypeAlias(recType);
                if (recType && recType->type == AST_TYPE_DECL && recType->left) {
                    recType = recType->left;
                }
            }
        }
        if (!recType || recType->type != AST_RECORD_TYPE) {
            recType = findRecordTypeByFieldName(node->token ? node->token->value : NULL);
        }
    }

    int fieldOffset = getRecordFieldOffset(recType, node->token ? node->token->value : NULL);
    if (fieldOffset < 0 && recType && base->type == AST_VARIABLE &&
        base->token && base->token->value &&
        (strcasecmp(base->token->value, "myself") == 0 ||
         strcasecmp(base->token->value, "my") == 0)) {
        int offset = 0;
        if (recType->extra && recType->extra->token && recType->extra->token->value) {
            AST* parent = lookupType(recType->extra->token->value);
            offset = getRecordFieldCount(parent);
        }
        for (int i = 0; fieldOffset < 0 && recType && i < recType->child_count; i++) {
            AST* decl = recType->children[i];
            if (!decl) continue;
            if (decl->type == AST_VAR_DECL) {
                for (int j = 0; j < decl->child_count; j++) {
                    AST* var = decl->children[j];
                    if (var && var->token && node->token && node->token->value &&
                        strcasecmp(var->token->value, node->token->value) == 0) {
                        fieldOffset = offset;
                        break;
                    }
                    offset++;
                }
            } else if (decl->token) {
                if (node->token && node->token->value &&
                    strcasecmp(decl->token->value, node->token->value) == 0) {
                    fieldOffset = offset;
                    break;
                }
                offset++;
            }
        }
    }

    if (recordTypeHasVTable(recType)) fieldOffset++;

    if (fieldOffset < 0) {
        fprintf(stderr, "L%d: Compiler error: Unknown field '%s'.\n", line,
                node->token ? node->token->value : "<null>");
        compiler_had_error = true;
        return false;
    }

    if (outFieldOffset) *outFieldOffset = fieldOffset;
    return true;
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
                if (isCurrentFunctionResultIdentifier(current_function_compiler, varName)) {
                    local_slot = resolveLocal(current_function_compiler, current_function_compiler->name);
                } else {
                    local_slot = resolveLocal(current_function_compiler, varName);
                }

                if (local_slot != -1) {
                    is_ref = current_function_compiler->locals[local_slot].is_ref;
                }
                // Robust fallback: if the variable is declared somewhere in the current
                // function's body but wasn't added to the locals table yet (e.g., due to
                // interleaved declarations/statements or frontend nuances), discover it
                // and register it now so we address it as a local rather than a global.
                if (compiler_dynamic_locals && local_slot == -1 && current_function_compiler->function_symbol &&
                    current_function_compiler->function_symbol->type_def) {
                    AST* func_decl = current_function_compiler->function_symbol->type_def;
                    AST* decl_in_scope = findDeclarationInScope(varName, func_decl, node);
                    if (decl_in_scope && astNodeIsDescendant(func_decl, decl_in_scope)) {
                        addLocal(current_function_compiler, varName, line, false);
                        local_slot = current_function_compiler->local_count - 1;
                        is_ref = false;
                    }
                }
            }

            bool treat_as_local = (local_slot != -1);
            if (treat_as_local && current_function_compiler) {
                int param_count = 0;
                if (current_function_compiler->function_symbol) {
                    param_count = current_function_compiler->function_symbol->arity;
                }
                bool is_param = (local_slot < param_count);
                if (!is_param) {
                    Symbol* local_sym = lookupLocalSymbol(varName);
                    if (local_sym && !local_sym->is_local_var) {
                        treat_as_local = false;
                    }
                }
            }

            if (treat_as_local) {
                // For by-ref locals, the slot holds an address already.
                if (is_ref) {
                    noteLocalSlotUse(current_function_compiler, local_slot);
                    writeBytecodeChunk(chunk, GET_LOCAL, line);
                    writeBytecodeChunk(chunk, (uint8_t)local_slot, line);
                } else {
                    noteLocalSlotUse(current_function_compiler, local_slot);
                    writeBytecodeChunk(chunk, GET_LOCAL_ADDRESS, line);
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
                        writeBytecodeChunk(chunk, GET_UPVALUE, line);
                        writeBytecodeChunk(chunk, (uint8_t)upvalue_slot, line);
                    } else {
                        writeBytecodeChunk(chunk, GET_UPVALUE_ADDRESS, line);
                        writeBytecodeChunk(chunk, (uint8_t)upvalue_slot, line);
                    }
                } else {
                    if (!globalVariableExists(varName) && !lookupGlobalSymbol(varName)) {
                        fprintf(stderr, "L%d: Undefined variable '%s'.\n", line, varName);
                        if (current_function_compiler && current_function_compiler->name) {
                            DBG_PRINTF("[dbg] in function '%s', locals=", current_function_compiler->name);
                            for (int li = 0; li < current_function_compiler->local_count; li++) {
                                const char* lname = current_function_compiler->locals[li].name;
                                if (!lname) continue;
                                fprintf(stderr, "%s%s", li==0?"":" ,", lname);
                            }
                            fprintf(stderr, "\n");
                        }
                        compiler_had_error = true;
                        break;
                    }
                    int nameIndex = addStringConstant(chunk, varName);
                    emitGlobalNameIdx(chunk, GET_GLOBAL_ADDRESS, GET_GLOBAL_ADDRESS16,
                                       nameIndex, line);
                }
            }
            break;
        }
        case AST_FIELD_ACCESS: {
            char qualified_name[MAX_SYMBOL_LENGTH * 2 + 2];
            Symbol* resolved_symbol = NULL;
            if (resolveUnitQualifiedGlobal(node, qualified_name, sizeof(qualified_name), &resolved_symbol)) {
                if (resolved_symbol && resolved_symbol->is_const) {
                    fprintf(stderr, "L%d: Compiler error: Cannot assign to constant '%s'.\n", line, qualified_name);
                    compiler_had_error = true;
                    break;
                }

                int nameIndex = addStringConstant(chunk, qualified_name);
                emitGlobalNameIdx(chunk, GET_GLOBAL_ADDRESS, GET_GLOBAL_ADDRESS16,
                                   nameIndex, line);
                break;
            }

            if (node->token && node->token->value) {
                Value* const_ptr = findCompilerConstant(node->token->value);
                if (const_ptr) {
                    fprintf(stderr,
                            "L%d: Compiler error: Cannot take address of constant field '%s'.\n",
                            line, node->token->value);
                    compiler_had_error = true;
                    break;
                }
            }

            int fieldOffset = -1;
            if (!pushFieldBaseAndResolveOffset(node, chunk, line, &fieldOffset)) {
                break;
            }

            if (fieldOffset <= 0xFF) {
                writeBytecodeChunk(chunk, GET_FIELD_OFFSET, line);
                writeBytecodeChunk(chunk, (uint8_t)fieldOffset, line);
            } else {
                writeBytecodeChunk(chunk, GET_FIELD_OFFSET16, line);
                emitShort(chunk, (uint16_t)fieldOffset, line);
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
                writeBytecodeChunk(chunk, GET_CHAR_ADDRESS, line); // CORRECT: Pops both, pushes address of the character
                break; // We are done with this case
            } else {
                ConstArrayAccessInfo const_info;
                if (computeConstantArrayAccess(node, &const_info)) {
                    compileLValue(const_info.base_expr, chunk, getLine(const_info.base_expr));
                    writeBytecodeChunk(chunk, GET_ELEMENT_ADDRESS_CONST, line);
                    emitInt32(chunk, (uint32_t)const_info.offset, line);
                    break;
                }
                // Standard array access: push the array base address first, followed by
                // each index expression in declaration order. GET_ELEMENT_ADDRESS expects
                // to pop the base first, then each index in order.

                // Push indices first
                for (int i = 0; i < node->child_count; i++) {
                    compileRValue(node->children[i], chunk, getLine(node->children[i]));
                }
                // Push address of the array variable (Value*) last so base is on top.
                compileLValue(node->left, chunk, getLine(node->left));

                // Now, get the address of the specific element.
                writeBytecodeChunk(chunk, GET_ELEMENT_ADDRESS, line);
                writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
            }
            break;
        }
        case AST_NEW: {
            if (!node || !node->token || !node->token->value) { break; }
            global_init_new_depth++;
            const char* className = node->token->value;
            char lowerClassName[MAX_SYMBOL_LENGTH];
            strncpy(lowerClassName, className, sizeof(lowerClassName) - 1);
            lowerClassName[sizeof(lowerClassName) - 1] = '\0';
            toLowerString(lowerClassName);
            AST* classType = lookupType(lowerClassName);

            bool hasVTable = recordTypeHasVTable(classType);
            int fieldCount = getRecordFieldCount(classType) + (hasVTable ? 1 : 0);
            bool defer_vtable = compiling_global_var_init && global_init_new_depth == 1;

            if (fieldCount <= 0xFF) {
                writeBytecodeChunk(chunk, ALLOC_OBJECT, line);
                writeBytecodeChunk(chunk, (uint8_t)fieldCount, line);
            } else {
                writeBytecodeChunk(chunk, ALLOC_OBJECT16, line);
                emitShort(chunk, (uint16_t)fieldCount, line);
            }

            if (hasVTable) {
                if (defer_vtable) {
                    // Constructors executed during global initialisation may immediately
                    // invoke virtual methods (for example, via `myself.setupLighting()`),
                    // so make sure the vtable pointer is installed right away even when
                    // we plan to refresh it later once all vtables have been emitted.
                }
                // Initialise hidden __vtable field (offset 0)
                writeBytecodeChunk(chunk, DUP, line);
                writeBytecodeChunk(chunk, GET_FIELD_OFFSET, line);
                writeBytecodeChunk(chunk, (uint8_t)0, line);
                char vtName[512];
                snprintf(vtName, sizeof(vtName), "%s_vtable", lowerClassName);
                int vtNameIdx = addStringConstant(chunk, vtName);
                emitGlobalNameIdx(chunk, GET_GLOBAL_ADDRESS, GET_GLOBAL_ADDRESS16, vtNameIdx, line);
                writeBytecodeChunk(chunk, SET_INDIRECT, line);
            }

            emitArrayFieldInitializers(classType, chunk, line, hasVTable);

            Symbol* ctorSymbol = lookupProcedure(lowerClassName);
            Symbol* resolvedCtor = resolveSymbolAlias(ctorSymbol);
            const char* ctorLookupName = lowerClassName;
            if (resolvedCtor && resolvedCtor->name) {
                ctorLookupName = resolvedCtor->name;
            } else if (ctorSymbol && ctorSymbol->name) {
                ctorLookupName = ctorSymbol->name;
            }

            if (resolvedCtor || ctorSymbol || node->child_count > 0) {
                writeBytecodeChunk(chunk, DUP, line);
                for (int i = 0; i < node->child_count; i++) {
                    compileRValue(node->children[i], chunk, getLine(node->children[i]));
                }
                int ctorNameIdx = addStringConstant(chunk, ctorLookupName);
                writeBytecodeChunk(chunk, CALL_USER_PROC, line);
                emitShort(chunk, (uint16_t)ctorNameIdx, line);
                writeBytecodeChunk(chunk, (uint8_t)(node->child_count + 1), line);
            }
            global_init_new_depth--;
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

static bool emitDirectStoreForVariable(AST* lvalue, BytecodeChunk* chunk, int line) {
    if (!lvalue || lvalue->type != AST_VARIABLE || !lvalue->token || !lvalue->token->value) {
        return false;
    }

    VarType target_type = lvalue->var_type;
    if (target_type == TYPE_UNKNOWN) {
        Symbol* sym = lookupGlobalSymbol(lvalue->token->value);
        if (sym) {
            target_type = sym->type;
        }
    }

    bool type_is_safe = false;
    if (target_type != TYPE_UNKNOWN) {
        if (isRealType(target_type)) {
            type_is_safe = true;
        } else if (target_type == TYPE_BOOLEAN) {
            type_is_safe = true;
        }
    }

    if (!type_is_safe) {
        return false;
    }

    if (line <= 0) {
        line = getLine(lvalue);
    }
    if (line <= 0) {
        line = 0;
    }

    const char* varName = lvalue->token->value;

    if (current_function_compiler) {
        int local_slot = -1;
        if (isCurrentFunctionResultIdentifier(current_function_compiler, varName)) {
            local_slot = resolveLocal(current_function_compiler, current_function_compiler->name);
        } else {
            local_slot = resolveLocal(current_function_compiler, varName);
        }

        if (local_slot != -1) {
            if (current_function_compiler->locals[local_slot].is_ref) {
                return false;
            }
            noteLocalSlotUse(current_function_compiler, local_slot);
            writeBytecodeChunk(chunk, SET_LOCAL, line);
            writeBytecodeChunk(chunk, (uint8_t)local_slot, line);
            return true;
        }

        int upvalue_slot = resolveUpvalue(current_function_compiler, varName);
        if (upvalue_slot != -1) {
            if (current_function_compiler->upvalues[upvalue_slot].is_ref) {
                return false;
            }
            writeBytecodeChunk(chunk, SET_UPVALUE, line);
            writeBytecodeChunk(chunk, (uint8_t)upvalue_slot, line);
            return true;
        }
    }

    if (!globalVariableExists(varName) && !lookupGlobalSymbol(varName)) {
        return false;
    }

    int nameIdx = addStringConstant(chunk, varName);
    emitGlobalNameIdx(chunk, SET_GLOBAL, SET_GLOBAL16, nameIdx, line);
    return true;
}

static bool readConstantInt(BytecodeChunk* chunk, int index, long long* out_value) {
    if (!chunk || index < 0 || index >= chunk->constants_count) {
        return false;
    }

    Value const_val = chunk->constants[index];
    if (!IS_INTLIKE(const_val)) {
        return false;
    }

    if (out_value) {
        *out_value = AS_INTEGER(const_val);
    }
    return true;
}

static void applyPeepholeOptimizations(BytecodeChunk* chunk) {
    if (!chunk || chunk->count <= 0 || !chunk->code) {
        return;
    }

    const int original_count = chunk->count;
    uint8_t* original_code = chunk->code;
    int* original_lines = chunk->lines;

    typedef struct {
        int original_target;
        int new_offset;
    } JumpFixup;

    typedef struct {
        int operand_offset;
        int original_address;
    } AbsoluteFixup;

    uint8_t* optimized_code = (uint8_t*)malloc((size_t)original_count);
    int* optimized_lines = (int*)malloc((size_t)original_count * sizeof(int));
    int* offset_map = (int*)malloc((size_t)(original_count + 1) * sizeof(int));
    bool* original_instruction_starts = NULL;
    JumpFixup* jump_fixes = NULL;
    int jump_count = 0;
    int jump_capacity = 0;
    AbsoluteFixup* absolute_fixes = NULL;
    int absolute_count = 0;
    int absolute_capacity = 0;
    bool abort_optimizations = true;

    if (!optimized_code || !optimized_lines || !offset_map) {
        goto cleanup;
    }

    original_instruction_starts = (bool*)calloc((size_t)(original_count + 1), sizeof(bool));
    if (!original_instruction_starts) {
        goto cleanup;
    }

    for (int i = 0; i <= original_count; ++i) {
        offset_map[i] = -1;
    }

    bool instruction_map_error = false;
    int scan_offset = 0;
    while (scan_offset < original_count) {
        original_instruction_starts[scan_offset] = true;
        int instr_len = getInstructionLength(chunk, scan_offset);
        if (instr_len <= 0 || scan_offset + instr_len > original_count) {
            if (compiler_debug) {
                DBG_PRINTF("[dbg] Invalid instruction encountered at byte %d while preparing peephole map (len=%d).\n",
                           scan_offset,
                           instr_len);
            }
            fprintf(stderr,
                    "Compiler error: Invalid instruction layout encountered while optimizing bytecode.\n");
            compiler_had_error = true;
#ifndef NDEBUG
            assert(!"invalid instruction layout before peephole optimization");
#endif
            instruction_map_error = true;
            break;
        }
        scan_offset += instr_len;
    }
    original_instruction_starts[original_count] = true;

    if (instruction_map_error) {
        goto cleanup;
    }

    int read_index = 0;
    int write_index = 0;
    bool changed = false;

    while (read_index < original_count) {
        uint8_t opcode = original_code[read_index];

        if (opcode == CONSTANT || opcode == CONSTANT16) {
            int constant_length = (opcode == CONSTANT) ? 2 : 3;
            int constant_index = -1;
            if (opcode == CONSTANT) {
                if (read_index + 1 < original_count) {
                    constant_index = original_code[read_index + 1];
                }
            } else {
                if (read_index + 2 < original_count) {
                    constant_index = (original_code[read_index + 1] << 8) |
                                     original_code[read_index + 2];
                }
            }

            int call_offset = read_index + constant_length;
            if (constant_index >= 0 && call_offset + 3 < original_count &&
                original_code[call_offset] == CALL_BUILTIN) {
                int builtin_name_idx = (original_code[call_offset + 1] << 8) |
                                        original_code[call_offset + 2];
                uint8_t arg_count = original_code[call_offset + 3];
                if (arg_count == 1 &&
                    builtin_name_idx >= 0 &&
                    builtin_name_idx < chunk->constants_count &&
                    constant_index < chunk->constants_count) {
                    Value* builtin_name_val = &chunk->constants[builtin_name_idx];
                    if (builtin_name_val->type == TYPE_STRING &&
                        builtin_name_val->s_val &&
                        strcasecmp(builtin_name_val->s_val, "byte") == 0) {
                        Value const_val = chunk->constants[constant_index];
                        if (isIntlikeType(const_val.type) &&
                            const_val.type != TYPE_BOOLEAN &&
                            const_val.type != TYPE_CHAR) {
                            long long iv = AS_INTEGER(const_val);
                            if (iv >= 0 && iv <= 255) {
                                int replacement_start = write_index;
                                for (int i = 0; i < constant_length &&
                                                (read_index + i) < original_count; ++i) {
                                    optimized_code[write_index] = original_code[read_index + i];
                                    optimized_lines[write_index] = original_lines
                                        ? original_lines[read_index + i] : 0;
                                    offset_map[read_index + i] = write_index;
                                    write_index++;
                                }
                                for (int i = 0; i < 4; ++i) {
                                    offset_map[call_offset + i] = (write_index > 0)
                                        ? (write_index - 1)
                                        : replacement_start;
                                }
                                read_index += constant_length + 4;
                                changed = true;
                                continue;
                            }
                        }
                    }
                }
            }
        }

        if (opcode == GET_LOCAL) {
            if (read_index + 6 < original_count) {
                uint8_t slot = original_code[read_index + 1];
                int const_offset = read_index + 2;
                uint8_t const_opcode = original_code[const_offset];
                int constant_length = 0;
                int constant_index = -1;

                if (const_opcode == CONSTANT) {
                    if (const_offset + 1 < original_count) {
                        constant_index = original_code[const_offset + 1];
                        constant_length = 2;
                    }
                } else if (const_opcode == CONSTANT16) {
                    if (const_offset + 2 < original_count) {
                        constant_index = (original_code[const_offset + 1] << 8) |
                                         original_code[const_offset + 2];
                        constant_length = 3;
                    }
                }

                if (constant_length > 0) {
                    int arithmetic_offset = const_offset + constant_length;
                    if (arithmetic_offset < original_count) {
                        uint8_t arithmetic_opcode = original_code[arithmetic_offset];
                        if (arithmetic_opcode == ADD || arithmetic_opcode == SUBTRACT) {

                            long long constant_value = 0;
                            if (readConstantInt(chunk, constant_index, &constant_value)) {
                                uint8_t replacement = 0;
                                if (arithmetic_opcode == ADD) {
                                    if (constant_value == 1) replacement = INC_LOCAL;
                                    else if (constant_value == -1) replacement = DEC_LOCAL;
                                } else {
                                    if (constant_value == 1) replacement = DEC_LOCAL;
                                    else if (constant_value == -1) replacement = INC_LOCAL;
                                }

                                int store_offset = arithmetic_offset + 1;
                                if (replacement != 0 && store_offset < original_count) {
                                    bool handled = false;
                                    if (original_code[store_offset] == SET_LOCAL &&
                                        store_offset + 1 < original_count &&
                                        original_code[store_offset + 1] == slot) {
                                        int sequence_length = 2 + constant_length + 1 + 2;
                                        int replacement_start = write_index;
                                        optimized_code[write_index] = replacement;
                                        optimized_lines[write_index++] = original_lines
                                            ? original_lines[read_index] : 0;
                                        optimized_code[write_index] = slot;
                                        optimized_lines[write_index++] = original_lines
                                            ? original_lines[read_index] : 0;
                                        for (int i = 0; i < sequence_length &&
                                                (read_index + i) < original_count; ++i) {
                                            offset_map[read_index + i] = replacement_start;
                                        }
                                        read_index += sequence_length;
                                        changed = true;
                                        handled = true;
                                    } else if (original_code[store_offset] == GET_LOCAL_ADDRESS &&
                                               store_offset + 3 < original_count &&
                                               original_code[store_offset + 1] == slot &&
                                               original_code[store_offset + 2] == SWAP &&
                                               original_code[store_offset + 3] == SET_INDIRECT) {
                                        int sequence_length = 2 + constant_length + 1 + 4;
                                        int replacement_start = write_index;
                                        optimized_code[write_index] = replacement;
                                        optimized_lines[write_index++] = original_lines
                                            ? original_lines[read_index] : 0;
                                        optimized_code[write_index] = slot;
                                        optimized_lines[write_index++] = original_lines
                                            ? original_lines[read_index] : 0;
                                        for (int i = 0; i < sequence_length &&
                                                (read_index + i) < original_count; ++i) {
                                            offset_map[read_index + i] = replacement_start;
                                        }
                                        read_index += sequence_length;
                                        changed = true;
                                        handled = true;
                                    }

                                    if (handled) {
                                        continue;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        int instruction_length = getInstructionLength(chunk, read_index);
        if (instruction_length <= 0) instruction_length = 1;

        if ((opcode == JUMP || opcode == JUMP_IF_FALSE) && read_index + 2 < original_count) {
            if (jump_count >= jump_capacity) {
                int new_capacity = jump_capacity < 8 ? 8 : jump_capacity * 2;
                JumpFixup* resized = (JumpFixup*)realloc(jump_fixes, (size_t)new_capacity * sizeof(JumpFixup));
                if (!resized) {
                    goto cleanup;
                }
                jump_fixes = resized;
                jump_capacity = new_capacity;
            }
            int16_t operand = (int16_t)((original_code[read_index + 1] << 8) | original_code[read_index + 2]);
            jump_fixes[jump_count].original_target = read_index + 3 + operand;
            jump_fixes[jump_count].new_offset = write_index;
            jump_count++;
        } else if (opcode == THREAD_CREATE && read_index + 2 < original_count) {
            if (absolute_count >= absolute_capacity) {
                int new_capacity = absolute_capacity < 8 ? 8 : absolute_capacity * 2;
                AbsoluteFixup* resized = (AbsoluteFixup*)realloc(absolute_fixes, (size_t)new_capacity * sizeof(AbsoluteFixup));
                if (!resized) {
                    goto cleanup;
                }
                absolute_fixes = resized;
                absolute_capacity = new_capacity;
            }
            uint16_t original_address = (uint16_t)((original_code[read_index + 1] << 8) |
                                                    original_code[read_index + 2]);
            absolute_fixes[absolute_count].operand_offset = write_index + 1;
            absolute_fixes[absolute_count].original_address = (int)original_address;
            absolute_count++;
        }

        for (int i = 0; i < instruction_length && (read_index + i) < original_count; i++) {
            optimized_code[write_index] = original_code[read_index + i];
            optimized_lines[write_index] = original_lines ? original_lines[read_index + i] : 0;
            offset_map[read_index + i] = write_index;
            write_index++;
        }
        read_index += instruction_length;
    }

    offset_map[original_count] = write_index;

    if (!changed) {
        goto cleanup;
    }

    for (int i = 0; i < jump_count; ++i) {
        int original_target = jump_fixes[i].original_target;
        bool target_is_end = (original_target == original_count);
        bool target_within = (original_target >= 0 && original_target < original_count);
        bool target_marked = target_within ? original_instruction_starts[original_target] : false;

        if (!target_is_end && (!target_within || !target_marked)) {
            if (compiler_debug) {
                DBG_PRINTF("[dbg] Peephole optimizer encountered invalid jump target %d at new offset %d.\n",
                           original_target,
                           jump_fixes[i].new_offset);
            }
            fprintf(stderr,
                    "Compiler error: Peephole optimizer encountered invalid jump target %d.\n",
                    original_target);
            compiler_had_error = true;
#ifndef NDEBUG
            assert(!"peephole optimizer encountered invalid jump target");
#endif
            continue;
        }

        int target_index = target_is_end ? original_count : original_target;
        int new_target = offset_map[target_index];
        if (new_target < 0) {
            if (compiler_debug) {
                DBG_PRINTF("[dbg] Peephole optimizer could not map jump target %d (new offset %d).\n",
                           original_target,
                           jump_fixes[i].new_offset);
            }
            fprintf(stderr,
                    "Compiler error: Peephole optimizer could not map jump target %d.\n",
                    original_target);
            compiler_had_error = true;
#ifndef NDEBUG
            assert(!"peephole optimizer missing mapping for jump target");
#endif
            continue;
        }

        int new_offset = jump_fixes[i].new_offset;
        int new_delta = new_target - (new_offset + 3);
        optimized_code[new_offset + 1] = (uint8_t)((new_delta >> 8) & 0xFF);
        optimized_code[new_offset + 2] = (uint8_t)(new_delta & 0xFF);
    }

    for (int i = 0; i < absolute_count; ++i) {
        int original_address = absolute_fixes[i].original_address;
        if (original_address < 0) original_address = 0;
        if (original_address > original_count) original_address = original_count;
        int new_address = offset_map[original_address];
        if (new_address < 0) new_address = offset_map[original_count];
        optimized_code[absolute_fixes[i].operand_offset] = (uint8_t)((new_address >> 8) & 0xFF);
        optimized_code[absolute_fixes[i].operand_offset + 1] = (uint8_t)(new_address & 0xFF);
    }

    uint8_t* resized_code = (uint8_t*)realloc(optimized_code, (size_t)write_index);
    if (resized_code) optimized_code = resized_code;
    int* resized_lines = (int*)realloc(optimized_lines, (size_t)write_index * sizeof(int));
    if (resized_lines) optimized_lines = resized_lines;

    free(chunk->code);
    free(chunk->lines);
    chunk->code = optimized_code;
    chunk->lines = optimized_lines;
    chunk->count = write_index;
    chunk->capacity = write_index;

    if (procedure_table) {
        for (int bucket = 0; bucket < HASHTABLE_SIZE; ++bucket) {
            for (Symbol* sym = procedure_table->buckets[bucket]; sym; sym = sym->next) {
                Symbol* target = resolveSymbolAlias(sym);
                if (!target || target != sym || !target->is_defined) continue;
                int old_address = target->bytecode_address;
                if (old_address < 0 || old_address > original_count) continue;
                int mapped = offset_map[old_address];
                if (mapped < 0) mapped = offset_map[original_count];
                target->bytecode_address = mapped;
            }
        }
    }

    for (int i = 0; i < address_constant_count; ++i) {
        int const_index = address_constant_entries[i].constant_index;
        if (const_index < 0 || const_index >= chunk->constants_count) continue;
        int old_address = address_constant_entries[i].original_address;
        if (old_address < 0) old_address = 0;
        if (old_address > original_count) old_address = original_count;
        int mapped = offset_map[old_address];
        if (mapped < 0) mapped = offset_map[original_count];

        int element_index = address_constant_entries[i].element_index;
        if (element_index >= 0) {
            Value* array_const = &chunk->constants[const_index];
            if (array_const->type == TYPE_ARRAY && array_const->array_val) {
                int total = calculateArrayTotalSize(array_const);
                if (element_index >= 0 && element_index < total) {
                    Value* elem = &array_const->array_val[element_index];
                    SET_INT_VALUE(elem, mapped);
                    elem->type = TYPE_INT32;
                }
            }
        } else {
            Value* val = &chunk->constants[const_index];
            val->i_val = (long long)mapped;
            val->u_val = (unsigned long long)mapped;
        }
    }
    abort_optimizations = false;

cleanup:
    if (offset_map) free(offset_map);
    if (jump_fixes) free(jump_fixes);
    if (absolute_fixes) free(absolute_fixes);
    if (original_instruction_starts) free(original_instruction_starts);
    if (abort_optimizations) {
        free(optimized_code);
        free(optimized_lines);
    }
}


bool compileASTToBytecode(AST* rootNode, BytecodeChunk* outputChunk) {
    if (!rootNode || !outputChunk) return false;
    if (!ensureProcedureTableInitialized()) return false;
    resetAddressConstantTracking();
    if (!compiler_debug) {
        const char* d = getenv("REA_DEBUG");
        if (d && *d && *d != '0') compiler_debug = 1;
    }
    bool vtable_state_pushed = false;
    if (tracked_vtable_chunk != NULL) {
        vtable_state_pushed = pushVTableTrackerState(outputChunk);
    } else {
        initializeVTableTracker(outputChunk);
    }
    gCurrentProgramRoot = rootNode;
    compilerGlobalCount = 0;
    compiler_had_error = false;
    current_function_compiler = NULL;
    compiler_defined_myself_global = false;
    compiler_myself_global_name_idx = -1;
    postpone_global_initializers = false;
    if (deferred_global_initializers) {
        free(deferred_global_initializers);
        deferred_global_initializers = NULL;
    }
    deferred_global_initializer_count = 0;
    deferred_global_initializer_capacity = 0;

    ensureMyselfGlobalDefined(outputChunk, rootNode ? getLine(rootNode) : 0);

    current_procedure_table = procedure_table;

    LabelTableState program_labels;
    initLabelTable(&program_labels);

    if (rootNode->type == AST_PROGRAM) {
        if (rootNode->right && rootNode->right->type == AST_BLOCK) {
            compileNode(rootNode->right, outputChunk, getLine(rootNode));
        } else {
            fprintf(stderr, "Compiler error: AST_PROGRAM node missing main block.\n");
            compiler_had_error = true;
        }
    } else {
        fprintf(stderr, "Compiler error: Expected AST_PROGRAM as root for compilation, got %s.\n",
                astTypeToString(rootNode->type));
        compiler_had_error = true;
    }

    finalizeLabelTable(&program_labels, "program");

    if (!compiler_had_error) {
        writeBytecodeChunk(outputChunk, HALT, rootNode ? getLine(rootNode) : 0);
        applyPeepholeOptimizations(outputChunk);
    }
    if (vtable_state_pushed) {
        popVTableTrackerState();
    }
    return !compiler_had_error;
}

bool compileModuleAST(AST* rootNode, BytecodeChunk* outputChunk) {
    if (!rootNode || !outputChunk) return false;
    if (!ensureProcedureTableInitialized()) return false;
    resetAddressConstantTracking();
    if (!compiler_debug) {
        const char* d = getenv("REA_DEBUG");
        if (d && *d && *d != '0') compiler_debug = 1;
    }
    bool vtable_state_pushed = false;
    if (tracked_vtable_chunk != NULL) {
        vtable_state_pushed = pushVTableTrackerState(outputChunk);
    } else {
        initializeVTableTracker(outputChunk);
    }
    gCurrentProgramRoot = rootNode;
    compilerGlobalCount = 0;
    compiler_had_error = false;
    current_function_compiler = NULL;
    int saved_myself_flag = compiler_defined_myself_global;
    int saved_myself_idx = compiler_myself_global_name_idx;
    compiler_defined_myself_global = true;
    compiler_myself_global_name_idx = saved_myself_idx;
    postpone_global_initializers = false;
    if (deferred_global_initializers) {
        free(deferred_global_initializers);
        deferred_global_initializers = NULL;
    }
    deferred_global_initializer_count = 0;
    deferred_global_initializer_capacity = 0;

    ensureMyselfGlobalDefined(outputChunk, rootNode ? getLine(rootNode) : 0);

    current_procedure_table = procedure_table;

    LabelTableState module_labels;
    initLabelTable(&module_labels);

    const char *moduleName = NULL;
    if (rootNode->type == AST_PROGRAM && rootNode->right && rootNode->right->type == AST_BLOCK &&
        rootNode->right->child_count > 0) {
        AST *decls = rootNode->right->children[0];
        if (decls && decls->type == AST_COMPOUND) {
            for (int i = 0; i < decls->child_count; i++) {
                AST *child = decls->children[i];
                if (child && child->type == AST_MODULE && child->token && child->token->value) {
                    moduleName = child->token->value;
                    break;
                }
            }
        }
    }

    compilerSetCurrentUnitName(moduleName);

    if (rootNode->type == AST_PROGRAM) {
        if (rootNode->right && rootNode->right->type == AST_BLOCK) {
            compileNode(rootNode->right, outputChunk, getLine(rootNode));
        } else {
            fprintf(stderr, "Compiler error: AST_PROGRAM node missing main block in module compilation.\n");
            compiler_had_error = true;
        }
    } else {
        fprintf(stderr, "Compiler error: Expected AST_PROGRAM as root for module compilation, got %s.\n",
                astTypeToString(rootNode->type));
        compiler_had_error = true;
    }
    compilerSetCurrentUnitName(NULL);
    compiler_defined_myself_global = saved_myself_flag;
    compiler_myself_global_name_idx = saved_myself_idx;

    finalizeLabelTable(&module_labels, moduleName ? moduleName : "module");

    if (vtable_state_pushed) {
        popVTableTrackerState();
    }
    return !compiler_had_error;
}

void compilerResetState(void) {
    current_compilation_unit_name = NULL;
    gCurrentProgramRoot = NULL;
    current_class_const_table = NULL;
    current_class_record_type = NULL;
    current_function_compiler = NULL;
    current_label_table = NULL;
    compiler_defined_myself_global = false;
    compiler_myself_global_name_idx = -1;
    compilerGlobalCount = 0;
    resetCompilerConstants();
    compiler_had_error = false;
    postpone_global_initializers = false;
    resetAddressConstantTracking();
    if (address_constant_entries) {
        free(address_constant_entries);
        address_constant_entries = NULL;
    }
    address_constant_capacity = 0;
    clearCurrentVTableTracker();
    tracked_vtable_chunk = NULL;
    if (vtable_tracker_stack) {
        for (int i = 0; i < vtable_tracker_depth; ++i) {
            if (vtable_tracker_stack[i].classes) {
                freeVTableClassList(vtable_tracker_stack[i].classes,
                                    vtable_tracker_stack[i].count);
            }
        }
        free(vtable_tracker_stack);
        vtable_tracker_stack = NULL;
    }
    vtable_tracker_depth = 0;
    vtable_tracker_capacity = 0;
    emitted_vtable_classes = NULL;
    emitted_vtable_count = 0;
    emitted_vtable_capacity = 0;
    if (pending_global_vtables) {
        free(pending_global_vtables);
        pending_global_vtables = NULL;
    }
    pending_global_vtable_count = 0;
    if (deferred_global_initializers) {
        free(deferred_global_initializers);
        deferred_global_initializers = NULL;
    }
    deferred_global_initializer_count = 0;
    deferred_global_initializer_capacity = 0;
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
            bool at_program_level = node->parent && node->parent->type == AST_PROGRAM;

            registerLabelDeclarations(declarations);

            if (declarations && declarations->type == AST_COMPOUND) {
                bool saved_postpone = postpone_global_initializers;
                if (at_program_level) postpone_global_initializers = true;
                // Pass 1: Compile type, constant, and variable declarations from the declaration block.
                for (int i = 0; i < declarations->child_count; i++) {
                    AST* decl_child = declarations->children[i];
                    if (!decl_child) continue;
                    if (decl_child->type == AST_COMPOUND) {
                        for (int j = 0; j < decl_child->child_count; j++) {
                            AST* nested = decl_child->children[j];
                            if (!nested) continue;
                            if (nested->type == AST_MODULE) {
                                compileNode(nested, chunk, getLine(nested));
                                continue;
                            }
                            if (nested->type == AST_VAR_DECL ||
                                nested->type == AST_CONST_DECL ||
                                nested->type == AST_TYPE_DECL) {
                                compileNode(nested, chunk, getLine(nested));
                            }
                        }
                        continue;
                    }
                    if (decl_child->type == AST_MODULE) {
                        compileNode(decl_child, chunk, getLine(decl_child));
                        continue;
                    }
                    if (decl_child->type == AST_VAR_DECL ||
                        decl_child->type == AST_CONST_DECL ||
                        decl_child->type == AST_TYPE_DECL) {
                        compileNode(decl_child, chunk, getLine(decl_child));
                    }
                }
                if (at_program_level) postpone_global_initializers = saved_postpone;
                if (compiler_had_error) {
                    break;
                }
                // Pass 2: Compile routines from the declaration block.
                for (int i = 0; i < declarations->child_count; i++) {
                    AST* decl_child = declarations->children[i];
                    if (!decl_child) continue;
                    if (decl_child->type == AST_COMPOUND) {
                        for (int j = 0; j < decl_child->child_count; j++) {
                            AST* nested = decl_child->children[j];
                            if (!nested) continue;
                            if (nested->type == AST_PROCEDURE_DECL || nested->type == AST_FUNCTION_DECL) {
                                compileNode(nested, chunk, getLine(nested));
                            }
                        }
                        continue;
                    }
                    if (decl_child->type == AST_PROCEDURE_DECL || decl_child->type == AST_FUNCTION_DECL) {
                        compileNode(decl_child, chunk, getLine(decl_child));
                    }
                }
            }

            if (at_program_level) {
                emitVTables(chunk);
                emitDeferredGlobalInitializers(chunk);
                for (int pg = 0; pg < pending_global_vtable_count; pg++) {
                    PendingGlobalVTableInit* p = &pending_global_vtables[pg];
                    int objNameIdx = addStringConstant(chunk, p->var_name);
                    emitGlobalNameIdx(chunk, GET_GLOBAL, GET_GLOBAL16, objNameIdx, 0);
                    writeBytecodeChunk(chunk, DUP, 0);
                    writeBytecodeChunk(chunk, GET_FIELD_OFFSET, 0);
                    writeBytecodeChunk(chunk, (uint8_t)0, 0);
                    char vtName[512];
                    snprintf(vtName, sizeof(vtName), "%s_vtable", p->class_name);
                    int vtIdx = addStringConstant(chunk, vtName);
                    emitGlobalNameIdx(chunk, GET_GLOBAL_ADDRESS, GET_GLOBAL_ADDRESS16, vtIdx, 0);
                    writeBytecodeChunk(chunk, SET_INDIRECT, 0);
                    writeBytecodeChunk(chunk, POP, 0);
                    free(p->var_name);
                    free(p->class_name);
                }
                free(pending_global_vtables);
                pending_global_vtables = NULL;
                pending_global_vtable_count = 0;
            }

            // Pass 3: Compile the main statement block.
            if (statements && statements->type == AST_COMPOUND) {
                for (int i = 0; i < statements->child_count; i++) {
                    if (statements->children[i]) {
                        compileStatement(statements->children[i], chunk,
                                         getLine(statements->children[i]));
                    }
                }
            }
            break;
        }
        case AST_VAR_DECL: {
            bool global_ctx = (current_function_compiler == NULL) &&
                              isGlobalScopeNode(node);
            if (node->child_count > 0 && node->children[0] && node->children[0]->token) {
                DBG_PRINTF("[dbg] VAR_DECL name=%s line=%d ctx=%s\n",
                        node->children[0]->token->value,
                        line,
                        global_ctx ? "global" : (current_function_compiler ? "local" : "unknown"));
            }

            if (global_ctx) { // Global variables
                AST* type_specifier_node = node->right;

                // First, resolve the type alias if one exists.
                AST* actual_type_def_node = type_specifier_node;
                if (actual_type_def_node && actual_type_def_node->type == AST_TYPE_REFERENCE) {
                    AST* resolved_node = lookupType(actual_type_def_node->token->value);
                    if (resolved_node) {
                        actual_type_def_node = resolved_node; // This now points to the AST_ARRAY_TYPE node
                    } else {
                        fprintf(stderr, "L%d: identifier '%s' not in scope.\n", getLine(actual_type_def_node), actual_type_def_node->token->value);
                        compiler_had_error = true;
                        break;
                    }
                }

                if (!actual_type_def_node) {
                    fprintf(stderr, "L%d: Compiler error: Could not determine type definition for a variable declaration.\n", getLine(node));
                    compiler_had_error = true;
                    break;
                }

                bool defer_initializer = postpone_global_initializers && node->left;
                if (defer_initializer) {
                    queueDeferredGlobalInitializer(node);
                    if (compiler_had_error) {
                        break;
                    }
                    for (int i = 0; i < node->child_count; i++) {
                        AST* varNameNode = node->children[i];
                        if (varNameNode && varNameNode->token) {
                            resolveGlobalVariableIndex(chunk,
                                                       varNameNode->token->value,
                                                       getLine(varNameNode));
                        }
                    }
                    break;
                }

                for (int i = 0; i < node->child_count; i++) {
                    AST* varNameNode = node->children[i];
                    if (!varNameNode || !varNameNode->token) continue;
                    emitGlobalVarDefinition(node,
                                            varNameNode,
                                            type_specifier_node,
                                            actual_type_def_node,
                                            chunk,
                                            node->left != NULL);
                }
            } else { // Local variables
                if (current_function_compiler != NULL) {
                    registerVarDeclLocals(node, false);
                }
                AST* type_specifier_node = node->right;

                // Resolve type alias if necessary
                AST* actual_type_def_node = type_specifier_node;
                if (actual_type_def_node && actual_type_def_node->type == AST_TYPE_REFERENCE) {
                    AST* resolved_node = lookupType(actual_type_def_node->token->value);
                    if (resolved_node) {
                        actual_type_def_node = resolved_node;
                    } else {
                        fprintf(stderr, "L%d: identifier '%s' not in scope.\n", getLine(actual_type_def_node), actual_type_def_node->token->value);
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

                    AST* resolved_local_type = resolveTypeAlias(actual_type_def_node);
                    if (resolved_local_type && resolved_local_type->type == AST_TYPE_DECL && resolved_local_type->left) {
                        resolved_local_type = resolveTypeAlias(resolved_local_type->left);
                    }
                    bool is_record_type = resolved_local_type && resolved_local_type->type == AST_RECORD_TYPE;

                    if (node->var_type == TYPE_ARRAY) {
                        int dimension_count = actual_type_def_node->child_count;
                        if (dimension_count > 255) {
                            fprintf(stderr, "L%d: Compiler error: Maximum array dimensions (255) exceeded.\n", getLine(varNameNode));
                            compiler_had_error = true;
                            break;
                        }

                        noteLocalSlotUse(current_function_compiler, slot);
                        writeBytecodeChunk(chunk, INIT_LOCAL_ARRAY, getLine(varNameNode));
                        writeBytecodeChunk(chunk, (uint8_t)slot, getLine(varNameNode));
                        writeBytecodeChunk(chunk, (uint8_t)dimension_count, getLine(varNameNode));

                        for (int dim = 0; dim < dimension_count; dim++) {
                            AST* subrange = actual_type_def_node->children[dim];
                            if (subrange && subrange->type == AST_SUBRANGE) {
                                Value lower_b = evaluateCompileTimeValue(subrange->left);
                                Value upper_b = evaluateCompileTimeValue(subrange->right);

                                if (IS_INTLIKE(lower_b)) {
                                    emitConstantIndex16(chunk, addIntConstant(chunk, AS_INTEGER(lower_b)), getLine(varNameNode));
                                } else {
                                    fprintf(stderr, "L%d: Compiler error: Array bound did not evaluate to a constant integer.\n", getLine(varNameNode));
                                    compiler_had_error = true;
                                }
                                freeValue(&lower_b);

                                if (IS_INTLIKE(upper_b)) {
                                    emitConstantIndex16(chunk, addIntConstant(chunk, AS_INTEGER(upper_b)), getLine(varNameNode));
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
                        emitConstantIndex16(chunk,
                                            addStringConstant(chunk, elem_type_name),
                                            getLine(varNameNode));
                    } else if (is_record_type) {
                        Value record_init = makeValueForType(TYPE_RECORD, resolved_local_type, NULL);
                        int const_idx = addConstantToChunk(chunk, &record_init);
                        freeValue(&record_init);
                        emitConstant(chunk, const_idx, getLine(varNameNode));
                        noteLocalSlotUse(current_function_compiler, slot);
                        writeBytecodeChunk(chunk, SET_LOCAL, getLine(varNameNode));
                        writeBytecodeChunk(chunk, (uint8_t)slot, getLine(varNameNode));
                    } else if (node->var_type == TYPE_STRING) {
                        int len = 0;
                        if (actual_type_def_node->right) {
                            Value len_val = evaluateCompileTimeValue(actual_type_def_node->right);
                            if (len_val.type == TYPE_INTEGER) {
                                len = (int)len_val.i_val;
                                if (len < 0 || len > 255) {
                                    fprintf(stderr, "L%d: Compiler error: Fixed string length out of range (0-255).\n", getLine(varNameNode));
                                    compiler_had_error = true;
                                    len = 0;
                                }
                            } else {
                                fprintf(stderr, "L%d: Compiler error: String length did not evaluate to a constant integer.\n", getLine(varNameNode));
                                compiler_had_error = true;
                            }
                            freeValue(&len_val);
                        }
                        noteLocalSlotUse(current_function_compiler, slot);
                        writeBytecodeChunk(chunk, INIT_LOCAL_STRING, getLine(varNameNode));
                        writeBytecodeChunk(chunk, (uint8_t)slot, getLine(varNameNode));
                        writeBytecodeChunk(chunk, (uint8_t)len, getLine(varNameNode));
                    } else if (node->var_type == TYPE_FILE) {
                        noteLocalSlotUse(current_function_compiler, slot);
                        writeBytecodeChunk(chunk, INIT_LOCAL_FILE, getLine(varNameNode));
                        writeBytecodeChunk(chunk, (uint8_t)slot, getLine(varNameNode));

                        VarType file_element_type = TYPE_VOID;
                        const char *file_element_name = "";
                        bool is_text_file = false;

                        AST *resolved_file_type = resolveTypeAlias(actual_type_def_node);
                        if (resolved_file_type && resolved_file_type->type == AST_TYPE_DECL && resolved_file_type->left) {
                            resolved_file_type = resolveTypeAlias(resolved_file_type->left);
                        }
                        if (resolved_file_type && resolved_file_type->type == AST_VAR_DECL && resolved_file_type->right) {
                            resolved_file_type = resolveTypeAlias(resolved_file_type->right);
                        }
                        if (resolved_file_type && resolved_file_type->type == AST_VARIABLE &&
                            resolved_file_type->token && resolved_file_type->token->value) {
                            const char *type_name = resolved_file_type->token->value;
                            if (strcasecmp(type_name, "file") == 0 && resolved_file_type->right) {
                                AST *element_node = resolveTypeAlias(resolved_file_type->right);
                                AST *source_node = element_node ? element_node : resolved_file_type->right;
                                if (source_node && source_node->var_type != TYPE_VOID && source_node->var_type != TYPE_UNKNOWN) {
                                    file_element_type = source_node->var_type;
                                }
                                if (source_node && source_node->token && source_node->token->value) {
                                    file_element_name = source_node->token->value;
                                }
                            } else if (strcasecmp(type_name, "text") == 0) {
                                is_text_file = true;
                                file_element_type = TYPE_VOID;
                                file_element_name = "";
                            }
                        }

                        writeBytecodeChunk(chunk, (uint8_t)file_element_type, getLine(varNameNode));
                        if (!is_text_file && file_element_name && file_element_name[0]) {
                            int type_name_index = addStringConstant(chunk, file_element_name);
                            emitConstantIndex16(chunk, type_name_index, getLine(varNameNode));
                        } else {
                            emitShort(chunk, 0xFFFF, getLine(varNameNode));
                        }
                    } else if (node->var_type == TYPE_POINTER) {
                        noteLocalSlotUse(current_function_compiler, slot);
                        writeBytecodeChunk(chunk, INIT_LOCAL_POINTER, getLine(varNameNode));
                        writeBytecodeChunk(chunk, (uint8_t)slot, getLine(varNameNode));

                        const char* type_name = "";
                        // Prefer the base type name for pointer types so the VM can resolve it.
                        AST* ptr_ast = type_specifier_node ? type_specifier_node : actual_type_def_node;
                        if (ptr_ast && ptr_ast->type == AST_POINTER_TYPE) {
                            AST* base = ptr_ast->right;
                            if (base && base->token && base->token->value) {
                                type_name = base->token->value;
                            } else if (ptr_ast->token && ptr_ast->token->value) {
                                type_name = ptr_ast->token->value;
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
                                maybeAutoBoxInterfaceForType(actual_type_def_node, node->left, chunk, getLine(node->left), true, false);
                            }
                        } else {
                            compileRValue(node->left, chunk, getLine(node->left));
                            maybeAutoBoxInterfaceForType(actual_type_def_node, node->left, chunk, getLine(node->left), true, false);
                        }
                        noteLocalSlotUse(current_function_compiler, slot);
                        writeBytecodeChunk(chunk, SET_LOCAL, getLine(varNameNode));
                        writeBytecodeChunk(chunk, (uint8_t)slot, getLine(varNameNode));
                    }
                }
            }
            break;
        }
        case AST_CONST_DECL: {
            if (!node->token) {
                break;
            }

            Value const_val = makeVoid();
            AST* type_specifier_node = node->right;
            AST* actual_type_def_node = type_specifier_node;
            if (actual_type_def_node && actual_type_def_node->type == AST_TYPE_REFERENCE) {
                AST* resolved = lookupType(actual_type_def_node->token->value);
                if (resolved) actual_type_def_node = resolved;
            }

            if (node->var_type == TYPE_ARRAY && node->left && node->left->type == AST_ARRAY_LITERAL && actual_type_def_node) {
                if (actual_type_def_node->type == AST_ARRAY_TYPE) {
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
                        VarType elem_type = elem_type_node ? elem_type_node->var_type : TYPE_UNKNOWN;
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

            if (const_val.type == TYPE_VOID || const_val.type == TYPE_UNKNOWN) {
                fprintf(stderr, "L%d: Constant '%s' must be compile-time evaluable.\n", line, node->token->value);
                compiler_had_error = true;
                freeValue(&const_val);
                break;
            }

            if (current_function_compiler == NULL) {
                DBG_PRINTF("[dbg] CONST_DECL name=%s line=%d ctx=global\n", node->token->value, line);
                if (constIsClassMember(node)) {
                    if (current_class_const_table) {
                        insertConstSymbolIn(current_class_const_table, node->token->value, const_val);
                    }
                } else {
                    insertGlobalSymbol(node->token->value, const_val.type, actual_type_def_node);
                    Symbol* sym = lookupGlobalSymbol(node->token->value);
                    if (sym && sym->value) {
                        freeValue(sym->value);
                        *(sym->value) = makeCopyOfValue(&const_val);
                        sym->is_const = true;
                    }
                    insertConstGlobalSymbol(node->token->value, const_val);
                }
            } else {
                AST* type_for_symbol = actual_type_def_node ? actual_type_def_node : type_specifier_node;
                Symbol* sym = insertLocalSymbol(node->token->value,
                                                const_val.type,
                                                type_for_symbol,
                                                false);
                if (sym && sym->value) {
                    freeValue(sym->value);
                    *(sym->value) = makeCopyOfValue(&const_val);
                    sym->is_const = true;
                }

                insertGlobalSymbol(node->token->value, const_val.type, actual_type_def_node);
                Symbol* global_sym = lookupGlobalSymbol(node->token->value);
                if (global_sym && global_sym->value) {
                    freeValue(global_sym->value);
                    *(global_sym->value) = makeCopyOfValue(&const_val);
                    global_sym->is_const = true;
                }
                insertConstGlobalSymbol(node->token->value, const_val);
            }

            freeValue(&const_val);
            break;
        }
        case AST_TYPE_DECL: {
            if (node->left && node->left->type == AST_RECORD_TYPE) {
                HashTable* saved_table = current_class_const_table;
                HashTable* tbl = NULL;
                if (node->left->symbol_table) {
                    tbl = (HashTable*)node->left->symbol_table;
                } else {
                    tbl = createHashTable();
                    node->left->symbol_table = (Symbol*)tbl;
                }
                current_class_const_table = tbl;
                for (int i = 0; i < node->left->child_count; i++) {
                    AST* member = node->left->children[i];
                    if (member && member->type == AST_CONST_DECL) {
                        compileNode(member, chunk, getLine(member));
                    }
                }
                current_class_const_table = saved_table;
            }
            break;
        }
        case AST_USES_CLAUSE:
            break;
        case AST_PROCEDURE_DECL:
        case AST_FUNCTION_DECL: {
            if (!node->token || !node->token->value || node->is_forward_decl) break;
            DBG_PRINTF("[dbg] compile decl %s\n", node->token->value);
            writeBytecodeChunk(chunk, JUMP, line);
            int jump_over_body_operand_offset = chunk->count;
            emitShort(chunk, 0xFFFF, line);
            compileDefinedFunction(node, chunk, line);
            uint16_t offset_to_skip_body = (uint16_t)(chunk->count - (jump_over_body_operand_offset + 2));
            patchShort(chunk, jump_over_body_operand_offset, offset_to_skip_body);
            break;
        }
        case AST_MODULE: {
            if (node->right) {
                compileNode(node->right, chunk, getLine(node->right));
            }
            break;
        }
        case AST_COMPOUND: {
            bool enters_scope = current_function_compiler != NULL && !node->is_global_scope;
            SymbolEnvSnapshot scope_snapshot;
            int starting_local = -1;
            if (enters_scope) {
                compilerBeginScope(current_function_compiler);
                starting_local = current_function_compiler->local_count;
                saveLocalEnv(&scope_snapshot);
            }
            // Pass 1: compile nested routine declarations so they are available
            // regardless of where they appear in the block.
            for (int i = 0; i < node->child_count; i++) {
                AST *child = node->children[i];
                if (!child) continue;
            if (child->type == AST_VAR_DECL) {
                registerVarDeclLocals(child, false);
                } else if (child->type == AST_MODULE) {
                    compileNode(child, chunk, getLine(child));
                    continue;
                }
                if (child->type == AST_PROCEDURE_DECL ||
                    child->type == AST_FUNCTION_DECL) {
                    compileNode(child, chunk, getLine(child));
                }
            }

            // Pass 2: compile executable statements in their original order,
            // skipping nested routine declarations which were already handled.
            for (int i = 0; i < node->child_count; i++) {
                AST *child = node->children[i];
                if (!child || child->type == AST_PROCEDURE_DECL ||
                    child->type == AST_FUNCTION_DECL) {
                    continue;
                }
                compileStatement(child, chunk, getLine(child));
            }
            if (enters_scope) {
                for (int i = current_function_compiler->local_count - 1; i >= starting_local; i--) {
                    if (current_function_compiler->locals[i].name) {
                        free(current_function_compiler->locals[i].name);
                        current_function_compiler->locals[i].name = NULL;
                    }
                }
                current_function_compiler->local_count = starting_local;
                compilerEndScope(current_function_compiler);
                restoreLocalEnv(&scope_snapshot);
            }
            break;
        }
        default:
            compileStatement(node, chunk, line);
            break;
    }
}

static void compileDefinedFunction(AST* func_decl_node, BytecodeChunk* chunk, int line) {
    SymbolEnvSnapshot env_snap;
    saveLocalEnv(&env_snap);

    FunctionCompilerState fc;
    initFunctionCompiler(&fc);
    fc.enclosing = current_function_compiler;
    current_function_compiler = &fc;

    FunctionCompilerState* outer_fc = fc.enclosing;
    const char* func_name = func_decl_node->token->value;
    int jump_over_body_operand_offset = -1;
    if (outer_fc != NULL) {
        writeBytecodeChunk(chunk, JUMP, line);
        jump_over_body_operand_offset = chunk->count;
        emitShort(chunk, 0xFFFF, line);
    }

    // --- FIX: Declare all variables at the top of the function ---
    int return_value_slot = -1;
    Symbol* proc_symbol = NULL;
    char name_for_lookup[MAX_SYMBOL_LENGTH * 2 + 2];
    AST* blockNode = NULL;

    fc.name = func_name;
    fc.returns_value = (func_decl_node->type == AST_FUNCTION_DECL);

    int func_bytecode_start_address = chunk->count;

    HashTable* saved_class_const_table = current_class_const_table;
    AST* saved_class_record_type = current_class_record_type;
    current_class_const_table = NULL;
    current_class_record_type = NULL;
    const char* dot_pos = strchr(func_name, '.');
    if (dot_pos) {
        size_t cls_len = (size_t)(dot_pos - func_name);
        if (cls_len < MAX_SYMBOL_LENGTH) {
            char cls_name[MAX_SYMBOL_LENGTH];
            strncpy(cls_name, func_name, cls_len);
            cls_name[cls_len] = '\0';
            AST* classType = lookupType(cls_name);
            if (classType) {
                AST* rec = NULL;
                if (classType->left && classType->left->type == AST_RECORD_TYPE) {
                    rec = classType->left;
                } else if (classType->type == AST_RECORD_TYPE) {
                    rec = classType;
                }
                if (rec) {
                    if (rec->symbol_table) current_class_const_table = (HashTable*)rec->symbol_table;
                    current_class_record_type = rec;
                }
            }
        }
    }

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
        /* In REA we allow implementations without prior interface declarations.
         * Materialize a symbol on the fly so the compiler can proceed. */
        if (ensureProcedureTableInitialized()) {
            proc_symbol = (Symbol *)calloc(1, sizeof(Symbol));
            if (proc_symbol) {
                proc_symbol->name = strdup(name_for_lookup);
                proc_symbol->type = func_decl_node->var_type;
                proc_symbol->type_def = copyAST(func_decl_node);
                Value *v = (Value *)calloc(1, sizeof(Value));
                if (v) {
                    v->type = TYPE_POINTER;
                    v->ptr_val = (Value *)func_decl_node;
                    proc_symbol->value = v;
                }
                hashTableInsert(procedure_table, proc_symbol);
            }
        }
    }

    if (!proc_symbol) {
        fprintf(stderr, "L%d: Compiler Error: Procedure implementation for '%s' (looked up as '%s') does not have a corresponding interface declaration.\n", line, func_name, name_for_lookup);
        compiler_had_error = true;
        current_class_const_table = saved_class_const_table;
        current_class_record_type = saved_class_record_type;
        current_function_compiler = NULL;
        restoreLocalEnv(&env_snap);
        return;
    }

    proc_symbol->bytecode_address = func_bytecode_start_address;
    proc_symbol->is_defined = true;
    fc.function_symbol = proc_symbol;
    proc_symbol->enclosing = fc.enclosing ? fc.enclosing->function_symbol : NULL;

    // Step 1: Add parameters to the local scope FIRST.
    if (func_decl_node->children) {
        for (int i = 0; i < func_decl_node->child_count; i++) {
            AST* param_group_node = func_decl_node->children[i];
            if (param_group_node && param_group_node->type == AST_VAR_DECL) {
                bool is_var_param = param_group_node->by_ref;
                AST* param_type_node = param_group_node->right ? param_group_node->right : param_group_node->type_def;
                VarType param_var_type = param_type_node ? param_type_node->var_type : TYPE_UNKNOWN;
                for (int j = 0; j < param_group_node->child_count; j++) {
                    AST* param_name_node = param_group_node->children[j];
                    if (param_name_node && param_name_node->token) {
                        addLocal(&fc, param_name_node->token->value, getLine(param_name_node), is_var_param);
                        insertLocalSymbol(param_name_node->token->value,
                                          param_var_type,
                                          param_type_node,
                                          true);
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
    if (blockNode) {
        int locals_before_decl_scan = fc.local_count;
        if (blockNode->type == AST_BLOCK && blockNode->child_count > 0 &&
            blockNode->children[0]->type == AST_COMPOUND) {
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
        } else if (compiler_dynamic_locals && blockNode->type == AST_COMPOUND) {
            for (int i = 0; i < blockNode->child_count; i++) {
                AST* child = blockNode->children[i];
                if (child && child->type == AST_VAR_DECL) {
                    for (int j = 0; j < child->child_count; j++) {
                        AST* var_name_node = child->children[j];
                        if (var_name_node && var_name_node->token) {
                            addLocal(&fc, var_name_node->token->value, getLine(var_name_node), false);
                        }
                    }
                }
            }
        }
        for (int i = locals_before_decl_scan; i < fc.local_count; i++) {
            fc.locals[i].depth = -1;
            fc.locals[i].decl_node = NULL;
        }
    }

    // Step 4: Compile the function body.
    LabelTableState routine_labels;
    initLabelTable(&routine_labels);

    HashTable *saved_table = current_procedure_table;
    if (func_decl_node->symbol_table) {
        current_procedure_table = (HashTable*)func_decl_node->symbol_table;
    }
    if (blockNode) {
        compileNode(blockNode, chunk, getLine(blockNode));
    }
    current_procedure_table = saved_table;

    finalizeLabelTable(&routine_labels, func_name ? func_name : "routine");

    updateMaxSlotFromBytecode(&fc, chunk, func_bytecode_start_address, chunk->count);

    int func_code_start = func_bytecode_start_address;
    int func_code_end = chunk->count;
    int func_code_len = func_code_end - func_code_start;
    if (func_code_len > 0) {
        bool* valid_offsets = (bool*)calloc((size_t)func_code_len, sizeof(bool));
        if (valid_offsets) {
            const char* func_name = (fc.function_symbol && fc.function_symbol->name)
                                        ? fc.function_symbol->name
                                        : (fc.name ? fc.name : "<anonymous>");
            bool instruction_scan_failed = false;
            int scan_offset = func_code_start;
            while (scan_offset < func_code_end) {
                valid_offsets[scan_offset - func_code_start] = true;
                int instr_len = getInstructionLength(chunk, scan_offset);
                if (instr_len <= 0 || scan_offset + instr_len > func_code_end) {
                    if (compiler_debug) {
                        DBG_PRINTF("[dbg] Invalid instruction length while validating jumps in '%s' at byte %d (len=%d).\n",
                                   func_name,
                                   scan_offset - func_code_start,
                                   instr_len);
                    }
                    fprintf(stderr,
                            "Compiler error: Invalid instruction layout while validating jumps in '%s'.\n",
                            func_name);
                    compiler_had_error = true;
#ifndef NDEBUG
                    assert(!"invalid instruction length during jump validation");
#endif
                    instruction_scan_failed = true;
                    break;
                }
                scan_offset += instr_len;
            }

            if (!instruction_scan_failed) {
                scan_offset = func_code_start;
                while (scan_offset < func_code_end) {
                    uint8_t opcode = chunk->code[scan_offset];
                    int instr_len = getInstructionLength(chunk, scan_offset);
                    bool is_jump = (opcode == JUMP || opcode == JUMP_IF_FALSE);
                    if (is_jump && scan_offset + instr_len <= func_code_end) {
                        int operand_idx = scan_offset + 1;
                        int16_t rel = (int16_t)((chunk->code[operand_idx] << 8) |
                                                chunk->code[operand_idx + 1]);
                        int instr_end = scan_offset + instr_len;
                        int dest = instr_end + rel;
                        bool dest_is_end = (dest == func_code_end);
                        bool dest_within_body = (dest >= func_code_start && dest < func_code_end);
                        if (dest_within_body || dest_is_end) {
                            if (!dest_is_end) {
                                bool marked_valid = valid_offsets[dest - func_code_start];
                                if (!marked_valid) {
                                    if (compiler_debug) {
                                        DBG_PRINTF(
                                            "[dbg] Invalid jump target %d discovered in '%s' at byte %d.\n",
                                            dest - func_code_start,
                                            func_name,
                                            scan_offset - func_code_start);
                                    }
                                    fprintf(stderr,
                                            "Compiler error: Jump at byte %d in '%s' targets invalid offset %d.\n",
                                            scan_offset - func_code_start,
                                            func_name,
                                            dest - func_code_start);
                                    compiler_had_error = true;
#ifndef NDEBUG
                                    assert(!"jump target offset not marked as valid");
#endif
                                }
                            }
                        } else {
                            if (compiler_debug) {
                                DBG_PRINTF(
                                    "[dbg] Jump at byte %d in '%s' targets out-of-range offset %d.\n",
                                    scan_offset - func_code_start,
                                    func_name,
                                    dest - func_code_start);
                            }
                            fprintf(stderr,
                                    "Compiler error: Jump at byte %d in '%s' targets out-of-range offset %d.\n",
                                    scan_offset - func_code_start,
                                    func_name,
                                    dest - func_code_start);
                            compiler_had_error = true;
#ifndef NDEBUG
                            assert(!"jump target offset outside of function bounds");
#endif
                        }
                    }
                    scan_offset += instr_len;
                }
            }

            free(valid_offsets);
        }
    }

    // Update locals_count in case new locals were declared during body compilation.
    int max_slots = fc.max_local_count;
    if (fc.max_slot_used > max_slots) {
        max_slots = fc.max_slot_used;
    }
    int effective_locals = max_slots - proc_symbol->arity;
    if (effective_locals < 0) {
        effective_locals = 0;
    }
    proc_symbol->locals_count = (uint16_t)effective_locals;

    // Step 5: Emit the return instruction.
    if (func_decl_node->type == AST_FUNCTION_DECL) {
        noteLocalSlotUse(&fc, return_value_slot);
        writeBytecodeChunk(chunk, GET_LOCAL, line);
        writeBytecodeChunk(chunk, (uint8_t)return_value_slot, line);
    }
    writeBytecodeChunk(chunk, RETURN, line);
    
    // Step 6: Cleanup.
    if (proc_symbol) {
        proc_symbol->upvalue_count = fc.upvalue_count;
        for (int i = 0; i < fc.upvalue_count; i++) {
            proc_symbol->upvalues[i].index = fc.upvalues[i].index;
            proc_symbol->upvalues[i].isLocal = fc.upvalues[i].isLocal;
            proc_symbol->upvalues[i].is_ref = fc.upvalues[i].is_ref;
        }
    }

    if (jump_over_body_operand_offset >= 0) {
        uint16_t offset_to_skip_body = (uint16_t)(chunk->count - (jump_over_body_operand_offset + 2));
        patchShort(chunk, jump_over_body_operand_offset, offset_to_skip_body);
    }

    for(int i = 0; i < fc.local_count; i++) {
        free(fc.locals[i].name);
    }
    current_class_const_table = saved_class_const_table;
    current_class_record_type = saved_class_record_type;
    current_function_compiler = fc.enclosing;
    restoreLocalEnv(&env_snap);
}

static void compileInlineRoutine(Symbol* proc_symbol, AST* call_node, BytecodeChunk* chunk, int line, bool push_result) {
    if (!proc_symbol || !proc_symbol->type_def) {
        // Fallback to normal call semantics handled by caller
        return;
    }

    AST* decl = proc_symbol->type_def;

    // If we're in the top-level program (no active FunctionCompilerState),
    // create a temporary one so the inliner can allocate locals and emit
    // GET_LOCAL/SET_LOCAL instructions as usual. This mirrors how
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
    bool saved_returns_value = current_function_compiler->returns_value;
    if (decl->type == AST_FUNCTION_DECL) {
        current_function_compiler->returns_value = true;
    }
    AST* blockNode = (decl->type == AST_PROCEDURE_DECL) ? decl->right : decl->extra;
    if (!blockNode) {
        current_function_compiler->returns_value = saved_returns_value;
        return;
    }

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
            noteLocalSlotUse(current_function_compiler, slot);
            writeBytecodeChunk(chunk, SET_LOCAL, line);
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

    LabelTableState inline_labels;
    initLabelTable(&inline_labels);

    HashTable* saved_table = current_procedure_table;
    if (decl->symbol_table) {
        current_procedure_table = (HashTable*)decl->symbol_table;
    }
    compileNode(blockNode, chunk, getLine(blockNode));
    current_procedure_table = saved_table;

    finalizeLabelTable(&inline_labels, proc_symbol->name ? proc_symbol->name : "inline routine");

    if (push_result && decl->type == AST_FUNCTION_DECL) {
        if (result_slot != -1) {
            noteLocalSlotUse(current_function_compiler, result_slot);
            writeBytecodeChunk(chunk, GET_LOCAL, line);
            writeBytecodeChunk(chunk, (uint8_t)result_slot, line);
        } else {
            emitConstant(chunk, addNilConstant(chunk), line);
        }
    }

    current_function_compiler->returns_value = saved_returns_value;

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

static void compilePrintf(AST* node, BytecodeChunk* chunk, int line) {
    if (!node) return;

    bool first_is_literal =
        node->child_count > 0 && node->children[0]->type == AST_STRING &&
        node->children[0]->token && node->children[0]->token->value;

    if (first_is_literal) {
        const char* fmt = node->children[0]->token->value;
        bool has_spec = false;
        for (size_t i = 0; fmt[i]; i++) {
            if (fmt[i] == '%') {
                if (fmt[i + 1] == '%') { i++; continue; }
                has_spec = true;
                break;
            }
        }

        if (!has_spec) {
            size_t flen = strlen(fmt);
            char* processed = (char*)malloc(flen + 1);
            size_t out = 0;
            for (size_t i = 0; i < flen; i++) {
                if (fmt[i] == '%' && fmt[i + 1] == '%') {
                    processed[out++] = '%';
                    i++;
                } else {
                    processed[out++] = fmt[i];
                }
            }
            processed[out] = '\0';

            Value sv = makeString(processed);
            int cidx = addConstantToChunk(chunk, &sv);
            freeValue(&sv);
            free(processed);

            Value nl = makeInt(0);
            int nlidx = addConstantToChunk(chunk, &nl);
            freeValue(&nl);
            emitConstant(chunk, nlidx, line);
            emitConstant(chunk, cidx, line);
            int write_arg_count = 2;

            for (int i = 1; i < node->child_count; i++) {
                AST* arg = node->children[i];
                compileRValue(arg, chunk, getLine(arg));
                write_arg_count++;
            }

            emitBuiltinProcedureCall(chunk, "write", (uint8_t)write_arg_count, line);

            Value zero = makeInt(0);
            int zidx = addConstantToChunk(chunk, &zero);
            freeValue(&zero);
            emitConstant(chunk, zidx, line);
            return;
        }
    }

    for (int i = 0; i < node->child_count; i++) {
        compileRValue(node->children[i], chunk, getLine(node->children[i]));
    }
    Value cnt = makeInt(node->child_count);
    int idx = addConstantToChunk(chunk, &cnt);
    freeValue(&cnt);
    emitConstant(chunk, idx, line);
    writeBytecodeChunk(chunk, CALL_HOST, line);
    writeBytecodeChunk(chunk, (uint8_t)HOST_FN_PRINTF, line);
}

static void compileStatement(AST* node, BytecodeChunk* chunk, int current_line_approx) {
    if (!node) return;
    int line = getLine(node);
    if (line <= 0) line = current_line_approx;

    switch (node->type) {
        case AST_NOOP:
            break;
        case AST_RETURN: {
            if (node->left) {
                compileRValue(node->left, chunk, getLine(node->left));
                AST* func_decl = (current_function_compiler && current_function_compiler->function_symbol)
                                     ? current_function_compiler->function_symbol->type_def
                                     : NULL;
                AST* return_type = NULL;
                if (func_decl && func_decl->type == AST_FUNCTION_DECL) {
                    return_type = func_decl->right;
                }
                maybeAutoBoxInterfaceForType(return_type, node->left, chunk, getLine(node->left), true, false);
            }
            writeBytecodeChunk(chunk, RETURN, line);
            break;
        }
        case AST_LABEL: {
            if (node->token) {
                defineLabel(node->token, chunk, line);
            }
            if (node->left) {
                compileStatement(node->left, chunk, getLine(node->left));
            }
            break;
        }
        case AST_GOTO: {
            compileGotoStatement(node, chunk, line);
            break;
        }
        case AST_CONTINUE: {
            addContinueJump(chunk, line);
            break;
        }
        case AST_BREAK: {
            addBreakJump(chunk, line);
            break;
        }
        case AST_THREAD_SPAWN: {
            compileRValue(node, chunk, line);
            writeBytecodeChunk(chunk, POP, line);
            break;
        }
        case AST_THREAD_JOIN: {
            if (node->left) {
                compileRValue(node->left, chunk, getLine(node->left));
            }
            writeBytecodeChunk(chunk, THREAD_JOIN, line);
            break;
        }
        case AST_EXPR_STMT: {
            if (node->left) {
                if (node->left->type == AST_PROCEDURE_CALL ||
                    node->left->type == AST_WRITE ||
                    node->left->type == AST_WRITELN) {
                    // Compile as a statement to avoid treating procedures as R-values
                    compileNode(node->left, chunk, getLine(node->left));
                } else {
                    compileRValue(node->left, chunk, getLine(node->left));
                    writeBytecodeChunk(chunk, POP, line);
                }
            }
            break;
        }
        case AST_VAR_DECL: {
            if (current_function_compiler != NULL) {
                registerVarDeclLocals(node, true);
            }
            /*
             * After registering locals (if any), delegate to compileNode so that
             * type-specific initialization opcodes (e.g. INIT_LOCAL_ARRAY) and
             * per-variable initializers are emitted for each declared variable.
             * This ensures both global and local variables receive proper
             * backing storage and zeroing before first use.
             */
            compileNode(node, chunk, line);
            break;
        }
        case AST_CONST_DECL: {
            // Local constant declarations do not emit bytecode but must be recorded
            // in the local symbol table so they can be referenced in subsequent
            // expressions within the same scope.
            if (current_function_compiler != NULL && node->token) {
                Value const_val = evaluateCompileTimeValue(node->left);
                AST *type_node = node->right ? node->right : node->left;
                Symbol *sym = insertLocalSymbol(node->token->value,
                                                const_val.type,
                                                type_node,
                                                false);
                if (sym && sym->value) {
                    freeValue(sym->value);
                    *(sym->value) = makeCopyOfValue(&const_val);
                    sym->is_const = true;
                }
                freeValue(&const_val);
            }
            break;
        }
        case AST_WRITELN: {
            int argCount = node->child_count;
            Value nl = makeInt(1);
            int nlidx = addConstantToChunk(chunk, &nl);
            freeValue(&nl);
            emitConstant(chunk, nlidx, line);
            for (int i = 0; i < argCount; i++) {
                compileRValue(node->children[i], chunk, getLine(node->children[i]));
            }
            emitBuiltinProcedureCall(chunk, "write", (uint8_t)(argCount + 1), line);
            break;
        }
        case AST_WHILE: {
            startLoop(chunk->count); // <<< MODIFIED: Mark loop start

            int loopStart = chunk->count;
            // In WHILE, 'continue' jumps to re-evaluate the condition
            loop_stack[loop_depth].continue_target = loopStart;

            compileRValue(node->left, chunk, line);

            writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
            int exitJumpOffset = chunk->count;
            emitShort(chunk, 0xFFFF, line);

            compileStatement(node->right, chunk, getLine(node->right));

            // All 'continue' statements in the body should jump to loopStart to re-evaluate condition
            patchContinuesTo(chunk, loopStart);

            writeBytecodeChunk(chunk, JUMP, line);
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

                bool share_branch_body = (num_labels > 1);
                int* match_jumps = NULL;
                int match_jumps_count = 0;

                // 3. For each label within the current branch.
                for (int j = 0; j < num_labels; j++) {
                    AST* label = labels_to_check[j];

                    writeBytecodeChunk(chunk, DUP, line);

                    if (label->type == AST_SUBRANGE) {
                        // Logic for range: (case_val >= lower) AND (case_val <= upper)
                        // This is a more direct and correct translation.

                        // Check lower bound
                        writeBytecodeChunk(chunk, DUP, line);                   // Stack: [case, case]
                        compileRValue(label->left, chunk, getLine(label));      // Stack: [case, case, lower]
                        writeBytecodeChunk(chunk, SWAP, line);                   // Stack: [case, lower, case]
                        writeBytecodeChunk(chunk, GREATER_EQUAL, line);          // Stack: [case, case, bool1]

                        // Check upper bound
                        writeBytecodeChunk(chunk, SWAP, line);                   // Stack: [case, bool1, case]
                        compileRValue(label->right, chunk, getLine(label));     // Stack: [case, bool1, case, upper]
                        writeBytecodeChunk(chunk, SWAP, line);                   // Stack: [case, bool1, upper, case]
                        writeBytecodeChunk(chunk, LESS_EQUAL, line);           // Stack: [case, bool1, bool2]

                        // Combine the two boolean results
                        writeBytecodeChunk(chunk, AND, line);                    // Stack: [case, final_bool]

                    } else {
                        // For single labels
                        compileRValue(label, chunk, getLine(label));
                        writeBytecodeChunk(chunk, EQUAL, line);                  // Stack: [case, bool]
                    }

                    // If the comparison is false, skip the branch body.
                    int false_jump = chunk->count;
                    writeBytecodeChunk(chunk, JUMP_IF_FALSE, line); emitShort(chunk, 0xFFFF, line);

                    // When the label matches, pop the case value.
                    writeBytecodeChunk(chunk, POP, line);

                    if (share_branch_body) {
                        // For multi-label branches, jump to the shared branch body.
                        int match_jump = chunk->count;
                        writeBytecodeChunk(chunk, JUMP, line); emitShort(chunk, 0xFFFF, line);

                        match_jumps = realloc(match_jumps, (match_jumps_count + 1) * sizeof(int));
                        match_jumps[match_jumps_count++] = match_jump;

                        // Patch the false jump to point to the next label check.
                        patchShort(chunk, false_jump + 1, chunk->count - (false_jump + 3));
                        fallthrough_jump = false_jump + 1;
                        continue;
                    }

                    // Single-label branches emit their body inline like the original implementation.
                    compileStatement(branch->right, chunk, getLine(branch->right));

                    // After body, jump to the end of the CASE.
                    end_jumps = realloc(end_jumps, (end_jumps_count + 1) * sizeof(int));
                    end_jumps[end_jumps_count++] = chunk->count;
                    writeBytecodeChunk(chunk, JUMP, line); emitShort(chunk, 0xFFFF, line);

                    // Patch the false jump to point to the next label / branch.
                    patchShort(chunk, false_jump + 1, chunk->count - (false_jump + 3));
                    fallthrough_jump = false_jump + 1;

                    // Move to the next CASE branch.
                    goto next_branch;
                }

                if (share_branch_body) {
                    int branch_body_start = chunk->count;
                    for (int j = 0; j < match_jumps_count; j++) {
                        patchShort(chunk, match_jumps[j] + 1, branch_body_start - (match_jumps[j] + 3));
                    }
                    if (match_jumps) free(match_jumps);

                    compileStatement(branch->right, chunk, getLine(branch->right));

                    // After body, jump to the end of the CASE.
                    end_jumps = realloc(end_jumps, (end_jumps_count + 1) * sizeof(int));
                    end_jumps[end_jumps_count++] = chunk->count;
                    writeBytecodeChunk(chunk, JUMP, line); emitShort(chunk, 0xFFFF, line);
                }

            next_branch:;
            }

            // After all branches, if an 'else' exists, compile it.
            if (fallthrough_jump != -1) {
                patchShort(chunk, fallthrough_jump, chunk->count - (fallthrough_jump + 2));
            }
            writeBytecodeChunk(chunk, POP, line); // Pop the case value if no branch was taken.
            
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

            // In REPEAT..UNTIL, 'continue' jumps to the condition check point (here)
            patchContinuesTo(chunk, chunk->count);

            if (node->right) {
                compileRValue(node->right, chunk, getLine(node->right));
            } else {
                int falseConstIdx = addBooleanConstant(chunk, false);
                emitConstant(chunk, falseConstIdx, line);
            }

            writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
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
            emitBuiltinProcedureCall(chunk, "read", (uint8_t)node->child_count, line);
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

            // Call the built-in `readln` procedure.
            emitBuiltinProcedureCall(chunk, "readln", (uint8_t)node->child_count, line);
            break;
        }
        case AST_WRITE: {
            int argCount = node->child_count;
            Value nl = makeInt(0);
            int nlidx = addConstantToChunk(chunk, &nl);
            freeValue(&nl);
            emitConstant(chunk, nlidx, line);
            for (int i = 0; i < argCount; i++) {
                compileRValue(node->children[i], chunk, getLine(node->children[i]));
            }
            emitBuiltinProcedureCall(chunk, "write", (uint8_t)(argCount + 1), line);
            break;
        }
        case AST_ASSIGN: {
            AST* lvalue = node->left;
            AST* rvalue = node->right;

            if (isIntlikeType(lvalue->var_type) && isIntlikeType(rvalue->var_type)) {
                int lrank = intTypeRank(lvalue->var_type);
                int rrank = intTypeRank(rvalue->var_type);
                if (rrank > lrank && !constantFitsInIntType(rvalue, lvalue->var_type)) {
                    fprintf(stderr, "L%d: Compiler warning: assigning %s to %s may lose precision.\n",
                            line, varTypeToString(rvalue->var_type), varTypeToString(lvalue->var_type));
                }
            }

            if (node->token && (node->token->type == TOKEN_PLUS || node->token->type == TOKEN_MINUS)) {
                compileLValue(lvalue, chunk, getLine(lvalue));
                writeBytecodeChunk(chunk, DUP, line);
                writeBytecodeChunk(chunk, GET_INDIRECT, line);
                compileRValue(rvalue, chunk, getLine(rvalue));
                if (node->token->type == TOKEN_PLUS) writeBytecodeChunk(chunk, ADD, line);
                else writeBytecodeChunk(chunk, SUBTRACT, line);
                writeBytecodeChunk(chunk, SET_INDIRECT, line);
            } else {
                compileRValue(rvalue, chunk, getLine(rvalue));
                maybeAutoBoxInterfaceForExpression(lvalue, rvalue, chunk, line, false);

                if (current_function_compiler && current_function_compiler->returns_value &&
                    current_function_compiler->name && lvalue->type == AST_VARIABLE &&
                    lvalue->token && lvalue->token->value &&
                    isCurrentFunctionResultIdentifier(current_function_compiler, lvalue->token->value)) {

                    int return_slot = resolveLocal(current_function_compiler, current_function_compiler->name);
                    if (return_slot != -1) {
                        noteLocalSlotUse(current_function_compiler, return_slot);
                        writeBytecodeChunk(chunk, SET_LOCAL, line);
                        writeBytecodeChunk(chunk, (uint8_t)return_slot, line);
                        // The POP instruction that was here has been removed.
                    } else {
                        fprintf(stderr, "L%d: Compiler internal error: could not resolve slot for function return value '%s'.\n", line, current_function_compiler->name);
                        compiler_had_error = true;
                    }
                } else {
                    int store_line = getLine(lvalue);
                    if (!emitDirectStoreForVariable(lvalue, chunk, store_line)) {
                        compileLValue(lvalue, chunk, store_line);
                        writeBytecodeChunk(chunk, SWAP, line);
                        writeBytecodeChunk(chunk, SET_INDIRECT, line);
                    }
                }
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
                DBG_PRINTF("[dbg] FOR var '%s' not local; treating as global. Locals: ", var_node && var_node->token ? var_node->token->value : "<nil>");
#ifdef DEBUG
                if (current_function_compiler) {
                    for (int li = 0; li < current_function_compiler->local_count; li++) {
                        const char* lname = current_function_compiler->locals[li].name;
                        if (!lname) continue;
                        fprintf(stderr, "%s%s", li==0?"":" ,", lname);
                    }
                    fprintf(stderr, "\n");
                }
#endif
                var_name_idx = addStringConstant(chunk, var_node->token->value);
            }

            // 1. Initial assignment of the loop variable
            compileRValue(start_node, chunk, getLine(start_node));
            if (var_slot != -1) {
                noteLocalSlotUse(current_function_compiler, var_slot);
                writeBytecodeChunk(chunk, SET_LOCAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_slot, line);
            } else {
                emitGlobalNameIdx(chunk, SET_GLOBAL, SET_GLOBAL16,
                                   var_name_idx, line);
            }

            // 2. Setup loop context for handling 'break'
            startLoop(-1); // Start address is not needed for FOR loop's break handling

            int loopStart = chunk->count;

            // 3. The loop condition check
            if (var_slot != -1) {
                noteLocalSlotUse(current_function_compiler, var_slot);
                writeBytecodeChunk(chunk, GET_LOCAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_slot, line);
            } else {
                emitGlobalNameIdx(chunk, GET_GLOBAL, GET_GLOBAL16,
                                   var_name_idx, line);
            }
            
            compileRValue(end_node, chunk, getLine(end_node));
            
            writeBytecodeChunk(chunk, is_downto ? GREATER_EQUAL : LESS_EQUAL, line);

            writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
            int exitJump = chunk->count;
            emitShort(chunk, 0xFFFF, line);

            // 4. Compile the loop body
            compileStatement(body_node, chunk, getLine(body_node));
            
            // 5. Increment/Decrement the loop variable
            // Any 'continue' in the body should land here (the post step), not at the condition.
            loop_stack[loop_depth].continue_target = chunk->count;
            patchContinuesTo(chunk, chunk->count);
            if (var_slot != -1) {
                noteLocalSlotUse(current_function_compiler, var_slot);
                writeBytecodeChunk(chunk, GET_LOCAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_slot, line);
            } else {
                emitGlobalNameIdx(chunk, GET_GLOBAL, GET_GLOBAL16,
                                   var_name_idx, line);
            }
            int one_const_idx = addIntConstant(chunk, 1);
            emitConstant(chunk, one_const_idx, line);
            writeBytecodeChunk(chunk, is_downto ? SUBTRACT : ADD, line);

            if (var_slot != -1) {
                noteLocalSlotUse(current_function_compiler, var_slot);
                writeBytecodeChunk(chunk, SET_LOCAL, line);
                writeBytecodeChunk(chunk, (uint8_t)var_slot, line);
            } else {
                emitGlobalNameIdx(chunk, SET_GLOBAL, SET_GLOBAL16,
                                   var_name_idx, line);
            }

            // The value from the increment/decrement is still on the stack.
            // Pop it to prevent stack overflow.
            //writeBytecodeChunk(chunk, POP, line);

            // 6. Jump back to the top of the loop to re-evaluate the condition
            writeBytecodeChunk(chunk, JUMP, line);
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
            writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
            emitShort(chunk, 0xFFFF, line);
            compileStatement(node->right, chunk, getLine(node->right));
            if (node->extra) {
                int jump_over_else_addr = chunk->count;
                writeBytecodeChunk(chunk, JUMP, line);
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
            bool treat_as_literal = false;
            if (node->child_count == 0 && node->token && node->token->value) {
                if (node->var_type == TYPE_POINTER) {
                    treat_as_literal = true;
                } else if (node->type_def &&
                           (node->type_def->type == AST_PROC_PTR_TYPE ||
                            (node->type_def->type == AST_TYPE_REFERENCE && node->type_def->right &&
                             node->type_def->right->type == AST_PROC_PTR_TYPE))) {
                    treat_as_literal = true;
                } else if (node->parent && node->parent->type == AST_ASSIGN && node->parent->left) {
                    AST *lhs = node->parent->left;
                    AST *lhsType = lhs->type_def;
                    if ((lhsType && (lhsType->type == AST_PROC_PTR_TYPE ||
                                     (lhsType->type == AST_TYPE_REFERENCE && lhsType->right &&
                                      lhsType->right->type == AST_PROC_PTR_TYPE))) ||
                        lhs->var_type == TYPE_POINTER) {
                        treat_as_literal = true;
                    }
                }
            }
            if (treat_as_literal) {
                Symbol *closure_sym = resolveProcedureSymbolInScope(node->token->value, node, gCurrentProgramRoot);
                if (closure_sym) {
                    if (!emitClosureLiteral(closure_sym, chunk, line)) {
                        break;
                    }
                    break;
                }
            }
            const char* calleeName = (node->token && node->token->value)
                                         ? node->token->value
                                         : NULL;
            const char* methodIdentifier = calleeName;
            bool isCallQualified = (node->left && node->token && node->token->value &&
                                    node->token->type == TOKEN_IDENTIFIER);
            bool usesReceiverGlobal = false;

            // --- NEW, MORE ROBUST LOOKUP LOGIC ---
            Symbol* proc_symbol_lookup = NULL;
            char callee_lower[MAX_SYMBOL_LENGTH];
            if (calleeName) {
                strncpy(callee_lower, calleeName, sizeof(callee_lower) - 1);
                callee_lower[sizeof(callee_lower) - 1] = '\0';
                toLowerString(callee_lower);
            } else {
                callee_lower[0] = '\0';
            }

            // First, try direct (unqualified) lookup
            if (callee_lower[0] != '\0') {
                proc_symbol_lookup = lookupProcedure(callee_lower);
            }

            // If it fails and we are inside a unit, try a qualified lookup
            if (!proc_symbol_lookup && current_compilation_unit_name && callee_lower[0] != '\0') {
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
            if (proc_symbol && proc_symbol->name) {
                calleeName = proc_symbol->name;   // ensure emitted call uses canonical lowercase
                methodIdentifier = proc_symbol->name;
            }

            if (!proc_symbol && callee_lower[0] != '\0') {
                const char* dot = strrchr(callee_lower, '.');
                if (dot && *(dot + 1)) {
                    Symbol* alt = lookupProcedure(dot + 1);
                    if (alt && alt->is_alias) {
                        alt = alt->real_symbol;
                    }
                    if (alt) {
                        proc_symbol = alt;
                        if (proc_symbol->name) {
                            calleeName = proc_symbol->name;
                            methodIdentifier = proc_symbol->name;
                        } else {
                            calleeName = dot + 1;
                            methodIdentifier = dot + 1;
                        }
                        if (node->child_count > 0) {
                            usesReceiverGlobal = true;
                        }
                    }
                }
            }

            if (!calleeName) {
                calleeName = "";
            }
            if (!methodIdentifier) {
                methodIdentifier = calleeName;
            }

            // Ensure the target procedure is compiled so its address is available
            if (proc_symbol && !proc_symbol->is_defined && proc_symbol->type_def &&
                !proc_symbol->type_def->is_forward_decl) {
                compileDefinedFunction(proc_symbol->type_def, chunk,
                                      getLine(proc_symbol->type_def));
            }

            AST* interfaceReceiver = NULL;
            AST* interfaceType = NULL;
            int interfaceArgStart = 0;

            if (isCallQualified) {
                if (node->child_count > 0) {
                    AST* candidate = node->children[0];
                    AST* candidateType = getInterfaceTypeFromExpression(candidate);
                    if (candidateType || candidate->var_type == TYPE_INTERFACE) {
                        interfaceReceiver = candidate;
                        interfaceType = candidateType;
                        interfaceArgStart = 1;
                    }
                }
                if (!interfaceReceiver && node->left) {
                    AST* candidateType = getInterfaceTypeFromExpression(node->left);
                    if (candidateType || node->left->var_type == TYPE_INTERFACE) {
                        interfaceReceiver = node->left;
                        interfaceType = candidateType;
                        interfaceArgStart = 0;

                        if (node->child_count > 0) {
                            AST* firstChild = node->children[0];
                            if (firstChild == interfaceReceiver ||
                                (firstChild && firstChild->type == AST_FIELD_ACCESS &&
                                 firstChild->left == interfaceReceiver)) {
                                interfaceArgStart = 1;
                            }
                        }
                    }
                }
            }

            bool isVirtualMethod = (isCallQualified && node->child_count > 0 && node->i_val == 0 &&
                                    proc_symbol && proc_symbol->type_def &&
                                    proc_symbol->type_def->is_virtual &&
                                    interfaceReceiver == NULL);
            bool isInterfaceDispatch = (isCallQualified && interfaceReceiver != NULL);

            int receiver_offset = (usesReceiverGlobal && node->child_count > 0) ? 1 : 0;

            if (frontendIsRea() && !proc_symbol && node->child_count > 0 && node->children[0]) {
                AST* recv = node->children[0];
                AST* tdef = recv->type_def;
                // Resolve TYPE_REFERENCE chain
                while (tdef && tdef->type == AST_TYPE_REFERENCE) tdef = tdef->right;
                const char* cls_name = NULL;
                if (tdef && tdef->token && tdef->token->value &&
                    (tdef->type == AST_TYPE_IDENTIFIER || tdef->type == AST_VARIABLE || tdef->type == AST_RECORD_TYPE)) {
                    // Prefer explicit type identifier token value
                    if (tdef->type == AST_TYPE_IDENTIFIER || tdef->type == AST_VARIABLE) {
                        cls_name = tdef->token->value;
                    } else if (recv->token && recv->token->value &&
                               (strcasecmp(recv->token->value, "myself") == 0 ||
                                strcasecmp(recv->token->value, "my") == 0) &&
                               current_function_compiler && current_function_compiler->function_symbol) {
                        // Derive from current function name 'Class.method' if available
                        const char* fname = current_function_compiler->function_symbol->name;
                        const char* dot = fname ? strchr(fname, '.') : NULL;
                        static char buf[256];
                        if (fname && dot && (dot - fname) < (int)sizeof(buf)) {
                            size_t n = (size_t)(dot - fname);
                            memcpy(buf, fname, n); buf[n] = '\0';
                            cls_name = buf;
                        }
                    }
                }
                if (cls_name && calleeName) {
                    size_t cls_len = strlen(cls_name);
                    size_t callee_len = strlen(calleeName);
                    bool alreadyQualified = false;
                    if (callee_len > cls_len &&
                        strncasecmp(calleeName, cls_name, cls_len) == 0 &&
                        calleeName[cls_len] == '.') {
                        alreadyQualified = true;
                    }

                    const char* lookup_name = calleeName;
                    char mangled[MAX_SYMBOL_LENGTH * 2 + 2];
                    if (!alreadyQualified) {
                        snprintf(mangled, sizeof(mangled), "%s.%s", cls_name, calleeName);
                        lookup_name = mangled;
                    }

                    char mangled_lower[MAX_SYMBOL_LENGTH * 2 + 2];
                    strncpy(mangled_lower, lookup_name, sizeof(mangled_lower) - 1);
                    mangled_lower[sizeof(mangled_lower) - 1] = '\0';
                    toLowerString(mangled_lower);
                    Symbol* m = lookupProcedure(mangled_lower);
                    if (m && m->is_alias) m = m->real_symbol;
                    if (m) {
                        proc_symbol = m;
                        calleeName = m->name; // use resolved name for emission
                    }
                }
            }

            if (strcasecmp(calleeName, "printf") == 0) {
                compilePrintf(node, chunk, line);
                writeBytecodeChunk(chunk, POP, line);
                break;
            }

            if (strcasecmp(calleeName, "lock") == 0) {
                if (node->child_count != 1) {
                    fprintf(stderr, "L%d: Compiler Error: lock expects 1 argument.\n", line);
                } else {
                    compileRValue(node->children[0], chunk, getLine(node->children[0]));
                }
                writeBytecodeChunk(chunk, MUTEX_LOCK, line);
                break;
            }
            if (strcasecmp(calleeName, "unlock") == 0) {
                if (node->child_count != 1) {
                    fprintf(stderr, "L%d: Compiler Error: unlock expects 1 argument.\n", line);
                } else {
                    compileRValue(node->children[0], chunk, getLine(node->children[0]));
                }
                writeBytecodeChunk(chunk, MUTEX_UNLOCK, line);
                break;
            }
            if (strcasecmp(calleeName, "destroy") == 0) {
                if (node->child_count != 1) {
                    fprintf(stderr, "L%d: Compiler Error: destroy expects 1 argument.\n", line);

                } else {
                    compileRValue(node->children[0], chunk, getLine(node->children[0]));
                }
                writeBytecodeChunk(chunk, MUTEX_DESTROY, line);
                break;
            }
            if (strcasecmp(calleeName, "mutex") == 0) {
                if (node->child_count != 0) {
                    fprintf(stderr, "L%d: Compiler Error: mutex expects no arguments.\n", line);
                }
                writeBytecodeChunk(chunk, MUTEX_CREATE, line);
                break;
            }
            if (strcasecmp(calleeName, "rcmutex") == 0) {
                if (node->child_count != 0) {
                    fprintf(stderr, "L%d: Compiler Error: rcmutex expects no arguments.\n", line);
                }
                writeBytecodeChunk(chunk, RCMUTEX_CREATE, line);
                break;
            }

            bool is_read_proc = (strcasecmp(calleeName, "read") == 0 || strcasecmp(calleeName, "readln") == 0);
            bool host_thread_helper = (strcasecmp(calleeName, "createthread") == 0 ||
                                       strcasecmp(calleeName, "waitforthread") == 0);
            bool callee_is_builtin = isBuiltin(calleeName) && !proc_symbol && !host_thread_helper;

            AST* procPtrSignature = NULL;
            AST* procPtrParams = NULL;
            if (!proc_symbol) {
                procPtrSignature = findProcPointerSignatureForCall(node);
                if (procPtrSignature && procPtrSignature->child_count > 0) {
                    procPtrParams = procPtrSignature->children[0];
                }
            }
            if (procPtrSignature) {
                is_read_proc = false;
                host_thread_helper = false;
                callee_is_builtin = false;
            }

            int arg_start = receiver_offset;
            int arg_count = node->child_count - receiver_offset;
            if (arg_count < 0) arg_count = 0;

            bool param_mismatch = false;
            if (!proc_symbol && procPtrSignature) {
                const char* pointerName = NULL;
                if (node->token && node->token->value) {
                    pointerName = node->token->value;
                } else if (calleeName && calleeName[0] != '\0') {
                    pointerName = calleeName;
                } else {
                    pointerName = "<procedure pointer>";
                }

                int expected = procPtrParams ? procPtrParams->child_count : 0;
                if (arg_count != expected) {
                    fprintf(stderr,
                            "L%d: Compiler Error: '%s' expects %d argument(s) but %d were provided.\n",
                            line, pointerName, expected, arg_count);
                    compiler_had_error = true;
                    param_mismatch = true;
                }

                if (!param_mismatch) {
                    for (int i = 0; i < expected; i++) {
                        AST* param_node = procPtrParams ? procPtrParams->children[i] : NULL;
                        AST* arg_node = node->children[i + arg_start];
                        if (!param_node || !arg_node) {
                            continue;
                        }

                        AST* param_type = getParameterTypeAST(param_node);
                        if (isInterfaceParameterNode(param_node, param_type)) {
                            continue;
                        }
                        bool match = typesMatch(param_type, arg_node, false);
                        if (!match) {
                            if (getInterfaceASTForParam(param_node, param_type)) {
                                continue;
                            }
                            AST* param_actual = resolveTypeAlias(param_type);
                            AST* arg_actual   = resolveTypeAlias(arg_node->type_def);
                            if (param_actual && param_actual->var_type == TYPE_INTERFACE) {
                                continue;
                            }
                            if (param_actual && arg_actual) {
                                if (param_actual->var_type == TYPE_ARRAY && arg_actual->var_type != TYPE_ARRAY) {
                                    fprintf(stderr,
                                            "L%d: Compiler Error: argument %lld to '%s' expects an array but got %s.\n",
                                            line, (long long)i + 1, pointerName,
                                            varTypeToString(arg_actual->var_type));
                                } else if (param_actual->var_type != TYPE_ARRAY && arg_actual->var_type == TYPE_ARRAY) {
                                    fprintf(stderr,
                                            "L%d: Compiler Error: argument %lld to '%s' expects %s but got an array.\n",
                                            line, (long long)i + 1, pointerName,
                                            varTypeToString(param_actual->var_type));
                                } else if (param_actual->var_type == TYPE_ARRAY && arg_actual->var_type == TYPE_ARRAY) {
                                    AST* param_elem = resolveTypeAlias(param_actual->right);
                                    AST* arg_elem   = resolveTypeAlias(arg_actual->right);
                                    const char* exp_str = param_elem ? varTypeToString(param_elem->var_type) : "UNKNOWN";
                                    const char* got_str = arg_elem ? varTypeToString(arg_elem->var_type) : "UNKNOWN";
                                    fprintf(stderr,
                                            "L%d: Compiler Error: argument %lld to '%s' expects type ARRAY OF %s but got ARRAY OF %s.\n",
                                            line, (long long)i + 1, pointerName,
                                            exp_str,
                                            got_str);
                                } else {
                                    fprintf(stderr,
                                            "L%d: Compiler Error: argument %lld to '%s' expects type %s but got %s.\n",
                                            line, (long long)i + 1, pointerName,
                                            varTypeToString(param_actual->var_type),
                                            varTypeToString(arg_actual->var_type));
                                }
                            } else {
                                VarType expected_vt = param_actual ? param_actual->var_type : param_type->var_type;
                                VarType actual_vt   = arg_actual ? arg_actual->var_type : arg_node->var_type;
                                fprintf(stderr,
                                        "L%d: Compiler Error: argument %lld to '%s' expects type %s but got %s.\n",
                                        line, (long long)i + 1, pointerName,
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
                                        "L%d: Compiler Error: argument %lld to '%s' must be a variable (VAR parameter).\n",
                                        line, (long long)i + 1, pointerName);
                                compiler_had_error = true;
                                param_mismatch = true;
                                break;
                            }
                        }
                    }
                }
            } else if (proc_symbol && proc_symbol->type_def) {
                int expected = proc_symbol->type_def->child_count;
                bool is_inc_dec = callee_is_builtin &&
                                   (strcasecmp(calleeName, "inc") == 0 || strcasecmp(calleeName, "dec") == 0);
                bool is_halt = callee_is_builtin && (strcasecmp(calleeName, "halt") == 0);
                if (expected == 0 && arg_count > 0) {
                    int idx = arg_start;
                    if (idx < node->child_count) {
                        AST* maybe_self = node->children[idx];
                        if (maybe_self && maybe_self->type == AST_VARIABLE &&
                            maybe_self->token && maybe_self->token->value &&
                            (strcasecmp(maybe_self->token->value, "myself") == 0 ||
                             strcasecmp(maybe_self->token->value, "my") == 0)) {
                            arg_start = idx + 1;
                            arg_count--;
                        }
                    }
                }
                if (is_inc_dec) {
                    if (!(arg_count == 1 || arg_count == 2)) {
                        fprintf(stderr, "L%d: Compiler Error: '%s' expects 1 or 2 argument(s) but %d were provided.\n",
                                line, calleeName, arg_count);
                        compiler_had_error = true;
                        param_mismatch = true;
                    }
                } else if (is_halt) {
                    if (!(arg_count == 0 || arg_count == 1)) {
                        fprintf(stderr, "L%d: Compiler Error: '%s' expects 0 or 1 argument(s) but %d were provided.\n",
                                line, calleeName, arg_count);
                        compiler_had_error = true;
                        param_mismatch = true;
                    }
                } else if (arg_count != expected) {
                    fprintf(stderr, "L%d: Compiler Error: '%s' expects %d argument(s) but %d were provided.\n",
                            line, calleeName, expected, arg_count);
                    compiler_had_error = true;
                    param_mismatch = true;
                }

            if (!param_mismatch) {
                for (int i = 0; i < arg_count; i++) {
                    AST* param_node = proc_symbol->type_def->children[i];
                    AST* arg_node = node->children[i + arg_start];
                    if (!param_node || !arg_node) continue;

                        // VAR parameters preserve their full TYPE_ARRAY node so that
                        // structural comparisons (like array bounds) remain possible.
                    AST* param_type = getParameterTypeAST(param_node);
                    if (isInterfaceParameterNode(param_node, param_type)) {
                        continue;
                    }
                    bool match = typesMatch(param_type, arg_node, callee_is_builtin);
                    if (!match) {
                        if (getInterfaceASTForParam(param_node, param_type)) {
                            continue;
                        }
                        AST* param_actual = resolveTypeAlias(param_type);
                        AST* arg_actual   = resolveTypeAlias(arg_node->type_def);
                            if (param_actual && param_actual->var_type == TYPE_INTERFACE) {
                                continue;
                            }
                            if (param_actual && arg_actual) {
                                if (param_actual->var_type == TYPE_ARRAY && arg_actual->var_type != TYPE_ARRAY) {
                                fprintf(stderr,
                                        "L%d: Compiler Error: argument %lld to '%s' expects an array but got %s.\n",
                                        line, (long long)i + 1, calleeName,
                                        varTypeToString(arg_actual->var_type));
                                } else if (param_actual->var_type != TYPE_ARRAY && arg_actual->var_type == TYPE_ARRAY) {
                                fprintf(stderr,
                                        "L%d: Compiler Error: argument %lld to '%s' expects %s but got an array.\n",
                                        line, (long long)i + 1, calleeName,
                                        varTypeToString(param_actual->var_type));
                                } else if (param_actual->var_type == TYPE_ARRAY && arg_actual->var_type == TYPE_ARRAY) {
                                    AST* param_elem = resolveTypeAlias(param_actual->right);
                                    AST* arg_elem   = resolveTypeAlias(arg_actual->right);
                                    const char* exp_str = param_elem ? varTypeToString(param_elem->var_type) : "UNKNOWN";
                                    const char* got_str = arg_elem ? varTypeToString(arg_elem->var_type) : "UNKNOWN";
                                fprintf(stderr,
                                        "L%d: Compiler Error: argument %lld to '%s' expects type ARRAY OF %s but got ARRAY OF %s.\n",
                                        line, (long long)i + 1, calleeName,
                                        exp_str,
                                        got_str);
                                } else {
                                fprintf(stderr,
                                        "L%d: Compiler Error: argument %lld to '%s' expects type %s but got %s.\n",
                                        line, (long long)i + 1, calleeName,
                                        varTypeToString(param_actual->var_type),
                                        varTypeToString(arg_actual->var_type));
                                }
                            } else {
                                VarType expected_vt = param_actual ? param_actual->var_type : param_type->var_type;
                                VarType actual_vt   = arg_actual ? arg_actual->var_type : arg_node->var_type;
                            fprintf(stderr,
                                    "L%d: Compiler Error: argument %lld to '%s' expects type %s but got %s.\n",
                                    line, (long long)i + 1, calleeName,
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
                                        "L%d: Compiler Error: argument %lld to '%s' must be a variable (VAR parameter).\n",
                                        line, (long long)i + 1, calleeName);
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

            int call_arg_count = node->child_count - receiver_offset;
            if (call_arg_count < 0) call_arg_count = 0;

            // Inline routine bodies directly when possible.
            if (proc_symbol && proc_symbol->type_def && proc_symbol->type_def->is_inline) {
                compileInlineRoutine(proc_symbol, node, chunk, line, false);
                break;
            }

            if (isVirtualMethod) {
                AST* recv = node->children[0];
                compileRValue(recv, chunk, getLine(recv));
                writeBytecodeChunk(chunk, DUP, line);
                for (int i = 1; i < node->child_count; i++) {
                    AST* arg_node = node->children[i];
                    bool is_var_param = false;
                    if (proc_symbol->type_def && i < proc_symbol->type_def->child_count) {
                        AST* param_node = proc_symbol->type_def->children[i];
                        if (param_node && param_node->by_ref) is_var_param = true;
                    }
                    if (is_var_param) {
                        compileLValue(arg_node, chunk, getLine(arg_node));
                    } else {
                        compileRValue(arg_node, chunk, getLine(arg_node));
                    }
                    writeBytecodeChunk(chunk, SWAP, line);
                }
                writeBytecodeChunk(chunk, GET_FIELD_OFFSET, line);
                writeBytecodeChunk(chunk, (uint8_t)0, line);
                writeBytecodeChunk(chunk, GET_INDIRECT, line);
                emitConstant(chunk, addIntConstant(chunk, proc_symbol->type_def->i_val), line);
                writeBytecodeChunk(chunk, SWAP, line);
                writeBytecodeChunk(chunk, GET_ELEMENT_ADDRESS, line);
                writeBytecodeChunk(chunk, (uint8_t)1, line);
                writeBytecodeChunk(chunk, GET_INDIRECT, line);
                writeBytecodeChunk(chunk, PROC_CALL_INDIRECT, line);
                writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
                break;
            }

            if (isInterfaceDispatch) {
                const char* slotName = methodIdentifier ? methodIdentifier : calleeName;
                AST* methodSignature = (proc_symbol && proc_symbol->type_def)
                                           ? proc_symbol->type_def
                                           : NULL;

                if (!methodSignature && interfaceType) {
                    for (int i = 0; i < interfaceType->child_count; i++) {
                        AST* child = interfaceType->children[i];
                        if (!child) continue;
                        if (child->token && child->token->value &&
                            strcasecmp(child->token->value, slotName) == 0 &&
                            (child->type == AST_PROCEDURE_DECL || child->type == AST_FUNCTION_DECL)) {
                            methodSignature = child;
                            break;
                        }
                    }
                }

                int method_slot = methodSignature ? methodSignature->i_val : -1;
                if (interfaceType) {
                    int resolved_slot = ensureInterfaceMethodSlot(interfaceType, slotName);
                    if (resolved_slot >= 0) {
                        method_slot = resolved_slot;
                        if (proc_symbol && proc_symbol->type_def) {
                            proc_symbol->type_def->i_val = method_slot;
                        } else if (methodSignature) {
                            methodSignature->i_val = method_slot;
                        }
                    }
                }

                if (method_slot < 0) {
                    fprintf(stderr,
                            "L%d: Compiler Error: Interface method '%s' missing slot index.\n",
                            line, calleeName ? calleeName : "<anonymous>");
                    compiler_had_error = true;
                    break;
                }

                AST* recv = interfaceReceiver;
                compileRValue(recv, chunk, getLine(recv));
                int slotConst = addIntConstant(chunk, method_slot);
                emitConstant(chunk, slotConst, line);
                writeBytecodeChunk(chunk, CALL_HOST, line);
                writeBytecodeChunk(chunk, (uint8_t)HOST_FN_INTERFACE_LOOKUP, line);

                MethodParameter* ifaceParams = NULL;
                int ifaceParamCount = 0;
                bool haveParamMetadata = false;
                if (methodSignature &&
                    (methodSignature->type == AST_PROCEDURE_DECL ||
                     methodSignature->type == AST_FUNCTION_DECL)) {
                    if (buildMethodParameterList(methodSignature, &ifaceParams, &ifaceParamCount)) {
                        haveParamMetadata = true;
                    } else {
                        ifaceParams = NULL;
                        ifaceParamCount = 0;
                    }
                }

                for (int i = interfaceArgStart; i < node->child_count; i++) {
                    AST* arg_node = node->children[i];
                    bool is_var_param = false;
                    AST* param_type_hint = NULL;

                    if (haveParamMetadata) {
                        int arg_index = i - interfaceArgStart;
                        if (arg_index >= 0 && arg_index < ifaceParamCount) {
                            AST* paramGroup = ifaceParams[arg_index].group;
                            if (paramGroup && paramGroup->by_ref) {
                                is_var_param = true;
                            }
                            if (paramGroup) {
                                param_type_hint = paramGroup->right ? paramGroup->right : paramGroup;
                            }
                        }
                    } else if (methodSignature) {
                        int metadataOffset = 1 - interfaceArgStart;
                        if (metadataOffset < 0) metadataOffset = 0;
                        int meta_index = i + metadataOffset;
                        if (meta_index >= 0 && meta_index < methodSignature->child_count) {
                            AST* param_node = methodSignature->children[meta_index];
                            if (param_node && param_node->by_ref) {
                                is_var_param = true;
                            }
                            if (param_node) {
                                param_type_hint = param_node->type_def ? param_node->type_def
                                                                      : (param_node->right ? param_node->right : param_node);
                            }
                        }
                    }

                    if (is_var_param) {
                        compileLValue(arg_node, chunk, getLine(arg_node));
                    } else {
                        compileRValue(arg_node, chunk, getLine(arg_node));
                        maybeAutoBoxInterfaceForType(param_type_hint, arg_node, chunk, getLine(arg_node), true, false);
                    }
                    writeBytecodeChunk(chunk, SWAP, line);
                }

                if (haveParamMetadata) {
                    free(ifaceParams);
                }

                writeBytecodeChunk(chunk, PROC_CALL_INDIRECT, line);
                int total_args = node->child_count - interfaceArgStart;
                if (total_args < 0) total_args = 0;
                writeBytecodeChunk(chunk, (uint8_t)total_args, line);
                break;
            }

            // (Argument compilation logic remains the same...)
            if (usesReceiverGlobal && node->child_count > 0) {
                int myself_idx = ensureMyselfGlobalNameIndex(chunk);
                AST* recv_node = node->children[0];
                compileRValue(recv_node, chunk, getLine(recv_node));
                emitGlobalNameIdx(chunk, SET_GLOBAL, SET_GLOBAL16, myself_idx, line);
            }

            for (int i = receiver_offset; i < node->child_count; i++) {
                AST* arg_node = node->children[i];
                int param_index = i - receiver_offset;
                bool is_var_param = false;
                AST* param_type_hint = NULL;
                if (is_read_proc && (param_index > 0 || (param_index == 0 && arg_node->var_type != TYPE_FILE))) {
                    is_var_param = true;
                }
                else if (calleeName && (
                    (param_index == 0 && (strcasecmp(calleeName, "new") == 0 || strcasecmp(calleeName, "dispose") == 0 || strcasecmp(calleeName, "assign") == 0 || strcasecmp(calleeName, "reset") == 0 || strcasecmp(calleeName, "rewrite") == 0 || strcasecmp(calleeName, "append") == 0 || strcasecmp(calleeName, "close") == 0 || strcasecmp(calleeName, "rename") == 0 || strcasecmp(calleeName, "erase") == 0 || strcasecmp(calleeName, "inc") == 0 || strcasecmp(calleeName, "dec") == 0 || strcasecmp(calleeName, "setlength") == 0 || strcasecmp(calleeName, "mstreamloadfromfile") == 0 || strcasecmp(calleeName, "mstreamsavetofile") == 0 || strcasecmp(calleeName, "mstreamfree") == 0 || strcasecmp(calleeName, "eof") == 0 || strcasecmp(calleeName, "readkey") == 0)) ||
                    (strcasecmp(calleeName, "readln") == 0 && (param_index > 0 || (param_index == 0 && arg_node->var_type != TYPE_FILE))) ||
                    (strcasecmp(calleeName, "getmousestate") == 0) || // All params are VAR
                    (strcasecmp(calleeName, "getscreensize") == 0 && param_index <= 1) || // First two parameters are VAR
                    (strcasecmp(calleeName, "gettextsize") == 0 && param_index > 0) || // Width and Height are VAR
                    (strcasecmp(calleeName, "str") == 0 && param_index == 1) ||
                    /* Date/time routines return values via VAR parameters */
                    (strcasecmp(calleeName, "dosgetdate") == 0) ||
                    (strcasecmp(calleeName, "dosgettime") == 0) ||
                    (strcasecmp(calleeName, "getdate") == 0) ||
                    (strcasecmp(calleeName, "gettime") == 0) ||
                    /* MandelbrotRow returns results via the sixth VAR parameter */
                    ((strcasecmp(calleeName, "mandelbrotrow") == 0 ||
                      strcasecmp(calleeName, "MandelbrotRow") == 0) && param_index == 5) ||
                    /* BouncingBalls3D builtins write simulation data back via VAR arrays */
                    ((strcasecmp(calleeName, "bouncingballs3dstep") == 0 ||
                      strcasecmp(calleeName, "BouncingBalls3DStep") == 0) && param_index >= 12) ||
                    ((strcasecmp(calleeName, "bouncingballs3dstepultra") == 0 ||
                      strcasecmp(calleeName, "BouncingBalls3DStepUltra") == 0) && param_index >= 12) ||
                    ((strcasecmp(calleeName, "bouncingballs3dstepadvanced") == 0 ||
                      strcasecmp(calleeName, "BouncingBalls3DStepAdvanced") == 0) && param_index >= 15) ||
                    ((strcasecmp(calleeName, "bouncingballs3dstepultraadvanced") == 0 ||
                      strcasecmp(calleeName, "BouncingBalls3DStepUltraAdvanced") == 0) && param_index >= 15)
                    || ((strcasecmp(calleeName, "bouncingballs3daccelerate") == 0 ||
                         strcasecmp(calleeName, "BouncingBalls3DAccelerate") == 0) && param_index <= 5)
                )) {
                    is_var_param = true;
                }
                else if (!proc_symbol && procPtrParams && param_index < procPtrParams->child_count) {
                    AST* param_node = procPtrParams->children[param_index];
                    if (param_node && param_node->by_ref) {
                        is_var_param = true;
                    }
                    if (param_node) {
                        param_type_hint = param_node->right ? param_node->right : param_node;
                    }
                }
                else if (proc_symbol && proc_symbol->type_def && param_index < proc_symbol->type_def->child_count) {
                    AST* param_node = proc_symbol->type_def->children[param_index];
                    if (param_node && param_node->by_ref) {
                        is_var_param = true;
                    }
                    if (param_node) {
                        param_type_hint = param_node->type_def ? param_node->type_def
                                                               : (param_node->right ? param_node->right : param_node);
                    }
                }

                if (is_var_param) {
                    compileLValue(arg_node, chunk, getLine(arg_node));
                } else {
                    compileRValue(arg_node, chunk, getLine(arg_node));
                    maybeAutoBoxInterfaceForType(param_type_hint, arg_node, chunk, getLine(arg_node), true, false);
                }
            }


            if (callee_is_builtin) {
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
                        noteLocalSlotUse(current_function_compiler, slot);
                        writeBytecodeChunk(chunk, GET_LOCAL, line);
                        writeBytecodeChunk(chunk, (uint8_t)slot, line);
                    }
                    writeBytecodeChunk(chunk, EXIT, line);
                } else {
                    BuiltinRoutineType type = getBuiltinType(calleeName);
                    if (type == BUILTIN_TYPE_PROCEDURE) {
                        emitBuiltinProcedureCall(chunk, calleeName, (uint8_t)call_arg_count, line);
                    } else if (type == BUILTIN_TYPE_FUNCTION) {
                        int nameIndex = ensureBuiltinStringConstants(chunk, calleeName, NULL);
                        writeBytecodeChunk(chunk, CALL_BUILTIN, line);
                        emitShort(chunk, (uint16_t)nameIndex, line);
                        writeBytecodeChunk(chunk, (uint8_t)call_arg_count, line);
                        writeBytecodeChunk(chunk, POP, line);
                    } else {
                        // This case handles if a name is in the isBuiltin list but not in getBuiltinType,
                        // which would be an internal inconsistency.
                        fprintf(stderr, "L%d: Compiler Error: '%s' is not a recognized built-in procedure or function.\n", line, calleeName);
                        compiler_had_error = true;
                    }
                }
            } else if (proc_symbol) { // If a symbol was found (either defined or forward-declared)
                int nameIndex = addStringConstant(chunk, calleeName);
                writeBytecodeChunk(chunk, CALL_USER_PROC, line);
                emitShort(chunk, (uint16_t)nameIndex, line);
                writeBytecodeChunk(chunk, (uint8_t)call_arg_count, line);

                // This logic for user-defined functions is already correct.
                if (proc_symbol->type != TYPE_VOID) {
                    writeBytecodeChunk(chunk, POP, line);
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
                    writeBytecodeChunk(chunk, CALL_HOST, line);
                    writeBytecodeChunk(chunk, (uint8_t)HOST_FN_CREATE_THREAD_ADDR, line);
                } else if (strcasecmp(calleeName, "waitforthread") == 0) {
                    if (node->child_count != 1) {
                        fprintf(stderr, "L%d: Compiler Error: WaitForThread expects 1 argument (thread id).\n", line);
                    } else {
                        compileRValue(node->children[0], chunk, getLine(node->children[0]));
                    }
                    writeBytecodeChunk(chunk, CALL_HOST, line);
                    writeBytecodeChunk(chunk, (uint8_t)HOST_FN_WAIT_THREAD, line);
                } else {
                    // Indirect call through a procedure pointer expression: arguments are already on stack.
                    AST calleeExpr;
                    memset(&calleeExpr, 0, sizeof(AST));
                    AST *exprToCompile = NULL;

                    if (node->left) {
                        if (node->token) {
                            calleeExpr.type = AST_FIELD_ACCESS;
                            calleeExpr.token = node->token;
                            calleeExpr.left = node->left;
                            calleeExpr.var_type = node->var_type;
                            calleeExpr.type_def = node->type_def;
                            exprToCompile = &calleeExpr;
                        } else {
                            exprToCompile = node->left;
                        }
                    } else {
                        calleeExpr.type = AST_VARIABLE;
                        calleeExpr.token = node->token;
                        calleeExpr.var_type = node->var_type;
                        calleeExpr.type_def = node->type_def;
                        exprToCompile = &calleeExpr;
                    }

                    if (!exprToCompile) {
                        fprintf(stderr, "L%d: Compiler error: Unable to resolve procedure pointer call target.\n", line);
                        compiler_had_error = true;
                        break;
                    }

                    AST *savedParent = NULL;
                    if (exprToCompile == &calleeExpr && calleeExpr.left) {
                        savedParent = calleeExpr.left->parent;
                        calleeExpr.left->parent = &calleeExpr;
                    }

                    compileRValue(exprToCompile, chunk, line);

                    if (savedParent) {
                        calleeExpr.left->parent = savedParent;
                    }

                    writeBytecodeChunk(chunk, PROC_CALL_INDIRECT, line);
                    writeBytecodeChunk(chunk, (uint8_t)call_arg_count, line);
                }
            }
            break;
        }
        case AST_COMPOUND: {
            bool enters_scope = current_function_compiler != NULL && !node->is_global_scope;
            SymbolEnvSnapshot scope_snapshot;
            int starting_local = -1;
            if (enters_scope) {
                compilerBeginScope(current_function_compiler);
                starting_local = current_function_compiler->local_count;
                saveLocalEnv(&scope_snapshot);
            }
            for (int i = 0; i < node->child_count; i++) {
                if (node->children[i]) {
                    compileStatement(node->children[i], chunk, getLine(node->children[i]));
                }
            }
            if (enters_scope) {
                for (int i = current_function_compiler->local_count - 1; i >= starting_local; i--) {
                    if (current_function_compiler->locals[i].name) {
                        free(current_function_compiler->locals[i].name);
                        current_function_compiler->locals[i].name = NULL;
                    }
                }
                current_function_compiler->local_count = starting_local;
                compilerEndScope(current_function_compiler);
                restoreLocalEnv(&scope_snapshot);
            }
            break;
        }
        case AST_USES_CLAUSE:
            // Uses clauses are processed by the REA front end and don't emit code
            break;
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

static bool lookupEnumMemberOrdinal(const char* name, long long* outOrdinal) {
    if (!name || !outOrdinal) return false;

    for (TypeEntry* entry = type_table; entry; entry = entry->next) {
        if (!entry->typeAST) continue;

        AST* enum_ast = entry->typeAST;
        if (enum_ast->type == AST_TYPE_REFERENCE && enum_ast->right) {
            enum_ast = enum_ast->right;
        }

        if (!enum_ast || enum_ast->type != AST_ENUM_TYPE) continue;

        for (int i = 0; i < enum_ast->child_count; i++) {
            AST* value_node = enum_ast->children[i];
            if (!value_node || !value_node->token || !value_node->token->value) {
                continue;
            }
            if (strcasecmp(value_node->token->value, name) == 0) {
                *outOrdinal = value_node->i_val;
                return true;
            }
        }
    }

    return false;
}

static bool resolveSetElementOrdinal(AST* member, long long* outOrdinal) {
    if (!member || !outOrdinal) return false;

    bool success = false;
    long long ordinal = 0;

    Value elem_val = evaluateCompileTimeValue(member);
    switch (elem_val.type) {
        case TYPE_INTEGER:
            ordinal = elem_val.i_val;
            success = true;
            break;
        case TYPE_CHAR:
            ordinal = elem_val.c_val;
            success = true;
            break;
        case TYPE_ENUM:
            ordinal = elem_val.enum_val.ordinal;
            success = true;
            break;
        default:
            break;
    }
    freeValue(&elem_val);

    if (success) {
        *outOrdinal = ordinal;
        return true;
    }

    if (member->type == AST_VARIABLE && member->token && member->token->value) {
        const char* name = member->token->value;
        Symbol* sym = lookupLocalSymbol(name);
        if (!sym) {
            sym = lookupGlobalSymbol(name);
        }
        sym = resolveSymbolAlias(sym);
        if (sym && sym->value && sym->is_const) {
            Value* v = sym->value;
            if (v->type == TYPE_ENUM) {
                *outOrdinal = v->enum_val.ordinal;
                return true;
            }
            if (v->type == TYPE_INTEGER) {
                *outOrdinal = v->i_val;
                return true;
            }
            if (v->type == TYPE_CHAR) {
                *outOrdinal = v->c_val;
                return true;
            }
        }
        if (lookupEnumMemberOrdinal(name, outOrdinal)) {
            return true;
        }
    }

    return false;
}

static void compileRValue(AST* node, BytecodeChunk* chunk, int current_line_approx) {
    if (!node) return;
    int line = getLine(node);
    if (line <= 0) line = current_line_approx;

    Token* node_token = node->token;

    switch (node->type) {
        case AST_NEW: {
            if (!node || !node->token || !node->token->value) { break; }
            const char* className = node->token->value;
            char lowerClassName[MAX_SYMBOL_LENGTH];
            strncpy(lowerClassName, className, sizeof(lowerClassName) - 1);
            lowerClassName[sizeof(lowerClassName) - 1] = '\0';
            toLowerString(lowerClassName);
            AST* classType = lookupType(lowerClassName);

            bool hasVTable = recordTypeHasVTable(classType);
            int fieldCount = getRecordFieldCount(classType) + (hasVTable ? 1 : 0);

            global_init_new_depth++;
            bool defer_vtable = compiling_global_var_init && global_init_new_depth == 1;

            if (fieldCount <= 0xFF) {
                writeBytecodeChunk(chunk, ALLOC_OBJECT, line);
                writeBytecodeChunk(chunk, (uint8_t)fieldCount, line);
            } else {
                writeBytecodeChunk(chunk, ALLOC_OBJECT16, line);
                emitShort(chunk, (uint16_t)fieldCount, line);
            }

            if (hasVTable) {
                if (defer_vtable) {
                    // See compileLValue(): global initialisers still require the vtable
                    // immediately so constructors can dispatch virtual calls safely.
                }
                // Store class vtable pointer into hidden __vtable field (offset 0)
                writeBytecodeChunk(chunk, DUP, line);
                writeBytecodeChunk(chunk, GET_FIELD_OFFSET, line);
                writeBytecodeChunk(chunk, (uint8_t)0, line);
                char vtName[512];
                snprintf(vtName, sizeof(vtName), "%s_vtable", lowerClassName);
                int vtNameIdx = addStringConstant(chunk, vtName);
                emitGlobalNameIdx(chunk, GET_GLOBAL_ADDRESS, GET_GLOBAL_ADDRESS16, vtNameIdx, line);
                writeBytecodeChunk(chunk, SET_INDIRECT, line);
            }

            emitArrayFieldInitializers(classType, chunk, line, hasVTable);

            Symbol* ctorSymbol = lookupProcedure(lowerClassName);
            Symbol* resolvedCtor = resolveSymbolAlias(ctorSymbol);
            const char* ctorLookupName = lowerClassName;
            if (resolvedCtor && resolvedCtor->name) {
                ctorLookupName = resolvedCtor->name;
            } else if (ctorSymbol && ctorSymbol->name) {
                ctorLookupName = ctorSymbol->name;
            }

            if (resolvedCtor || ctorSymbol || node->child_count > 0) {
                writeBytecodeChunk(chunk, DUP, line);
                for (int i = 0; i < node->child_count; i++) {
                    compileRValue(node->children[i], chunk, getLine(node->children[i]));
                }
                int ctorNameIdx = addStringConstant(chunk, ctorLookupName);
                writeBytecodeChunk(chunk, CALL_USER_PROC, line);
                emitShort(chunk, (uint16_t)ctorNameIdx, line);
                writeBytecodeChunk(chunk, (uint8_t)(node->child_count + 1), line);
            }
            global_init_new_depth--;
            break;
        }
        case AST_TYPE_ASSERT: {
            if (!node->left || !node->right) {
                fprintf(stderr, "L%d: Compiler Error: Type assertion requires an expression and a target type.\n", line);
                compiler_had_error = true;
                emitConstant(chunk, addNilConstant(chunk), line);
                break;
            }

            compileRValue(node->left, chunk, getLine(node->left));

            AST* targetRef = node->right;
            AST* resolvedTarget = NULL;
            if (targetRef) {
                if (targetRef->type_def) {
                    resolvedTarget = resolveTypeAlias(targetRef->type_def);
                } else if (targetRef->right) {
                    resolvedTarget = resolveTypeAlias(targetRef->right);
                }
            }
            if (!resolvedTarget && targetRef) {
                resolvedTarget = resolveTypeAlias(targetRef);
            }

            const char* typeName = resolvedTarget ? getTypeNameFromAST(resolvedTarget) : NULL;
            if ((!typeName || !*typeName) && targetRef && targetRef->token && targetRef->token->value) {
                typeName = targetRef->token->value;
            }

            if (!typeName || !*typeName) {
                fprintf(stderr, "L%d: Compiler Error: Unable to resolve target type for assertion.\n", line);
                compiler_had_error = true;
                writeBytecodeChunk(chunk, POP, line);
                emitConstant(chunk, addNilConstant(chunk), line);
                break;
            }

            int typeConstIndex = addStringConstant(chunk, typeName);
            emitConstant(chunk, typeConstIndex, line);
            writeBytecodeChunk(chunk, CALL_HOST, line);
            writeBytecodeChunk(chunk, (uint8_t)HOST_FN_INTERFACE_ASSERT, line);
            break;
        }
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
                    long long start_ord = 0;
                    long long end_ord = 0;
                    bool start_ok = resolveSetElementOrdinal(member->left, &start_ord);
                    bool end_ok = resolveSetElementOrdinal(member->right, &end_ord);

                    if (start_ok && end_ok) {
                        if (start_ord <= end_ord) {
                            for (long long j = start_ord; j <= end_ord; j++) {
                                addOrdinalToSetValue(&set_const_val, j);
                            }
                        } else {
                            long long j = start_ord;
                            while (true) {
                                addOrdinalToSetValue(&set_const_val, j);
                                if (j == end_ord) {
                                    break;
                                }
                                if (j == LLONG_MIN) {
                                    fprintf(stderr, "L%d: Compiler error: Set range lower bound underflows ordinal minimum.\\n", getLine(member));
                                    compiler_had_error = true;
                                    break;
                                }
                                j--;
                            }
                        }
                    } else {
                        fprintf(stderr, "L%d: Compiler error: Set range bounds must be constant ordinal types.\n", getLine(member));
                        compiler_had_error = true;
                    }
                } else {
                    long long ord = 0;
                    if (resolveSetElementOrdinal(member, &ord)) {
                        addOrdinalToSetValue(&set_const_val, ord);
                    } else {
                        fprintf(stderr, "L%d: Compiler error: Set elements must be constant ordinal types.\n", getLine(member));
                        compiler_had_error = true;
                    }
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
            if (node_token->type == TOKEN_REAL_CONST) {
                constIndex = addRealConstant(chunk, atof(node_token->value));
            } else if (node_token->type == TOKEN_HEX_CONST) {
                unsigned long long v = strtoull(node_token->value, NULL, 16);
                constIndex = addIntConstant(chunk, (long long)v);
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
            writeBytecodeChunk(chunk, FORMAT_VALUE, line);
            writeBytecodeChunk(chunk, (uint8_t)width, line);
            writeBytecodeChunk(chunk, (uint8_t)decimals, line); // Using -1 (0xFF) for "not specified"
            break;
        }
        case AST_STRING: {
            if (!node_token || !node_token->value) { /* error */ break; }

            size_t len = (node->i_val > 0) ? (size_t)node->i_val
                                           : strlen(node_token->value);
            if (len == 1) {
                /* Single-character string literals represent CHAR constants.
                 * Cast through unsigned char so values in the 128..255 range
                 * are preserved correctly. */
                Value val = makeChar((unsigned char)node_token->value[0]);
                int constIndex = addConstantToChunk(chunk, &val);
                emitConstant(chunk, constIndex, line);
                // The temporary char value `val` does not need `freeValue`
            } else {
                int constIndex = addStringConstantLen(chunk, node_token->value, len);
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
            if (!node->left) {
                fprintf(stderr, "L%d: Compiler error: '@' requires addressable operand.\n", line);
                compiler_had_error = true;
                break;
            }

            // Support closure literals for procedures via @ProcName while preserving
            // regular address-of semantics for addressable l-values such as fields,
            // array elements, and dereferences.
            if (node->left->type == AST_VARIABLE && node->left->token && node->left->token->value) {
                const char* pname = node->left->token->value;
                Symbol* psym = resolveProcedureSymbolInScope(pname, node, gCurrentProgramRoot);
                if (psym) {
                    if (!emitClosureLiteral(psym, chunk, line)) {
                        break;
                    }
                    break;
                }
            }

            compileLValue(node->left, chunk, line);
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
            if (call->child_count == 0) {
                writeBytecodeChunk(chunk, THREAD_CREATE, line);
                emitShort(chunk, (uint16_t)proc_symbol->bytecode_address, line);
            } else {
                // Support spawning with multiple arguments (including receiver for methods).
                // Stack layout for host: [addr, arg0, arg1, ..., argc]
                int addrConstIndex = addIntConstant(chunk, proc_symbol->bytecode_address);
                recordAddressConstant(addrConstIndex, proc_symbol->bytecode_address);
                emitConstant(chunk, addrConstIndex, line);
                for (int i = 0; i < call->child_count; i++) {
                    compileRValue(call->children[i], chunk, getLine(call->children[i]));
                }
                emitConstant(chunk, addIntConstant(chunk, call->child_count), line);
                writeBytecodeChunk(chunk, CALL_HOST, line);
                writeBytecodeChunk(chunk, (uint8_t)HOST_FN_CREATE_THREAD_ADDR, line);
            }
            break;
        }
        case AST_DEREFERENCE: {
            // A dereference on the right-hand side means we get the value.
            // First, get the pointer value itself onto the stack by compiling the l-value.
            compileRValue(node->left, chunk, getLine(node->left));
            // Then, use GET_INDIRECT to replace the pointer with the value it points to.
            writeBytecodeChunk(chunk, GET_INDIRECT, line);
            break;
        }
        case AST_VARIABLE: {
            if (!node_token || !node_token->value) { /* error */ break; }
            const char* varName = node_token->value;

            int local_slot = -1;
            bool is_ref = false;
            if (current_function_compiler) {
                // Check if it's an assignment to the function name itself
                if (isCurrentFunctionResultIdentifier(current_function_compiler, varName)) {
                    local_slot = resolveLocal(current_function_compiler, current_function_compiler->name);
                } else {
                    local_slot = resolveLocal(current_function_compiler, varName);
                }

                if (local_slot != -1) {
                    is_ref = current_function_compiler->locals[local_slot].is_ref;
                }
                // Robust fallback for locals not yet registered in the function state.
                if (compiler_dynamic_locals && local_slot == -1 && current_function_compiler->function_symbol &&
                    current_function_compiler->function_symbol->type_def) {
                    AST* func_decl = current_function_compiler->function_symbol->type_def;
                    AST* decl_in_scope = findDeclarationInScope(varName, func_decl, node);
                    if (decl_in_scope && astNodeIsDescendant(func_decl, decl_in_scope)) {
                        addLocal(current_function_compiler, varName, line, false);
                        local_slot = current_function_compiler->local_count - 1;
                        is_ref = false;
                    }
                }
            }
            
            if (strcasecmp(varName, "break_requested") == 0) {
                // This is a special host-provided variable.
                // Instead of treating it as a global, we call a host function.
                writeBytecodeChunk(chunk, CALL_HOST, line);
                writeBytecodeChunk(chunk, (uint8_t)HOST_FN_QUIT_REQUESTED, line);
                break; // We are done compiling this node.
            }
            
            bool treat_as_local = (local_slot != -1);
            if (treat_as_local && current_function_compiler) {
                int param_count = 0;
                if (current_function_compiler->function_symbol) {
                    param_count = current_function_compiler->function_symbol->arity;
                }
                bool is_param = (local_slot < param_count);
                if (!is_param) {
                    Symbol *local_sym = lookupLocalSymbol(varName);
                    if (local_sym && !local_sym->is_local_var) {
                        treat_as_local = false;
                    }
                }
                if (treat_as_local) {
                    CompilerLocal* local = &current_function_compiler->locals[local_slot];
                    if (local->decl_node && getLine(local->decl_node) > line) {
                        treat_as_local = false;
                    }
                }
            }

            if (treat_as_local) {
                DBG_PRINTF("[dbg] RV %s -> local[%d] line=%d\n", varName, local_slot, line);
                noteLocalSlotUse(current_function_compiler, local_slot);
                writeBytecodeChunk(chunk, GET_LOCAL, line);
                writeBytecodeChunk(chunk, (uint8_t)local_slot, line);
                if (is_ref && node->var_type != TYPE_ARRAY) {
                    writeBytecodeChunk(chunk, GET_INDIRECT, line);
                }
            } else {
                // Check if the identifier refers to a local constant recorded in the
                // symbol table before resolving upvalues or globals. These constants do
                // not occupy a slot in the function's locals array, so `resolveLocal`
                // does not detect them.
                Symbol *local_const_sym = lookupLocalSymbol(varName);
                if (local_const_sym && local_const_sym->is_const && local_const_sym->value) {
                    emitConstant(chunk, addConstantToChunk(chunk, local_const_sym->value), line);
                } else {
                    int upvalue_slot = -1;
                    if (current_function_compiler) {
                        upvalue_slot = resolveUpvalue(current_function_compiler, varName);
                    }
                    if (upvalue_slot != -1) {
                        bool up_is_ref = current_function_compiler->upvalues[upvalue_slot].is_ref;
                        writeBytecodeChunk(chunk, GET_UPVALUE, line);
                        writeBytecodeChunk(chunk, (uint8_t)upvalue_slot, line);
                        if (up_is_ref && node->var_type != TYPE_ARRAY) {
                            writeBytecodeChunk(chunk, GET_INDIRECT, line);
                        }
                    } else {
                        // Check if it's a compile-time constant first.
                        Value* const_val_ptr = findCompilerConstant(varName);
                        if (const_val_ptr) {
                            emitConstant(chunk, addConstantToChunk(chunk, const_val_ptr), line);
                        } else {
                            if (emitImplicitMyselfFieldValue(chunk, line, varName)) {
                                break;
                            }
                            DBG_PRINTF("[dbg] RV %s -> global line=%d\n", varName, line);
                            int nameIndex = addStringConstant(chunk, varName);
                            emitGlobalNameIdx(chunk, GET_GLOBAL, GET_GLOBAL16,
                                               nameIndex, line);
                        }
                    }
                }
            }
            break;
        }
        case AST_FIELD_ACCESS: {
            // Interface method calls generated by the Pascal front end keep the
            // qualified selector (e.g. `logger.Log`) as the first child of the
            // call expression.  When we compile that synthetic child we only
            // want the receiver value on the stack—not to treat the selector as
            // a real record field.  Otherwise the regular record lookup path
            // below fires and produces an "Unknown field" error because
            // interfaces do not expose concrete fields.
            AST* interfaceRecv = NULL;
            if (node->parent && node->parent->type == AST_PROCEDURE_CALL &&
                node->parent->child_count > 0 &&
                node->parent->children[0] == node) {
                interfaceRecv = node->parent->left;
            }
            if (!interfaceRecv && node->left) {
                interfaceRecv = node->left;
            }
            if (interfaceRecv) {
                AST* ifaceType = getInterfaceTypeFromExpression(interfaceRecv);
                if (ifaceType) {
                    compileRValue(interfaceRecv, chunk, getLine(interfaceRecv));
                    break;
                }
            }

            char qualified_name[MAX_SYMBOL_LENGTH * 2 + 2];
            Symbol* resolved_symbol = NULL;
            if (resolveUnitQualifiedGlobal(node, qualified_name, sizeof(qualified_name), &resolved_symbol)) {
                if (resolved_symbol && resolved_symbol->is_const && resolved_symbol->value) {
                    emitConstant(chunk, addConstantToChunk(chunk, resolved_symbol->value), line);
                } else {
                    int nameIndex = addStringConstant(chunk, qualified_name);
                    emitGlobalNameIdx(chunk, GET_GLOBAL, GET_GLOBAL16,
                                       nameIndex, line);
                }
                break;
            }

            // If this field is a compile-time constant, embed its value directly.
            if (node->token && node->token->value) {
                Value* const_ptr = findCompilerConstant(node->token->value);
                if (const_ptr) {
                    if (node->left) {
                        compileRValue(node->left, chunk, getLine(node->left));
                        writeBytecodeChunk(chunk, POP, line);
                    }
                    emitConstant(chunk, addConstantToChunk(chunk, const_ptr), line);
                    break;
                }
            }
            int fieldOffset = -1;
            if (!pushFieldBaseAndResolveOffset(node, chunk, line, &fieldOffset)) {
                break;
            }

            if (fieldOffset <= 0xFF) {
                writeBytecodeChunk(chunk, LOAD_FIELD_VALUE, line);
                writeBytecodeChunk(chunk, (uint8_t)fieldOffset, line);
            } else {
                writeBytecodeChunk(chunk, LOAD_FIELD_VALUE16, line);
                emitShort(chunk, (uint16_t)fieldOffset, line);
            }
            break;
        }
        case AST_ARRAY_ACCESS: {
            // This logic correctly distinguishes between accessing a string/char vs. a regular array.
            if (node->left && (node->left->var_type == TYPE_STRING || node->left->var_type == TYPE_CHAR)) {
                compileRValue(node->left, chunk, getLine(node->left));      // Push the string or char
                compileRValue(node->children[0], chunk, getLine(node->children[0])); // Push the index
                writeBytecodeChunk(chunk, GET_CHAR_FROM_STRING, line); // Use the specialized opcode
                break;
            }

            ConstArrayAccessInfo const_info;
            if (computeConstantArrayAccess(node, &const_info)) {
                bool emitted_base = false;
                AST* base_expr = const_info.base_expr;
                if (base_expr && base_expr->type == AST_VARIABLE &&
                    base_expr->token && base_expr->token->value) {
                    const char* base_name = base_expr->token->value;
                    Symbol* local_const = lookupLocalSymbol(base_name);
                    if (local_const && local_const->is_const && local_const->value) {
                        emitConstant(chunk, addConstantToChunk(chunk, local_const->value), line);
                        emitted_base = true;
                    } else {
                        Symbol* global_const = lookupGlobalSymbol(base_name);
                        if (global_const && global_const->is_const && global_const->value) {
                            emitConstant(chunk, addConstantToChunk(chunk, global_const->value), line);
                            emitted_base = true;
                        } else {
                            Value* const_val_ptr = findCompilerConstant(base_name);
                            if (const_val_ptr) {
                                emitConstant(chunk, addConstantToChunk(chunk, const_val_ptr), line);
                                emitted_base = true;
                            }
                        }
                    }
                }
                if (!emitted_base) {
                    compileLValue(base_expr, chunk, getLine(base_expr));
                }
                writeBytecodeChunk(chunk, LOAD_ELEMENT_VALUE_CONST, line);
                emitInt32(chunk, (uint32_t)const_info.offset, line);
                break;
            }

            for (int i = 0; i < node->child_count; i++) {
                compileRValue(node->children[i], chunk, getLine(node->children[i]));
            }

            AST* base = node->left;
            if (!base) {
                fprintf(stderr, "L%d: Compiler error: Array access missing base expression.\n", line);
                compiler_had_error = true;
                break;
            }

            switch (base->type) {
                case AST_VARIABLE: {
                    bool emitted_base = false;
                    if (base->token && base->token->value) {
                        const char* base_name = base->token->value;
                        Symbol* local_const = lookupLocalSymbol(base_name);
                        if (local_const && local_const->is_const && local_const->value) {
                            emitConstant(chunk, addConstantToChunk(chunk, local_const->value), line);
                            emitted_base = true;
                        } else {
                            Symbol* global_const = lookupGlobalSymbol(base_name);
                            if (global_const && global_const->is_const && global_const->value) {
                                emitConstant(chunk, addConstantToChunk(chunk, global_const->value), line);
                                emitted_base = true;
                            } else {
                                Value* const_val_ptr = findCompilerConstant(base_name);
                                if (const_val_ptr) {
                                    emitConstant(chunk, addConstantToChunk(chunk, const_val_ptr), line);
                                    emitted_base = true;
                                }
                            }
                        }
                    }
                    if (!emitted_base) {
                        compileLValue(base, chunk, getLine(base));
                    }
                    break;
                }
                case AST_FIELD_ACCESS:
                case AST_ARRAY_ACCESS:
                case AST_DEREFERENCE:
                    compileLValue(base, chunk, getLine(base));
                    break;
                default:
                    compileRValue(base, chunk, getLine(base));
                    break;
            }
            writeBytecodeChunk(chunk, LOAD_ELEMENT_VALUE, line);
            writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
            break;
        }
        case AST_ASSIGN: {
            AST* lvalue = node->left;
            AST* rvalue = node->right;

            if (isIntlikeType(lvalue->var_type) && isIntlikeType(rvalue->var_type)) {
                int lrank = intTypeRank(lvalue->var_type);
                int rrank = intTypeRank(rvalue->var_type);
                if (rrank > lrank && !constantFitsInIntType(rvalue, lvalue->var_type)) {
                    fprintf(stderr, "L%d: Compiler warning: assigning %s to %s may lose precision.\n",
                            line, varTypeToString(rvalue->var_type), varTypeToString(lvalue->var_type));
                }
            }

            if (node->token && (node->token->type == TOKEN_PLUS || node->token->type == TOKEN_MINUS)) {
                // Compound assignment: evaluate LHS once and preserve result on the stack
                compileLValue(lvalue, chunk, getLine(lvalue));            // stack: [addr]
                writeBytecodeChunk(chunk, DUP, line);                     // [addr, addr]
                writeBytecodeChunk(chunk, DUP, line);                     // [addr, addr, addr]
                writeBytecodeChunk(chunk, GET_INDIRECT, line);            // [addr, addr, value]
                compileRValue(rvalue, chunk, getLine(rvalue));            // [addr, addr, value, rhs]
                if (node->token->type == TOKEN_PLUS) writeBytecodeChunk(chunk, ADD, line);
                else writeBytecodeChunk(chunk, SUBTRACT, line);           // [addr, addr, result]
                writeBytecodeChunk(chunk, SET_INDIRECT, line);            // [addr]
                writeBytecodeChunk(chunk, GET_INDIRECT, line);            // [result]
            } else {
                compileRValue(rvalue, chunk, getLine(rvalue));
                maybeAutoBoxInterfaceForExpression(lvalue, rvalue, chunk, line, true);
                writeBytecodeChunk(chunk, DUP, line); // Preserve assigned value as the expression result

                if (current_function_compiler && current_function_compiler->returns_value &&
                    current_function_compiler->name && lvalue->type == AST_VARIABLE &&
                    lvalue->token && lvalue->token->value &&
                    isCurrentFunctionResultIdentifier(current_function_compiler, lvalue->token->value)) {

                    int return_slot = resolveLocal(current_function_compiler, current_function_compiler->name);
                    if (return_slot != -1) {
                        noteLocalSlotUse(current_function_compiler, return_slot);
                        writeBytecodeChunk(chunk, SET_LOCAL, line);
                        writeBytecodeChunk(chunk, (uint8_t)return_slot, line);
                    } else {
                        fprintf(stderr, "L%d: Compiler internal error: could not resolve slot for function return value '%s'.\n", line, current_function_compiler->name);
                        compiler_had_error = true;
                    }
                } else {
                    int store_line = getLine(lvalue);
                    if (!emitDirectStoreForVariable(lvalue, chunk, store_line)) {
                        compileLValue(lvalue, chunk, store_line);
                        writeBytecodeChunk(chunk, SWAP, line);
                        writeBytecodeChunk(chunk, SET_INDIRECT, line);
                    }
                }
            }
            break;
        }
        case AST_BINARY_OP: {
            if (node_token && node_token->type == TOKEN_AND) {
                // Check annotated type to decide between bitwise and logical AND
                if (node->var_type != TYPE_BOOLEAN && node->left && isIntlikeType(node->left->var_type)) {
                    // Bitwise AND for integer types
                    compileRValue(node->left, chunk, getLine(node->left));
                    compileRValue(node->right, chunk, getLine(node->right));
                    writeBytecodeChunk(chunk, AND, line);
                } else {
                    // Logical AND for booleans (with short-circuiting)
                    compileRValue(node->left, chunk, getLine(node->left)); // stack: [A]
                    int jump_if_false = chunk->count;
                    writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);     // Pops A. Jumps if A is false.
                    emitShort(chunk, 0xFFFF, line);

                    // If A was true, result is B.
                    compileRValue(node->right, chunk, getLine(node->right)); // stack: [B]
                    writeBytecodeChunk(chunk, TO_BOOL, line);
                    int jump_over_false_case = chunk->count;
                    writeBytecodeChunk(chunk, JUMP, line);
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
                if (node->var_type != TYPE_BOOLEAN && node->left && isIntlikeType(node->left->var_type)) {
                    // Bitwise OR for integer types
                    compileRValue(node->left, chunk, getLine(node->left));
                    compileRValue(node->right, chunk, getLine(node->right));
                    writeBytecodeChunk(chunk, OR, line);
                } else {
                    // Logical OR for booleans (with short-circuiting)
                compileRValue(node->left, chunk, getLine(node->left)); // stack: [A]
                int jump_if_false = chunk->count;
                writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);     // Pops A. Jumps if A is false.
                emitShort(chunk, 0xFFFF, line);

                // If we get here, A was true. Stack is empty. The result must be 'true'.
                int true_const_idx = addBooleanConstant(chunk, true);
                emitConstant(chunk, true_const_idx, line);
                int jump_to_end = chunk->count;
                writeBytecodeChunk(chunk, JUMP, line);
                emitShort(chunk, 0xFFFF, line);

                // This is where we land if A was false. Stack is empty.
                // The result of the expression is the result of B.
                patchShort(chunk, jump_if_false + 1, chunk->count - (jump_if_false + 3));
                compileRValue(node->right, chunk, getLine(node->right));
                writeBytecodeChunk(chunk, TO_BOOL, line);

                // The end for both paths.
                patchShort(chunk, jump_to_end + 1, chunk->count - (jump_to_end + 3));
                }
            }
            else if (node_token && node_token->type == TOKEN_XOR) {
                compileRValue(node->left, chunk, getLine(node->left));
                compileRValue(node->right, chunk, getLine(node->right));
                writeBytecodeChunk(chunk, XOR, line);
            }
            else { // Original logic for all other operators
                compileRValue(node->left, chunk, getLine(node->left));
                compileRValue(node->right, chunk, getLine(node->right));
                if (node_token) { // node_token is the operator
                    switch (node_token->type) {
                        case TOKEN_PLUS:          writeBytecodeChunk(chunk, ADD, line); break;
                        case TOKEN_MINUS:         writeBytecodeChunk(chunk, SUBTRACT, line); break;
                        case TOKEN_MUL:           writeBytecodeChunk(chunk, MULTIPLY, line); break;
                        case TOKEN_SLASH:         writeBytecodeChunk(chunk, DIVIDE, line); break;
                        case TOKEN_INT_DIV:
                            writeBytecodeChunk(chunk, INT_DIV, line);
                            break;
                        case TOKEN_MOD:           writeBytecodeChunk(chunk, MOD, line); break;
                        // AND and OR are now handled above
                        case TOKEN_SHL:           writeBytecodeChunk(chunk, SHL, line); break;
                        case TOKEN_SHR:           writeBytecodeChunk(chunk, SHR, line); break;
                        case TOKEN_EQUAL:         writeBytecodeChunk(chunk, EQUAL, line); break;
                        case TOKEN_NOT_EQUAL:     writeBytecodeChunk(chunk, NOT_EQUAL, line); break;
                        case TOKEN_LESS:          writeBytecodeChunk(chunk, LESS, line); break;
                        case TOKEN_LESS_EQUAL:    writeBytecodeChunk(chunk, LESS_EQUAL, line); break;
                        case TOKEN_GREATER:       writeBytecodeChunk(chunk, GREATER, line); break;
                        case TOKEN_GREATER_EQUAL: writeBytecodeChunk(chunk, GREATER_EQUAL, line); break;
                        case TOKEN_IN:            writeBytecodeChunk(chunk, IN, line); break;
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
                    case TOKEN_MINUS: writeBytecodeChunk(chunk, NEGATE, line); break;
                    case TOKEN_NOT:   writeBytecodeChunk(chunk, NOT, line);    break;
                    default:
                        fprintf(stderr, "L%d: Compiler error: Unknown unary operator %s\n", line, tokenTypeToString(node_token->type));
                        compiler_had_error = true;
                        break;
                }
            }
            break;
        }
        case AST_TERNARY: {
            if (!node->left || !node->right || !node->extra) {
                fprintf(stderr, "L%d: Compiler error: Incomplete ternary expression.\n", line);
                compiler_had_error = true;
                break;
            }
            compileRValue(node->left, chunk, getLine(node->left));
            int jumpToElse = chunk->count;
            writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
            emitShort(chunk, 0xFFFF, line);

            compileRValue(node->right, chunk, getLine(node->right));
            int jumpToEnd = chunk->count;
            writeBytecodeChunk(chunk, JUMP, line);
            emitShort(chunk, 0xFFFF, line);

            uint16_t elseOffset = (uint16_t)(chunk->count - (jumpToElse + 3));
            patchShort(chunk, jumpToElse + 1, elseOffset);

            compileRValue(node->extra, chunk, getLine(node->extra));
            uint16_t endOffset = (uint16_t)(chunk->count - (jumpToEnd + 3));
            patchShort(chunk, jumpToEnd + 1, endOffset);
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

            const char* functionName = NULL;
            const char* methodIdentifier = NULL;
            bool isCallQualified = false;
            bool usesReceiverGlobal = false;

            if (node->left &&
                node->token && node->token->value && node->token->type == TOKEN_IDENTIFIER) {
                functionName = node->token->value;
                methodIdentifier = node->token->value;
                isCallQualified = true;
            } else if (node->token && node->token->value && node->token->type == TOKEN_IDENTIFIER) {
                functionName = node->token->value;
                methodIdentifier = node->token->value;
                isCallQualified = false;
            } else {
                fprintf(stderr, "L%d: Compiler error: Invalid callee in AST_PROCEDURE_CALL (expression).\n", line);
                compiler_had_error = true;
                emitConstant(chunk, addNilConstant(chunk), line);
                break;
            }

            // If this is a qualified call like receiver.method(...), attempt to
            // mangle to ClassName.method by inspecting the receiver's static type.
            char mangled_name_buf[MAX_SYMBOL_LENGTH * 2 + 2];
            if (isCallQualified && node->left && functionName) {
                const char* cls_name = NULL;
                AST* type_ref = node->left->type_def;
                if (type_ref) {
                    // Resolve possible type alias to get to TYPE_REFERENCE
                    while (type_ref && type_ref->type == AST_TYPE_REFERENCE && type_ref->right) {
                        type_ref = type_ref->right;
                    }
                    if (node->left->type_def && node->left->type_def->token && node->left->type_def->token->value) {
                        cls_name = node->left->type_def->token->value;
                    } else if (type_ref && type_ref->token && type_ref->token->value) {
                        cls_name = type_ref->token->value;
                    }
                }
                if (cls_name) {
                    size_t cls_len = strlen(cls_name);
                    size_t fn_len = strlen(functionName);
                    bool alreadyQualified = false;
                    if (fn_len > cls_len &&
                        strncasecmp(functionName, cls_name, cls_len) == 0 &&
                        functionName[cls_len] == '.') {
                        alreadyQualified = true;
                    }
                    if (!alreadyQualified) {
                        snprintf(mangled_name_buf, sizeof(mangled_name_buf), "%s.%s", cls_name, functionName);
                        functionName = mangled_name_buf;
                    }
                }
            }

            if (strcasecmp(functionName, "printf") == 0) {
                compilePrintf(node, chunk, line);
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
                writeBytecodeChunk(chunk, CALL_HOST, line);
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
                writeBytecodeChunk(chunk, CALL_HOST, line);
                writeBytecodeChunk(chunk, (uint8_t)HOST_FN_WAIT_THREAD, line);
                break;
            }
            if (strcasecmp(functionName, "mutex") == 0) {
                if (node->child_count != 0) {
                    fprintf(stderr, "L%d: Compiler Error: mutex expects no arguments.\n", line);
                }
                writeBytecodeChunk(chunk, MUTEX_CREATE, line);
                break;
            }
            if (strcasecmp(functionName, "rcmutex") == 0) {
                if (node->child_count != 0) {
                    fprintf(stderr, "L%d: Compiler Error: rcmutex expects no arguments.\n", line);
                }
                writeBytecodeChunk(chunk, RCMUTEX_CREATE, line);
                break;
            }
            // (indirect function pointer calls are handled later in the fallback path)
            if (strcasecmp(functionName, "lock") == 0) {
                if (node->child_count != 1) {
                    fprintf(stderr, "L%d: Compiler Error: lock expects 1 argument.\n", line);
                } else {
                    compileRValue(node->children[0], chunk, getLine(node->children[0]));
                }
                writeBytecodeChunk(chunk, MUTEX_LOCK, line);
                break;
            }
            if (strcasecmp(functionName, "unlock") == 0) {
                if (node->child_count != 1) {
                    fprintf(stderr, "L%d: Compiler Error: unlock expects 1 argument.\n", line);
                } else {
                    compileRValue(node->children[0], chunk, getLine(node->children[0]));
                }
                writeBytecodeChunk(chunk, MUTEX_UNLOCK, line);
                break;
            }

            if (strcasecmp(functionName, "destroy") == 0) {
                if (node->child_count != 1) {
                    fprintf(stderr, "L%d: Compiler Error: destroy expects 1 argument.\n", line);
                } else {
                    compileRValue(node->children[0], chunk, getLine(node->children[0]));
                }
                writeBytecodeChunk(chunk, MUTEX_DESTROY, line);
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
            if (func_symbol && func_symbol->name) {
                functionName = func_symbol->name;   // ensure emitted call uses canonical lowercase
            }

            if (!func_symbol) {
                const char* dot = strrchr(func_name_lower, '.');
                if (dot && *(dot + 1)) {
                    Symbol* alt = lookupProcedure(dot + 1);
                    if (alt && alt->is_alias) {
                        alt = alt->real_symbol;
                    }
                    if (alt) {
                        func_symbol = alt;
                        if (func_symbol->name) {
                            functionName = func_symbol->name;
                        } else {
                            functionName = dot + 1;
                        }
                        if (node->child_count > 0) {
                            usesReceiverGlobal = true;
                        }
                    }
                }
            }

            AST* interfaceReceiver = NULL;
            AST* interfaceType = NULL;
            int interfaceArgStart = 0;

            if (isCallQualified) {
                if (node->child_count > 0) {
                    AST* candidate = node->children[0];
                    AST* candidateType = getInterfaceTypeFromExpression(candidate);
                    if (candidateType || candidate->var_type == TYPE_INTERFACE) {
                        interfaceReceiver = candidate;
                        interfaceType = candidateType;
                        interfaceArgStart = 1;
                    }
                }
                if (!interfaceReceiver && node->left) {
                    AST* candidateType = getInterfaceTypeFromExpression(node->left);
                    if (candidateType || node->left->var_type == TYPE_INTERFACE) {
                        interfaceReceiver = node->left;
                        interfaceType = candidateType;
                        interfaceArgStart = 0;

                        if (node->child_count > 0) {
                            AST* firstChild = node->children[0];
                            if (firstChild == interfaceReceiver ||
                                (firstChild && firstChild->type == AST_FIELD_ACCESS &&
                                 firstChild->left == interfaceReceiver)) {
                                interfaceArgStart = 1;
                            }
                        }
                    }
                }
            }

            bool isVirtualMethod = isCallQualified && interfaceReceiver == NULL &&
                                   node->i_val == 0 && func_symbol && func_symbol->type_def &&
                                   func_symbol->type_def->is_virtual;
            bool isInterfaceDispatch = isCallQualified && interfaceReceiver != NULL;

            int receiver_offset = (usesReceiverGlobal && node->child_count > 0) ? 1 : 0;

            // Inline function calls directly when marked inline.
            if (func_symbol && func_symbol->type_def && func_symbol->type_def->is_inline) {
                compileInlineRoutine(func_symbol, node, chunk, line, true);
                break;
            }

            if (isVirtualMethod) {
                if (node->child_count > 0) {
                    // Compile receiver and keep duplicate on top
                    AST* recv = node->children[0];
                    compileRValue(recv, chunk, getLine(recv));
                    writeBytecodeChunk(chunk, DUP, line);
                    for (int i = 1; i < node->child_count; i++) {
                        AST* arg_node = node->children[i];
                        bool is_var_param = false;
                        if (func_symbol->type_def && i < func_symbol->type_def->child_count) {
                            AST* param_node = func_symbol->type_def->children[i];
                            if (param_node && param_node->by_ref) is_var_param = true;
                        }
                        if (is_var_param) {
                            compileLValue(arg_node, chunk, getLine(arg_node));
                        } else {
                            compileRValue(arg_node, chunk, getLine(arg_node));
                        }
                        writeBytecodeChunk(chunk, SWAP, line);
                    }
                    writeBytecodeChunk(chunk, GET_FIELD_OFFSET, line);
                    writeBytecodeChunk(chunk, (uint8_t)0, line);
                    writeBytecodeChunk(chunk, GET_INDIRECT, line);
                    emitConstant(chunk, addIntConstant(chunk, func_symbol->type_def->i_val), line);
                    writeBytecodeChunk(chunk, SWAP, line);
                    writeBytecodeChunk(chunk, GET_ELEMENT_ADDRESS, line);
                    writeBytecodeChunk(chunk, (uint8_t)1, line);
                    writeBytecodeChunk(chunk, GET_INDIRECT, line);
                    writeBytecodeChunk(chunk, CALL_INDIRECT, line);
                    writeBytecodeChunk(chunk, (uint8_t)node->child_count, line);
                    break;
                }
            }

            if (isInterfaceDispatch) {
                if (!func_symbol || !func_symbol->type_def) {
                    fprintf(stderr,
                            "L%d: Compiler Error: Unable to resolve interface method '%s'.\n",
                            line, functionName ? functionName : "<anonymous>");
                    compiler_had_error = true;
                    emitConstant(chunk, addNilConstant(chunk), line);
                    break;
                }

                int method_slot = func_symbol->type_def->i_val;
                const char* slotName = methodIdentifier ? methodIdentifier : functionName;
                if (interfaceType) {
                    int resolved_slot = ensureInterfaceMethodSlot(interfaceType, slotName);
                    if (resolved_slot >= 0) {
                        method_slot = resolved_slot;
                        func_symbol->type_def->i_val = method_slot;
                    }
                }
                if (method_slot < 0) {
                    fprintf(stderr,
                            "L%d: Compiler Error: Interface method '%s' missing slot index.\n",
                            line, functionName ? functionName : "<anonymous>");
                    compiler_had_error = true;
                    emitConstant(chunk, addNilConstant(chunk), line);
                    break;
                }

                AST* recv = interfaceReceiver;
                compileRValue(recv, chunk, getLine(recv));
                int slotConst = addIntConstant(chunk, method_slot);
                emitConstant(chunk, slotConst, line);
                writeBytecodeChunk(chunk, CALL_HOST, line);
                writeBytecodeChunk(chunk, (uint8_t)HOST_FN_INTERFACE_LOOKUP, line);

                int metadataOffset = 1 - interfaceArgStart;
                if (metadataOffset < 0) metadataOffset = 0;
                for (int i = interfaceArgStart; i < node->child_count; i++) {
                    AST* arg_node = node->children[i];
                    bool is_var_param = false;
                    AST* param_type_hint = NULL;
                    int meta_index = i + metadataOffset;
                    if (func_symbol->type_def &&
                        meta_index >= 0 && meta_index < func_symbol->type_def->child_count) {
                        AST* param_node = func_symbol->type_def->children[meta_index];
                        if (param_node && param_node->by_ref) is_var_param = true;
                        if (param_node) {
                            param_type_hint = param_node->type_def ? param_node->type_def
                                                                   : (param_node->right ? param_node->right : param_node);
                        }
                    }
                    if (is_var_param) {
                        compileLValue(arg_node, chunk, getLine(arg_node));
                    } else {
                        compileRValue(arg_node, chunk, getLine(arg_node));
                        maybeAutoBoxInterfaceForType(param_type_hint, arg_node, chunk, getLine(arg_node), true, false);
                    }
                    writeBytecodeChunk(chunk, SWAP, line);
                }

                writeBytecodeChunk(chunk, CALL_INDIRECT, line);
                int total_args = node->child_count - interfaceArgStart;
                if (total_args < 0) total_args = 0;
                writeBytecodeChunk(chunk, (uint8_t)total_args, line);
                break;
            }

            bool emittedLowHighArg = false;
            bool isLowHighBuiltin = (!func_symbol && isBuiltin(functionName) &&
                                     (strcasecmp(functionName, "low") == 0 || strcasecmp(functionName, "high") == 0));

            if (isLowHighBuiltin && node->child_count == 1) {
                AST* arg0 = node->children[0];
                DBG_PRINTF("[dbg low/high] arg0 type=%s token=%s vtype=%s\n",
                           astTypeToString(arg0->type),
                           (arg0->token && arg0->token->value) ? arg0->token->value : "<null>",
                           varTypeToString(arg0->var_type));

                const char* tname = NULL;
                bool emittedValue = false;
                char buf[32];

                if (arg0->type == AST_VARIABLE && arg0->token && arg0->token->value) {
                    AST* td = lookupType(arg0->token->value);
                    if (td) {
                        VarType tv = td->var_type;
                        if (tv == TYPE_INTEGER) tv = TYPE_INT32;
                        if (tv == TYPE_REAL) tv = TYPE_DOUBLE;
                        if (tv == TYPE_INT32 || tv == TYPE_DOUBLE || tv == TYPE_FLOAT ||
                            tv == TYPE_CHAR  || tv == TYPE_BOOLEAN || tv == TYPE_BYTE || tv == TYPE_WORD) {
                            Value av;
                            memset(&av, 0, sizeof(Value));
                            av.type = tv;
                            int cidx = addConstantToChunk(chunk, &av);
                            emitConstant(chunk, cidx, line);
                            emittedValue = true;
                        } else {
                            tname = arg0->token->value;
                        }
                    } else {
                        VarType basic = resolveOrdinalBuiltinTypeName(arg0->token->value);
                        if (basic != TYPE_UNKNOWN) {
                            Value av;
                            memset(&av, 0, sizeof(Value));
                            av.type = basic;
                            int cidx = addConstantToChunk(chunk, &av);
                            emitConstant(chunk, cidx, line);
                            emittedValue = true;
                        }
                    }
                }

                if (!emittedValue && !tname &&
                    (arg0->type == AST_TYPE_REFERENCE || arg0->type == AST_PROCEDURE_CALL) &&
                    arg0->token && arg0->token->value) {
                    tname = arg0->token->value;
                }

                if (!emittedValue && !tname &&
                    arg0->var_type != TYPE_UNKNOWN && arg0->var_type != TYPE_VOID &&
                    arg0->var_type != TYPE_ARRAY) {
                    switch (arg0->var_type) {
                        case TYPE_INT32:
                            tname = "integer";
                            break;
                        case TYPE_DOUBLE:
                            tname = "real";
                            break;
                        case TYPE_FLOAT:
                            tname = "float";
                            break;
                        case TYPE_CHAR:
                            tname = "char";
                            break;
                        case TYPE_BOOLEAN:
                            tname = "boolean";
                            break;
                        case TYPE_BYTE:
                            tname = "byte";
                            break;
                        case TYPE_WORD:
                            tname = "word";
                            break;
                        default:
                            snprintf(buf, sizeof(buf), "%s", varTypeToString(arg0->var_type));
                            tname = buf;
                            break;
                    }
                }

                if (!emittedValue && tname) {
                    int typeNameIndex = addStringConstant(chunk, tname);
                    emitConstant(chunk, typeNameIndex, line);
                    emittedValue = true;
                }

                emittedLowHighArg = emittedValue;
            }

            if (!isInterfaceDispatch && (!isLowHighBuiltin || !emittedLowHighArg)) {
                for (int i = receiver_offset; i < node->child_count; i++) {
                    AST* arg_node = node->children[i];
                    if (!arg_node) continue;

                    int param_index = i - receiver_offset;
                    bool is_var_param = false;
                    AST* param_type_hint = NULL;
                    if (func_symbol && func_symbol->type_def && param_index < func_symbol->type_def->child_count) {
                        AST* param_node = func_symbol->type_def->children[param_index];
                        if (param_node && param_node->by_ref) {
                            is_var_param = true;
                        }
                        if (param_node) {
                            param_type_hint = param_node->type_def ? param_node->type_def
                                                                   : (param_node->right ? param_node->right : param_node);
                        }
                    } else if (functionName && param_index == 0 && strcasecmp(functionName, "eof") == 0) {
                        // Built-in EOF takes its file parameter by reference
                        is_var_param = true;
                    } else if (!func_symbol && functionName) {
                        if ((strcasecmp(functionName, "GetMouseState") == 0 && param_index <= 3) ||
                            (strcasecmp(functionName, "GetScreenSize") == 0 && param_index <= 1)) {
                            is_var_param = true;
                        }
                    }

                    if (is_var_param) {
                        compileLValue(arg_node, chunk, getLine(arg_node));
                    } else {
                        compileRValue(arg_node, chunk, getLine(arg_node));
                        maybeAutoBoxInterfaceForType(param_type_hint, arg_node, chunk, getLine(arg_node), true, false);
                    }
                }
            }

            int call_arg_count = node->child_count - receiver_offset;
            if (call_arg_count < 0) call_arg_count = 0;

            if (usesReceiverGlobal && node->child_count > 0) {
                int myself_idx = ensureMyselfGlobalNameIndex(chunk);
                AST* recv_node = node->children[0];
                compileRValue(recv_node, chunk, getLine(recv_node));
                emitGlobalNameIdx(chunk, SET_GLOBAL, SET_GLOBAL16, myself_idx, line);
            }

            if (!func_symbol) {
                AST* castType = lookupType(functionName);
                if (castType) {
                    AST* resolvedCast = resolveTypeAlias(castType);
                    if (resolvedCast && resolvedCast->var_type == TYPE_INTERFACE) {
                        if (call_arg_count != 1) {
                            fprintf(stderr, "L%d: Compiler Error: Interface cast '%s' expects exactly 1 argument (got %d).\n",
                                    line, functionName, call_arg_count);
                            compiler_had_error = true;
                            for (uint8_t i = 0; i < call_arg_count; ++i) {
                                writeBytecodeChunk(chunk, POP, line);
                            }
                            emitConstant(chunk, addNilConstant(chunk), line);
                            break;
                        }

                        AST* argNode = (receiver_offset < node->child_count) ? node->children[receiver_offset] : NULL;
                        InterfaceBoxingResult boxed = autoBoxInterfaceValue(
                            resolvedCast,
                            argNode,
                            chunk,
                            line,
                            functionName ? functionName : "",
                            true,
                            true);
                        if (boxed == INTERFACE_BOX_FAILED) {
                            break;
                        }
                        break;
                    }
                    if (call_arg_count != 1) {
                        fprintf(stderr, "L%d: Compiler Error: Type cast '%s' expects exactly 1 argument (got %d).\n",
                                line, functionName, call_arg_count);
                        compiler_had_error = true;
                        for (uint8_t i = 0; i < call_arg_count; ++i) {
                            writeBytecodeChunk(chunk, POP, line);
                        }
                        emitConstant(chunk, addNilConstant(chunk), line);
                    }
                    break;
                }
            }

            if (!func_symbol && isBuiltin(functionName)) {
                BuiltinRoutineType type = getBuiltinType(functionName);
                if (type == BUILTIN_TYPE_PROCEDURE) {
                    fprintf(stderr, "L%d: Compiler Error: Built-in procedure '%s' cannot be used as a function in an expression.\n", line, functionName);
                    compiler_had_error = true;
                    for (uint8_t i = 0; i < call_arg_count; ++i) writeBytecodeChunk(chunk, POP, line);
                    emitConstant(chunk, addNilConstant(chunk), line);
                } else if (type == BUILTIN_TYPE_FUNCTION) {
                    int nameIndex = ensureBuiltinStringConstants(chunk, functionName, NULL);
                    writeBytecodeChunk(chunk, CALL_BUILTIN, line);
                    emitShort(chunk, (uint16_t)nameIndex, line);
                    writeBytecodeChunk(chunk, (uint8_t)call_arg_count, line);
                } else {
                    // Fallback to indirect function pointer call: use variable's value as address.
                    AST tmpVar; memset(&tmpVar, 0, sizeof(AST));
                    tmpVar.type = AST_VARIABLE; tmpVar.token = node->token;
                    // Arguments are already compiled (above) and on the stack; now push callee address
                    compileRValue(&tmpVar, chunk, line);
                    writeBytecodeChunk(chunk, CALL_INDIRECT, line);
                    writeBytecodeChunk(chunk, (uint8_t)call_arg_count, line);
                }
            } else {
                char original_display_name[MAX_SYMBOL_LENGTH * 2 + 2];
                if (isCallQualified) {
                    const char *receiver_name = "<expr>";
                    if (node->left && node->left->token && node->left->token->value) {
                        receiver_name = node->left->token->value;
                    }
                    int max_symbol_segment = MAX_SYMBOL_LENGTH - 1;
                    snprintf(original_display_name, sizeof(original_display_name), "%.*s.%.*s",
                             max_symbol_segment, receiver_name,
                             max_symbol_segment, functionName);
                } else {
                    strncpy(original_display_name, functionName, sizeof(original_display_name)-1);
                    original_display_name[sizeof(original_display_name)-1] = '\0';
                }
                
                if (func_symbol) {

                    if (func_symbol->type == TYPE_VOID) {
                        fprintf(stderr, "L%d: Compiler Error: Procedure '%s' cannot be used as a function.\n", line, original_display_name);
                        compiler_had_error = true;
                        for (uint8_t i = 0; i < call_arg_count; ++i) writeBytecodeChunk(chunk, POP, line);
                        emitConstant(chunk, addNilConstant(chunk), line);
                    } else if (((!func_symbol && isBuiltin(functionName) &&
                                 (strcasecmp(functionName, "inc") == 0 || strcasecmp(functionName, "dec") == 0)))
                               ? !(call_arg_count == 1 || call_arg_count == 2)
                               : (func_symbol->arity != call_arg_count)) {
                        if (!func_symbol && isBuiltin(functionName) &&
                            (strcasecmp(functionName, "inc") == 0 || strcasecmp(functionName, "dec") == 0)) {
                            fprintf(stderr, "L%d: Compiler Error: '%s' expects 1 or 2 argument(s) but %d were provided.\n",
                                    line, original_display_name, call_arg_count);
                        } else {
                            fprintf(stderr, "L%d: Compiler Error: Function '%s' expects %d arguments, got %d.\n",
                                    line, original_display_name, func_symbol->arity, call_arg_count);
                        }
                        compiler_had_error = true;
                        for (uint8_t i = 0; i < call_arg_count; ++i) writeBytecodeChunk(chunk, POP, line);
                        emitConstant(chunk, addNilConstant(chunk), line);
                    } else {
                        int nameIndex = addStringConstant(chunk, functionName);
                        writeBytecodeChunk(chunk, CALL_USER_PROC, line);
                        emitShort(chunk, (uint16_t)nameIndex, line);
                        writeBytecodeChunk(chunk, (uint8_t)call_arg_count, line);
                    }
                } else {
                    // Fallback to indirect function pointer call: push callee address and perform indirect call
                    AST tmpVar; memset(&tmpVar, 0, sizeof(AST));
                    tmpVar.type = AST_VARIABLE; tmpVar.token = node->token;
                    compileRValue(&tmpVar, chunk, line);
                    writeBytecodeChunk(chunk, CALL_INDIRECT, line);
                    writeBytecodeChunk(chunk, (uint8_t)call_arg_count, line);
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
    bool vtable_state_pushed = false;
    if (tracked_vtable_chunk != NULL) {
        vtable_state_pushed = pushVTableTrackerState(outputChunk);
    } else {
        initializeVTableTracker(outputChunk);
    }

    if (!unit_ast || !outputChunk || unit_ast->type != AST_UNIT) {
        goto vtable_cleanup;
    }
    AST* impl_block = unit_ast->extra;
    if (!impl_block || impl_block->type != AST_COMPOUND) {
        goto vtable_cleanup;
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

vtable_cleanup:
    if (vtable_state_pushed) {
        popVTableTrackerState();
    }
}

void finalizeBytecode(BytecodeChunk* chunk) {

    if (!procedure_table || !chunk || !chunk->code) return;

    for (int offset = 0; offset < chunk->count; ) {
        uint8_t opcode = chunk->code[offset];

        if (opcode == CALL) {
            // Ensure we can read the full CALL instruction
            if (offset + 5 >= chunk->count) {
                fprintf(stderr, "Compiler Error: Malformed CALL instruction at offset %d.\n", offset);
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
                    fprintf(stderr, "Compiler Error: Invalid name index in CALL at offset %d.\n", name_index);
                    compiler_had_error = true;
                    offset += 6; // Skip this malformed instruction
                    continue;
                }

                Value name_val = chunk->constants[name_index];
                if (name_val.type != TYPE_STRING) {
                    fprintf(stderr, "Compiler Error: Constant at index %d is not a string for CALL.\n", name_index);
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
                symbol_to_patch = resolveSymbolAlias(symbol_to_patch);

                if (symbol_to_patch && symbol_to_patch->is_defined) {
                    // Patch the address in place. The address occupies bytes offset+3 and offset+4.
                    patchShort(chunk, offset + 3, (uint16_t)symbol_to_patch->bytecode_address);
                } else {
                    fprintf(stderr, "Compiler Error: Procedure '%s' was called but never defined.\n", proc_name);
                    compiler_had_error = true;
                }
            }
            offset += 6; // Advance past the 6-byte CALL instruction
        } else {
            // For any other instruction, use the new helper to get the correct length and advance the offset.
            offset += getInstructionLength(chunk, offset);
        }
    }
}
