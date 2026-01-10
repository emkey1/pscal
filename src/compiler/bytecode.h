// src/compiler/bytecode.h
//
//  bytecode.h
//  Pscal
//
//  Created by Michael Miller on 5/18/25.
//

#ifndef PSCAL_BYTECODE_H
#define PSCAL_BYTECODE_H

#include "core/types.h" // For Value struct, as constants will be Values
#include "symbol/symbol.h" // For HashTable definition

#define GLOBAL_INLINE_CACHE_SLOT_SIZE 8
_Static_assert(sizeof(Symbol*) <= GLOBAL_INLINE_CACHE_SLOT_SIZE,
               "GLOBAL_INLINE_CACHE_SLOT_SIZE is too small for Symbol* pointers");

// --- Opcode Definitions ---
typedef enum {
    RETURN,        // Return from current function/script (implicit at end of main block)
    CONSTANT,      // Push a constant from the constant pool onto the stack
    CONSTANT16,   // For cases where the number exceeds a byte
    CONST_0,      // Push immediate integer 0
    CONST_1,      // Push immediate integer 1
    CONST_TRUE,   // Push boolean true
    CONST_FALSE,  // Push boolean false
    PUSH_IMMEDIATE_INT8, // Push signed 8-bit integer inline
    ADD,           // Pop two values, add, push result
    SUBTRACT,      // Pop two values, subtract, push result
    MULTIPLY,      // Pop two values, multiply, push result
    DIVIDE,        // Pop two values, divide, push result (handle integer/real later)
    NEGATE,        // Pop one value, negate it, push result (for unary minus)
    NOT,           // Pop one value (boolean), invert, push result
    TO_BOOL,       // Pop one value, coerce using truthiness rules, push boolean result
    EQUAL,
    NOT_EQUAL,
    GREATER,
    GREATER_EQUAL,
    LESS,
    LESS_EQUAL,
    INT_DIV,
    MOD,
    AND,
    OR,
    XOR,
    SHL,           // Bit Shift Left
    SHR,           // Bit Shift Right

    JUMP_IF_FALSE, // Pops value; if false, jumps by a 16-bit signed offset
    JUMP,          // Unconditionally jumps by a 16-bit signed offset
    SWAP,          // OPCODE to swap the top two stack items.
    DUP,           // Duplicate the top value on the stack

    // DEFINE_GLOBAL encoding used by VM/disassembler:
    //   [name_const_idx][var_type_enum][payload...]
    //   If var_type_enum == TYPE_ARRAY:
    //       [dim_count] { [lower_idx][upper_idx] }*dim_count [elem_var_type][elem_type_name_idx]
    //   Else:
    //       [type_name_const_idx] [len_const_idx if var_type_enum == TYPE_STRING]
    //         // len_const_idx references an integer constant; 0 means dynamic length
    DEFINE_GLOBAL,
    DEFINE_GLOBAL16, // 16-bit name index variant of DEFINE_GLOBAL
    GET_GLOBAL,    // Get a global variable's value (takes constant index for name)
    SET_GLOBAL,    // Set a global variable's value (takes constant index for name)
    GET_GLOBAL_ADDRESS,
    GET_GLOBAL16,  // 16-bit name index variant of GET_GLOBAL
    SET_GLOBAL16,  // 16-bit name index variant of SET_GLOBAL
    GET_GLOBAL_ADDRESS16, // 16-bit name index variant of GET_GLOBAL_ADDRESS
    GET_GLOBAL_CACHED,    // Patched variant with inline Symbol* cache
    SET_GLOBAL_CACHED,
    GET_GLOBAL16_CACHED,
    SET_GLOBAL16_CACHED,

    GET_LOCAL,     // Get local scoped variables
    SET_LOCAL,     // Set local scoped variables
    INC_LOCAL,     // Increment a local slot by 1 (peephole optimized helper)
    DEC_LOCAL,     // Decrement a local slot by 1 (peephole optimized helper)
    INIT_LOCAL_ARRAY, // Initialize local array variable
    INIT_LOCAL_FILE,  // Initialize local file variable
    INIT_LOCAL_POINTER, // Initialize local pointer variable
    INIT_LOCAL_STRING, // Initialize local fixed-length string variable
    INIT_FIELD_ARRAY, // Initialize object field array
    GET_LOCAL_ADDRESS,

    GET_UPVALUE,
    SET_UPVALUE,
    GET_UPVALUE_ADDRESS,

    GET_FIELD_ADDRESS,
    GET_FIELD_ADDRESS16,
    LOAD_FIELD_VALUE_BY_NAME,   // Pops base record/pointer, looks up field by name (1-byte const index) and pushes its value
    LOAD_FIELD_VALUE_BY_NAME16, // Pops base record/pointer, looks up field by name (2-byte const index) and pushes its value
    GET_ELEMENT_ADDRESS,
    GET_ELEMENT_ADDRESS_CONST, // Pops an array base and pushes address using a constant flat offset
    LOAD_ELEMENT_VALUE,  // Pops array/pointer after its indices and pushes a copy of the addressed element's value
    LOAD_ELEMENT_VALUE_CONST, // Pops array/pointer and loads element at constant flat offset
    GET_CHAR_ADDRESS, // NEW: Gets address of char in string for s[i] := 'X'
    SET_INDIRECT,
    GET_INDIRECT,
    
    IN, // For set membership
    
    GET_CHAR_FROM_STRING, //  Pops index, pops string, pushes character.

    // --- Object support --------------------------------------------------
    // Allocate a record/object with the given number of fields.  The first
    // slot is always reserved for the hidden __vtable pointer.
    ALLOC_OBJECT,       // Operand: 1-byte field count
    ALLOC_OBJECT16,     // Operand: 2-byte field count
    // Fetch the address of a field using a zero based offset.  Pops the base
    // pointer/record from the stack and pushes the address of the selected
    // field.
    GET_FIELD_OFFSET,   // Operand: 1-byte field index
    GET_FIELD_OFFSET16, // Operand: 2-byte field index
    LOAD_FIELD_VALUE,   // Pops the base record/pointer and pushes a copy of the field value (1-byte offset)
    LOAD_FIELD_VALUE16, // Pops the base record/pointer and pushes a copy of the field value (2-byte offset)

    // For now, built-ins might be handled specially, or we can add a generic call
    CALL_BUILTIN,  // Placeholder for calling built-in functions
                      // Operands: 2-byte name index, 1-byte argument count
    
    CALL_BUILTIN_PROC, // For void built-in procedures. Operands: 2-byte builtin_id, 2-byte name index, 1-byte arg count
    CALL_USER_PROC,    // For user-defined procedures/functions. Operand1: name_const_idx, Operand2: arg_count

    CALL_HOST,

    POP,           // Pop the top value from the stack (e.g., after an expression statement)
    CALL,          // For user-defined procedure/function calls.
                      // Operands: 2-byte name_idx, 2-byte address, 1-byte arg count
    CALL_INDIRECT,     // Indirect call via address on stack. Operands: 1-byte arg count
    CALL_METHOD,       // Virtual method call using object's V-table
                          // Operands: 1-byte method index, 1-byte arg count
    PROC_CALL_INDIRECT,// Indirect call used in statement context; discards any return value. Operands: 1-byte arg count
    HALT,          // Stop the VM (though RETURN from main might suffice)
    EXIT,          // Early exit from the current function without halting the VM
    FORMAT_VALUE,  // Format the value on top of the stack. Operands: width (byte), precision (byte)

    // --- Threading Opcodes ---
    THREAD_CREATE,      // Create a new lightweight thread. Operand: 2-byte entry offset
    THREAD_JOIN,        // Join on a thread. Operand: none (pops thread id from stack)

    // --- Mutex Opcodes ---
    MUTEX_CREATE,       // Create a standard mutex. Pushes mutex id
    RCMUTEX_CREATE,     // Create a recursive mutex. Pushes mutex id
    MUTEX_LOCK,         // Lock mutex whose id is on top of stack
    MUTEX_UNLOCK,       // Unlock mutex whose id is on top of stack
    MUTEX_DESTROY,      // Destroy mutex whose id is on top of stack

    OPCODE_COUNT        // Total number of opcodes (must remain last)

} OpCode;

