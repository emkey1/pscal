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

// --- Opcode Definitions ---
typedef enum {
    OP_RETURN,        // Return from current function/script (implicit at end of main block)
    OP_CONSTANT,      // Push a constant from the constant pool onto the stack
    OP_CONSTANT16,   // For cases where the number exceeds a byte
    OP_ADD,           // Pop two values, add, push result
    OP_SUBTRACT,      // Pop two values, subtract, push result
    OP_MULTIPLY,      // Pop two values, multiply, push result
    OP_DIVIDE,        // Pop two values, divide, push result (handle integer/real later)
    OP_NEGATE,        // Pop one value, negate it, push result (for unary minus)
    OP_NOT,           // Pop one value (boolean), invert, push result
    OP_EQUAL,
    OP_NOT_EQUAL,
    OP_GREATER,
    OP_GREATER_EQUAL,
    OP_LESS,
    OP_LESS_EQUAL,
    OP_INT_DIV,
    OP_MOD,
    OP_AND,
    OP_OR,
    OP_SHL,           // Bit Shift Left
    OP_SHR,           // Bit Shift Right

    OP_JUMP_IF_FALSE, // Pops value; if false, jumps by a 16-bit signed offset
    OP_JUMP,          // Unconditionally jumps by a 16-bit signed offset
    OP_SWAP,          // OPCODE to swap the top two stack items.
    OP_DUP,           // Duplicate the top value on the stack

    // OP_DEFINE_GLOBAL encoding used by VM/disassembler:
    //   [name_const_idx][var_type_enum][payload...]
    //   If var_type_enum == TYPE_ARRAY:
    //       [dim_count] { [lower_idx][upper_idx] }*dim_count [elem_var_type][elem_type_name_idx]
    //   Else:
    //       [type_name_const_idx] [len_const_idx if var_type_enum == TYPE_STRING]
    //         // len_const_idx references an integer constant; 0 means dynamic length
    OP_DEFINE_GLOBAL,
    OP_DEFINE_GLOBAL16, // 16-bit name index variant of OP_DEFINE_GLOBAL
    OP_GET_GLOBAL,    // Get a global variable's value (takes constant index for name)
    OP_SET_GLOBAL,    // Set a global variable's value (takes constant index for name)
    OP_GET_GLOBAL_ADDRESS,
    OP_GET_GLOBAL16,  // 16-bit name index variant of OP_GET_GLOBAL
    OP_SET_GLOBAL16,  // 16-bit name index variant of OP_SET_GLOBAL
    OP_GET_GLOBAL_ADDRESS16, // 16-bit name index variant of OP_GET_GLOBAL_ADDRESS

    OP_GET_LOCAL,     // Get local scoped variables
    OP_SET_LOCAL,     // Set local scoped variables
    OP_INIT_LOCAL_ARRAY, // Initialize local array variable
    OP_INIT_LOCAL_FILE,  // Initialize local file variable
    OP_GET_LOCAL_ADDRESS,

    OP_GET_FIELD_ADDRESS,
    OP_GET_FIELD_ADDRESS16,
    OP_GET_ELEMENT_ADDRESS,
    OP_GET_CHAR_ADDRESS, // NEW: Gets address of char in string for s[i] := 'X'
    OP_SET_INDIRECT,
    OP_GET_INDIRECT,
    
    OP_IN, // For set membership
    
    OP_GET_CHAR_FROM_STRING, //  Pops index, pops string, pushes character.

    // For now, built-ins might be handled specially, or we can add a generic call
    OP_CALL_BUILTIN,  // Placeholder for calling built-in functions
                      // Needs: index of builtin, argument count
    
    OP_CALL_BUILTIN_PROC, // For void built-in procedures. Operand1: builtin_id, Operand2: arg_count
    OP_CALL_USER_PROC,    // For user-defined procedures/functions. Operand1: name_const_idx, Operand2: arg_count

    OP_WRITE_LN,      // Specific opcode for WriteLn for now (simpler than generic call)
                      // Operand: number of arguments to pop from stack for writeln
    OP_WRITE,         // Specific opcode for Write
    OP_CALL_HOST,

    OP_POP,           // Pop the top value from the stack (e.g., after an expression statement)
    OP_CALL,          // For user-defined procedure/function calls.
                      // Operands: 1-byte name_idx, 2-byte address, 1-byte arg count
    OP_HALT,          // Stop the VM (though OP_RETURN from main might suffice)
    OP_FORMAT_VALUE   // Format the value on top of the stack. Operands: width (byte), precision (byte)
    
} OpCode;

// --- Bytecode Chunk Structure ---
// A "chunk" represents a compiled piece of code (e.g., a procedure, function, or the main program block)
typedef struct {
    int count;          // Number of bytes currently in use in 'code'
    int capacity;       // Allocated capacity for 'code'
    uint8_t* code;      // The array of bytecode instructions and operands

    int constants_count; // Number of constants currently in use
    int constants_capacity; // Allocated capacity for 'constants'
    Value* constants;   // Array of constants (Value structs: numbers, strings)

    // Optional: For debugging runtime errors
    int* lines;         // Array storing the source line number for each byte of code
} BytecodeChunk;

// --- Function Prototypes for BytecodeChunk ---
void initBytecodeChunk(BytecodeChunk* chunk);
void writeBytecodeChunk(BytecodeChunk* chunk, uint8_t byte, int line); // Add byte to chunk
void freeBytecodeChunk(BytecodeChunk* chunk);
int addConstantToChunk(BytecodeChunk* chunk, const Value* value); // Add a value to constant pool, return index
void disassembleBytecodeChunk(BytecodeChunk* chunk, const char* name, HashTable* procedureTable);
int disassembleInstruction(BytecodeChunk* chunk, int offset, HashTable* procedureTable);
void emitShort(BytecodeChunk* chunk, uint16_t value, int line);
void patchShort(BytecodeChunk* chunk, int offset_in_code, uint16_t value);
int getInstructionLength(BytecodeChunk* chunk, int offset);

#endif // PSCAL_BYTECODE_H
