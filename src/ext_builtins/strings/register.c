#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"

void registerAtoiBuiltin(void);

void registerStringBuiltins(void) {
    extBuiltinRegisterCategory("strings");
    extBuiltinRegisterFunction("strings", "Atoi");
    registerAtoiBuiltin();
}
