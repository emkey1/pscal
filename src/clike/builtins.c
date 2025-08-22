#include "clike/builtins.h"
#include "backend_ast/builtin.h"
#include <string.h>

int clike_get_builtin_id(const char *name) {
    // The VM exposes Pascal's `length` builtin for string length.  Map the
    // C-like `strlen` to that builtin so we don't need a backend change.
    if (strcmp(name, "strlen") == 0) {
        name = "length";
    }
    // Map C-like `exit` to Pascal's `halt` so an optional exit code may be supplied.
    if (strcmp(name, "exit") == 0) {
        name = "halt";
    }
    return getBuiltinIDForCompiler(name);
}

void clike_register_builtins(void) {
    registerAllBuiltins();
    registerBuiltinFunction("printf", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("scanf", AST_FUNCTION_DECL, NULL);
}
