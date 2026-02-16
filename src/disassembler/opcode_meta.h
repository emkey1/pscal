#ifndef PSCAL_OPCODE_META_H
#define PSCAL_OPCODE_META_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const char* pscalOpcodeName(uint8_t opcode);
int pscalOpcodeFromName(const char* name);
int pscalOpcodeOperandInfo(uint8_t opcode, int *exact_operand_count_out, int *minimum_operand_count_out);

#ifdef __cplusplus
}
#endif

#endif
