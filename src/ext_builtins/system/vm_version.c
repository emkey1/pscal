#include "core/utils.h"
#include "core/version.h"
#include "vm/vm.h"
#include "backend_ast/builtin.h"

static Value vmBuiltinVMVersion(struct VM_s* vm, int arg_count, Value* args) {
    (void)vm; (void)args;
    return arg_count == 0 ? makeInt(PSCAL_VM_VERSION) : makeInt(-1);
}

static Value vmBuiltinBytecodeVersion(struct VM_s* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0 || !vm || !vm->chunk) return makeInt(-1);
    return makeInt(vm->chunk->version);
}

void registerVmVersionBuiltin(void) {
    registerBuiltinFunction("VMVersion", AST_FUNCTION_DECL, NULL);
    registerVmBuiltin("vmversion", vmBuiltinVMVersion);
    registerBuiltinFunction("BytecodeVersion", AST_FUNCTION_DECL, NULL);
    registerVmBuiltin("bytecodeversion", vmBuiltinBytecodeVersion);
}
