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

static Value vmBuiltinExtBuiltinCategoryCount(struct VM_s *vm, int arg_count,
                                              Value *args) {
  (void)args;
  if (arg_count != 0) {
    runtimeError(vm, "ExtBuiltinCategoryCount expects no arguments.");
    return makeInt(0);
  }
  size_t count = extBuiltinGetCategoryCount();
  return makeInt((long long)count);
}

static Value vmBuiltinExtBuiltinCategoryName(struct VM_s *vm, int arg_count,
                                             Value *args) {
  if (arg_count != 1 || !IS_INTLIKE(args[0])) {
    runtimeError(vm,
                 "ExtBuiltinCategoryName expects a single integer argument.");
    return makeString("");
  }
  long long idx = AS_INTEGER(args[0]);
  if (idx < 0) {
    return makeString("");
  }
  const char *name = extBuiltinGetCategoryName((size_t)idx);
  if (!name) {
    return makeString("");
  }
  return makeString(name);
}

static Value vmBuiltinExtBuiltinFunctionCount(struct VM_s *vm, int arg_count,
                                              Value *args) {
  if (arg_count != 1 || args[0].type != TYPE_STRING) {
    runtimeError(vm,
                 "ExtBuiltinFunctionCount expects a single string argument.");
    return makeInt(0);
  }
  size_t count = extBuiltinGetFunctionCount(args[0].s_val);
  return makeInt((long long)count);
}

static Value vmBuiltinExtBuiltinFunctionName(struct VM_s *vm, int arg_count,
                                             Value *args) {
  if (arg_count != 2 || args[0].type != TYPE_STRING || !IS_INTLIKE(args[1])) {
    runtimeError(
        vm,
        "ExtBuiltinFunctionName expects a string category and integer index.");
    return makeString("");
  }
  long long idx = AS_INTEGER(args[1]);
  if (idx < 0) {
    return makeString("");
  }
  const char *name =
      extBuiltinGetFunctionName(args[0].s_val, (size_t)idx);
  if (!name) {
    return makeString("");
  }
  return makeString(name);
}

void registerExtBuiltinQueryBuiltins(void) {
  registerVmBuiltin("hasextbuiltin", vmBuiltinHasExtBuiltin,
                    BUILTIN_TYPE_FUNCTION, "HasExtBuiltin");
  registerVmBuiltin("extbuiltincategorycount",
                    vmBuiltinExtBuiltinCategoryCount, BUILTIN_TYPE_FUNCTION,
                    "ExtBuiltinCategoryCount");
  registerVmBuiltin("extbuiltincategoryname",
                    vmBuiltinExtBuiltinCategoryName, BUILTIN_TYPE_FUNCTION,
                    "ExtBuiltinCategoryName");
  registerVmBuiltin("extbuiltinfunctioncount",
                    vmBuiltinExtBuiltinFunctionCount, BUILTIN_TYPE_FUNCTION,
                    "ExtBuiltinFunctionCount");
  registerVmBuiltin("extbuiltinfunctionname",
                    vmBuiltinExtBuiltinFunctionName, BUILTIN_TYPE_FUNCTION,
                    "ExtBuiltinFunctionName");
}
