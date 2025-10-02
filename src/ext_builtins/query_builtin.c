#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "ext_builtins/registry.h"
#include <string.h>
#include <strings.h>

static const char *kDefaultGroupName = "default";

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

static size_t countFunctionsAcrossGroups(const char *category) {
  size_t total = 0;
  if (extBuiltinHasGroup(category, NULL)) {
    total += extBuiltinGetFunctionCount(category, NULL);
  }
  size_t group_count = extBuiltinGetGroupCount(category);
  for (size_t i = 0; i < group_count; ++i) {
    const char *group = extBuiltinGetGroupName(category, i);
    if (!group) {
      continue;
    }
    total += extBuiltinGetFunctionCount(category, group);
  }
  return total;
}

static const char *getFunctionNameAcrossGroups(const char *category,
                                               size_t index,
                                               const char **out_group) {
  if (extBuiltinHasGroup(category, NULL)) {
    size_t default_count = extBuiltinGetFunctionCount(category, NULL);
    if (index < default_count) {
      if (out_group) {
        *out_group = kDefaultGroupName;
      }
      return extBuiltinGetFunctionName(category, NULL, index);
    }
    index -= default_count;
  }

  size_t group_count = extBuiltinGetGroupCount(category);
  for (size_t i = 0; i < group_count; ++i) {
    const char *group = extBuiltinGetGroupName(category, i);
    if (!group) {
      continue;
    }
    size_t group_size = extBuiltinGetFunctionCount(category, group);
    if (index < group_size) {
      if (out_group) {
        *out_group = group;
      }
      return extBuiltinGetFunctionName(category, group, index);
    }
    index -= group_size;
  }
  return NULL;
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

static Value vmBuiltinExtBuiltinGroupCount(struct VM_s *vm, int arg_count,
                                          Value *args) {
  if (arg_count != 1 || args[0].type != TYPE_STRING) {
    runtimeError(vm,
                 "ExtBuiltinGroupCount expects a single string argument.");
    return makeInt(0);
  }
  size_t count = extBuiltinGetGroupCount(args[0].s_val);
  if (extBuiltinHasGroup(args[0].s_val, NULL)) {
    ++count; /* include the default bucket */
  }
  return makeInt((long long)count);
}

static Value vmBuiltinExtBuiltinGroupName(struct VM_s *vm, int arg_count,
                                         Value *args) {
  if (arg_count != 2 || args[0].type != TYPE_STRING || !IS_INTLIKE(args[1])) {
    runtimeError(vm,
                 "ExtBuiltinGroupName expects a string category and integer index.");
    return makeString("");
  }
  long long idx = AS_INTEGER(args[1]);
  if (idx < 0) {
    return makeString("");
  }
  const char *category = args[0].s_val;
  size_t default_groups = extBuiltinHasGroup(category, NULL) ? 1 : 0;
  if ((size_t)idx < default_groups) {
    return makeString(kDefaultGroupName);
  }
  size_t adj_index = (size_t)idx - default_groups;
  const char *name = extBuiltinGetGroupName(category, adj_index);
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
  size_t count = countFunctionsAcrossGroups(args[0].s_val);
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
      getFunctionNameAcrossGroups(args[0].s_val, (size_t)idx, NULL);
  if (!name) {
    return makeString("");
  }
  return makeString(name);
}

static Value vmBuiltinExtBuiltinGroupFunctionCount(struct VM_s *vm,
                                                   int arg_count,
                                                   Value *args) {
  if (arg_count != 2 || args[0].type != TYPE_STRING || args[1].type != TYPE_STRING) {
    runtimeError(vm, "ExtBuiltinGroupFunctionCount expects two string arguments.");
    return makeInt(0);
  }
  size_t count = extBuiltinGetFunctionCount(args[0].s_val, args[1].s_val);
  if (!count && strcasecmp(args[1].s_val, kDefaultGroupName) == 0) {
    count = extBuiltinGetFunctionCount(args[0].s_val, NULL);
  }
  return makeInt((long long)count);
}

static Value vmBuiltinExtBuiltinGroupFunctionName(struct VM_s *vm,
                                                  int arg_count,
                                                  Value *args) {
  if (arg_count != 3 || args[0].type != TYPE_STRING || args[1].type != TYPE_STRING ||
      !IS_INTLIKE(args[2])) {
    runtimeError(vm, "ExtBuiltinGroupFunctionName expects category, group, and index.");
    return makeString("");
  }
  long long idx = AS_INTEGER(args[2]);
  if (idx < 0) {
    return makeString("");
  }
  const char *group = args[1].s_val;
  const char *category = args[0].s_val;
  const char *name = NULL;
  if (strcasecmp(group, kDefaultGroupName) == 0) {
    name = extBuiltinGetFunctionName(category, NULL, (size_t)idx);
  } else {
    name = extBuiltinGetFunctionName(category, group, (size_t)idx);
  }
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
  registerVmBuiltin("extbuiltingroupcount", vmBuiltinExtBuiltinGroupCount,
                    BUILTIN_TYPE_FUNCTION, "ExtBuiltinGroupCount");
  registerVmBuiltin("extbuiltingroupname", vmBuiltinExtBuiltinGroupName,
                    BUILTIN_TYPE_FUNCTION, "ExtBuiltinGroupName");
  registerVmBuiltin("extbuiltinfunctioncount",
                    vmBuiltinExtBuiltinFunctionCount, BUILTIN_TYPE_FUNCTION,
                    "ExtBuiltinFunctionCount");
  registerVmBuiltin("extbuiltinfunctionname",
                    vmBuiltinExtBuiltinFunctionName, BUILTIN_TYPE_FUNCTION,
                    "ExtBuiltinFunctionName");
  registerVmBuiltin("extbuiltingroupfunctioncount",
                    vmBuiltinExtBuiltinGroupFunctionCount,
                    BUILTIN_TYPE_FUNCTION, "ExtBuiltinGroupFunctionCount");
  registerVmBuiltin("extbuiltingroupfunctionname",
                    vmBuiltinExtBuiltinGroupFunctionName,
                    BUILTIN_TYPE_FUNCTION, "ExtBuiltinGroupFunctionName");
}
