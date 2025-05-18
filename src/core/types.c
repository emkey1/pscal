#include "types.h"
#include <stdio.h>
#include <stdlib.h>

void setTypeValue(Value *val, VarType type) {
    if (!val) return;
#ifdef DEBUG
    if (val->type != type) {
        fprintf(stderr, "DEBUG: Changing Value type from %s to %s\n",
                varTypeToString(val->type), varTypeToString(type));
    }
#endif
    val->type = type;
}

// Infer the type of a binary operation based on operand types
VarType inferBinaryOpType(VarType left, VarType right) {
    if (left == TYPE_STRING || right == TYPE_STRING) return TYPE_STRING;
    if (left == TYPE_REAL || right == TYPE_REAL) return TYPE_REAL;
    if (left == TYPE_INTEGER && right == TYPE_INTEGER) return TYPE_INTEGER;
    if (left == TYPE_BOOLEAN && right == TYPE_BOOLEAN) return TYPE_BOOLEAN;
    if (left == TYPE_CHAR && right == TYPE_CHAR) return TYPE_STRING; // for '+'
    return TYPE_VOID; // fallback
}

