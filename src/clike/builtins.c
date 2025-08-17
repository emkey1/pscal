#include "clike/builtins.h"
#include "backend_ast/builtin.h"

int clike_get_builtin_id(const char *name) {
    return getBuiltinIDForCompiler(name);
}

void clike_register_builtins(void) {
    registerAllBuiltins();
}
