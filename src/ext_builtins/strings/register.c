#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"

void registerAtoiBuiltin(void);

void registerStringBuiltins(void) {
    const char *category = "strings";
    extBuiltinRegisterCategory(category);
    extBuiltinRegisterGroup(category, "conversion");
    extBuiltinRegisterFunction(category, "conversion", "Atoi");
    registerAtoiBuiltin();
}
