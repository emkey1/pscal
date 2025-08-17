#include "tinyc/builtins.h"
#include "backend_ast/builtin.h"

int tinyc_get_builtin_id(const char *name) {
    return getBuiltinIDForCompiler(name);
}

void tinyc_register_builtins(void) {
    registerAllBuiltins();
}
