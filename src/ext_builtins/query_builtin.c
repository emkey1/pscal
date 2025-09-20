#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "ext_builtins/registry.h"

static Value vmBuiltinHasExtBuiltin(struct VM_s *vm, int arg_count,
                                    Value *args) {
  if (arg_count != 2) {
    runtimeError(vm, "HasExtBuiltin expects exactly 2 arguments.");
    return makeBoolean(0);
  }
  if (args[0].type != TYPE_STRING || args[1].type != TYPE_STRING) {
    runtimeError(vm, "HasExtBuiltin expects string arguments.");
    return makeBoolean(0);
  }
  const char *category = args[0].s_val;
  const char *func = args[1].s_val;
  int present = extBuiltinHasFunction(category, func);
  return makeBoolean(present);
}

void registerHasExtBuiltin(void) {
  registerVmBuiltin("hasextbuiltin", vmBuiltinHasExtBuiltin,
                    BUILTIN_TYPE_FUNCTION, "HasExtBuiltin");
}
