#include "clike/builtins.h"
#include "backend_ast/builtin.h"
#include <string.h>
#include <strings.h>

int clikeGetBuiltinID(const char *name) {
    // The VM exposes Pascal's `length` builtin for string length.  Map the
    // C-like `strlen` to that builtin so we don't need a backend change.
    if (strcasecmp(name, "strlen") == 0) {
        name = "length";
    }
    // Provide `itoa` as a wrapper around Pascal's `str` builtin.
    if (strcasecmp(name, "itoa") == 0) {
        name = "str";
    }
    // Map C-like `exit` to Pascal's `halt` so an optional exit code may be supplied.
    if (strcasecmp(name, "exit") == 0) {
        name = "halt";
    }
    if (strcasecmp(name, "remove") == 0) {
        name = "erase";
    }
    if (strcasecmp(name, "toupper") == 0) {
        name = "upcase";
    }
    return getBuiltinIDForCompiler(name);
}

void clikeRegisterBuiltins(void) {
    registerAllBuiltins();
    registerBuiltinFunction("printf", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("scanf", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("itoa", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("exit", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("remove", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("toupper", AST_FUNCTION_DECL, NULL);
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