// --- Bytecode Chunk Structure ---
// A "chunk" represents a compiled piece of code (e.g., a procedure, function, or the main program block)
typedef struct {
    uint32_t version;   // VM bytecode version this chunk targets
    int count;          // Number of bytes currently in use in 'code'
    int capacity;       // Allocated capacity for 'code'
    uint8_t* code;      // The array of bytecode instructions and operands

    int constants_count; // Number of constants currently in use
    int constants_capacity; // Allocated capacity for 'constants'
    Value* constants;   // Array of constants (Value structs: numbers, strings)
    int* builtin_lowercase_indices; // Maps string constant indices to their lowercase copies (-1 if not a builtin)
    struct Symbol_s** global_symbol_cache;

    // Optional: For debugging runtime errors
    int* lines;         // Array storing the source line number for each byte of code
} BytecodeChunk;

// --- Function Prototypes for BytecodeChunk ---
void initBytecodeChunk(BytecodeChunk* chunk);
void writeBytecodeChunk(BytecodeChunk* chunk, uint8_t byte, int line); // Add byte to chunk
void freeBytecodeChunk(BytecodeChunk* chunk);
int addConstantToChunk(BytecodeChunk* chunk, const Value* value); // Add a value to constant pool, return index
void disassembleBytecodeChunk(BytecodeChunk* chunk, const char* name, HashTable* procedureTable);
const char* bytecodeDisplayNameForPath(const char* path);
int disassembleInstruction(BytecodeChunk* chunk, int offset, HashTable* procedureTable);
void emitShort(BytecodeChunk* chunk, uint16_t value, int line);
void emitInt32(BytecodeChunk* chunk, uint32_t value, int line);
void patchShort(BytecodeChunk* chunk, int offset_in_code, uint16_t value);
int getInstructionLength(BytecodeChunk* chunk, int offset);
void setBuiltinLowercaseIndex(BytecodeChunk* chunk, int original_idx, int lowercase_idx);
int getBuiltinLowercaseIndex(const BytecodeChunk* chunk, int original_idx);
void writeInlineCacheSlot(BytecodeChunk* chunk, int line);

#endif // PSCAL_BYTECODE_H
