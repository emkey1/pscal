#include "disassembler/opcode_meta.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "compiler/bytecode.h"

// Mnemonic table generated from pscal-core's compiler/opcodes.def — the
// single source of truth for the opcode page.
static const char* kOpcodeNames[OPCODE_COUNT] = {
#define OP(name, value, operands, stack_in, stack_out) [value] = #name,
#include "compiler/opcodes.def"
#undef OP
};

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
