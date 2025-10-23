#ifndef THREAD_WRAPPERS_H
#define THREAD_WRAPPERS_H

#include "core/types.h"

struct VM_s;

Value builtinThreadSpawnNamedWrapper(struct VM_s* vm, int arg_count, Value* args);
Value builtinThreadPoolSubmitWrapper(struct VM_s* vm, int arg_count, Value* args);

#endif
