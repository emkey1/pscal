#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"

void registerUserBuiltins(void) {
    extBuiltinRegisterCategory("user");
    // add calls to user-defined builtins here
}
