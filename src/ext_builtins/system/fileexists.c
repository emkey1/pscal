#include "backend_ast/builtin.h"
#include "core/utils.h"
#include <stdio.h>

static Value vmBuiltinFileExists(struct VM_s *vm, int arg_count, Value *args) {
  if (arg_count != 1) {
    runtimeError(vm, "FileExists expects exactly 1 argument.");
    return makeBoolean(0);
  }
  if (args[0].type != TYPE_STRING) {
    runtimeError(vm, "FileExists argument must be a string.");
    return makeBoolean(0);
  }
  const char *path = args[0].s_val;
  if (!path) {
    runtimeError(vm, "FileExists received NIL string.");
    return makeBoolean(0);
  }
  FILE *f = fopen(path, "r");
  if (f) {
    fclose(f);
    return makeBoolean(1);
  }
  return makeBoolean(0);
}

void registerFileExistsBuiltin(void) {
  registerBuiltinFunction("FileExists", AST_FUNCTION_DECL, NULL);
  registerVmBuiltin("fileexists", vmBuiltinFileExists);
}
