#include <unistd.h>
#include "core/utils.h"
#include "backend_ast/builtin.h"

static Value vmBuiltinGetPid(struct VM_s* vm, int arg_count, Value* args) {
    (void)vm; (void)args;
    return arg_count == 0 ? makeInt(getpid()) : makeInt(-1);
}

void registerExtendedBuiltins(void) {
    registerBuiltinFunction("GetPid", AST_FUNCTION_DECL, NULL);
    registerVmBuiltin("getpid", vmBuiltinGetPid);
}
