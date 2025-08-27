#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"

void registerStringBuiltins(void) {
    extBuiltinRegisterCategory("strings");
    // no string builtins currently
}
