#include "core/utils.h"
#include "backend_ast/builtin.h"

static Value vmBuiltinSwap(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "Swap expects exactly 2 arguments.");
        return makeVoid();
    }
    if (args[0].type != TYPE_POINTER || args[1].type != TYPE_POINTER) {
        runtimeError(vm, "Arguments to Swap must be variables (VAR parameters).");
        return makeVoid();
    }
    Value* varA = (Value*)args[0].ptr_val;
    Value* varB = (Value*)args[1].ptr_val;
    if (!varA || !varB) {
        runtimeError(vm, "Swap received a NIL pointer for a VAR parameter.");
        return makeVoid();
    }
    if (varA->type != varB->type) {
        runtimeError(vm, "Cannot swap variables of different types (%s and %s).",
                     varTypeToString(varA->type), varTypeToString(varB->type));
        return makeVoid();
    }
    Value temp = *varA;
    *varA = *varB;
    *varB = temp;
    return makeVoid();
}

void registerSwapBuiltin(void) {
    registerBuiltinFunction("Swap", AST_PROCEDURE_DECL, NULL);
    registerVmBuiltin("swap", vmBuiltinSwap);
}

