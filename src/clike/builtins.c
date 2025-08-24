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
    registerBuiltinFunction("exit", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("mstreamcreate", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("mstreamloadfromfile", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("mstreamsavetofile", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("mstreamfree", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("mstreambuffer", AST_FUNCTION_DECL, NULL);

    /* DOS/OS helpers */
    registerBuiltinFunction("exec", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("findfirst", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("findnext", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("getfattr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("mkdir", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("rmdir", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("getenv", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("getdate", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("gettime", AST_FUNCTION_DECL, NULL);

    /* Math helpers */
    registerBuiltinFunction("arctan", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("arcsin", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("arccos", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("cotan", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("power", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("log10", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("sinh", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("cosh", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("tanh", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("max", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("min", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("floor", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("ceil", AST_FUNCTION_DECL, NULL);
}
