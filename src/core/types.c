#include "types.h"
#include "utils.h"
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

    bool left_real = is_real_type(left);
    bool right_real = is_real_type(right);
    bool left_int = is_intlike_type(left);
    bool right_int = is_intlike_type(right);

    if ((left_real && right_int) || (right_real && left_int)) return TYPE_UNKNOWN;

    if (left_real && right_real) {
        if (left == TYPE_LONG_DOUBLE || right == TYPE_LONG_DOUBLE) return TYPE_LONG_DOUBLE;
        if (left == TYPE_DOUBLE || right == TYPE_DOUBLE) return TYPE_DOUBLE;
        return TYPE_FLOAT;
    }
    if (left_int && right_int) return TYPE_INT32;
    if (left == TYPE_BOOLEAN && right == TYPE_BOOLEAN) return TYPE_BOOLEAN;
    if (left == TYPE_CHAR && right == TYPE_CHAR) return TYPE_STRING; // for '+'
    return TYPE_VOID; // fallback
}

