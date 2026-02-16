#include "disassembler/opcode_meta.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "compiler/bytecode.h"

#define PSCAL_OPCODE_LIST(X) \
    X(RETURN) \
    X(CONSTANT) \
    X(CONSTANT16) \
    X(CONST_0) \
    X(CONST_1) \
    X(CONST_TRUE) \
    X(CONST_FALSE) \
    X(PUSH_IMMEDIATE_INT8) \
    X(ADD) \
    X(SUBTRACT) \
    X(MULTIPLY) \
    X(DIVIDE) \
    X(NEGATE) \
    X(NOT) \
    X(TO_BOOL) \
    X(EQUAL) \
    X(NOT_EQUAL) \
    X(GREATER) \
    X(GREATER_EQUAL) \
    X(LESS) \
    X(LESS_EQUAL) \
    X(INT_DIV) \
    X(MOD) \
    X(AND) \
    X(OR) \
    X(XOR) \
    X(SHL) \
    X(SHR) \
    X(JUMP_IF_FALSE) \
    X(JUMP) \
    X(SWAP) \
    X(DUP) \
    X(DEFINE_GLOBAL) \
    X(DEFINE_GLOBAL16) \
    X(GET_GLOBAL) \
    X(SET_GLOBAL) \
    X(GET_GLOBAL_ADDRESS) \
    X(GET_GLOBAL16) \
    X(SET_GLOBAL16) \
    X(GET_GLOBAL_ADDRESS16) \
    X(GET_GLOBAL_CACHED) \
    X(SET_GLOBAL_CACHED) \
    X(GET_GLOBAL16_CACHED) \
    X(SET_GLOBAL16_CACHED) \
    X(GET_LOCAL) \
    X(SET_LOCAL) \
    X(INC_LOCAL) \
    X(DEC_LOCAL) \
    X(INIT_LOCAL_ARRAY) \
    X(INIT_LOCAL_FILE) \
    X(INIT_LOCAL_POINTER) \
    X(INIT_LOCAL_STRING) \
    X(INIT_FIELD_ARRAY) \
    X(GET_LOCAL_ADDRESS) \
    X(GET_UPVALUE) \
    X(SET_UPVALUE) \
    X(GET_UPVALUE_ADDRESS) \
    X(GET_FIELD_ADDRESS) \
    X(GET_FIELD_ADDRESS16) \
    X(LOAD_FIELD_VALUE_BY_NAME) \
    X(LOAD_FIELD_VALUE_BY_NAME16) \
    X(GET_ELEMENT_ADDRESS) \
    X(GET_ELEMENT_ADDRESS_CONST) \
    X(LOAD_ELEMENT_VALUE) \
    X(LOAD_ELEMENT_VALUE_CONST) \
    X(GET_CHAR_ADDRESS) \
    X(SET_INDIRECT) \
    X(GET_INDIRECT) \
    X(IN) \
    X(GET_CHAR_FROM_STRING) \
    X(ALLOC_OBJECT) \
    X(ALLOC_OBJECT16) \
    X(GET_FIELD_OFFSET) \
    X(GET_FIELD_OFFSET16) \
    X(LOAD_FIELD_VALUE) \
    X(LOAD_FIELD_VALUE16) \
    X(CALL_BUILTIN) \
    X(CALL_BUILTIN_PROC) \
    X(CALL_USER_PROC) \
    X(CALL_HOST) \
    X(POP) \
    X(CALL) \
    X(CALL_INDIRECT) \
    X(CALL_METHOD) \
    X(PROC_CALL_INDIRECT) \
    X(HALT) \
    X(EXIT) \
    X(FORMAT_VALUE) \
    X(THREAD_CREATE) \
    X(THREAD_JOIN) \
    X(MUTEX_CREATE) \
    X(RCMUTEX_CREATE) \
    X(MUTEX_LOCK) \
    X(MUTEX_UNLOCK) \
    X(MUTEX_DESTROY)

#define OPCODE_NAME_ENTRY(op) #op,
static const char* kOpcodeNames[] = {
    PSCAL_OPCODE_LIST(OPCODE_NAME_ENTRY)
};
#undef OPCODE_NAME_ENTRY

_Static_assert((sizeof(kOpcodeNames) / sizeof(kOpcodeNames[0])) == OPCODE_COUNT,
               "Opcode name table must match opcode enum count");

static int equalsIgnoreCase(const char* a, const char* b) {
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (tolower(ca) != tolower(cb)) {
            return 0;
        }
    }
    return *a == '\0' && *b == '\0';
}

const char* pscalOpcodeName(uint8_t opcode) {
    if (opcode >= OPCODE_COUNT) {
        return NULL;
    }
    return kOpcodeNames[opcode];
}

int pscalOpcodeFromName(const char* name) {
    if (!name || !*name) {
        return -1;
    }
    for (size_t i = 0; i < (sizeof(kOpcodeNames) / sizeof(kOpcodeNames[0])); ++i) {
        if (equalsIgnoreCase(name, kOpcodeNames[i])) {
            return (int)i;
        }
    }
    if (equalsIgnoreCase(name, "PUSH_IMM_I8")) {
        return PUSH_IMMEDIATE_INT8;
    }
    return -1;
}

static int opcodeLengthAssumingZeroOperands(uint8_t opcode) {
    uint8_t code[32];
    int lines[32];
    memset(code, 0, sizeof(code));
    memset(lines, 0, sizeof(lines));
    code[0] = opcode;

    BytecodeChunk chunk;
    memset(&chunk, 0, sizeof(chunk));
    chunk.code = code;
    chunk.lines = lines;
    chunk.count = (int)sizeof(code);
    chunk.capacity = chunk.count;
    return getInstructionLength(&chunk, 0);
}

int pscalOpcodeOperandInfo(uint8_t opcode, int *exact_operand_count_out, int *minimum_operand_count_out) {
    if (exact_operand_count_out) {
        *exact_operand_count_out = -1;
    }
    if (minimum_operand_count_out) {
        *minimum_operand_count_out = 0;
    }

    if (opcode >= OPCODE_COUNT) {
        return 0;
    }

    switch (opcode) {
        case INIT_LOCAL_ARRAY:
        case INIT_FIELD_ARRAY:
            if (minimum_operand_count_out) {
                *minimum_operand_count_out = 5;
            }
            return 1;
        case DEFINE_GLOBAL:
            if (minimum_operand_count_out) {
                *minimum_operand_count_out = 4;
            }
            return 1;
        case DEFINE_GLOBAL16:
            if (minimum_operand_count_out) {
                *minimum_operand_count_out = 5;
            }
            return 1;
        default:
            break;
    }

    int inst_len = opcodeLengthAssumingZeroOperands(opcode);
    if (inst_len <= 0) {
        return 0;
    }
    if (exact_operand_count_out) {
        *exact_operand_count_out = inst_len - 1;
    }
    if (minimum_operand_count_out) {
        *minimum_operand_count_out = inst_len - 1;
    }
    return 1;
}
