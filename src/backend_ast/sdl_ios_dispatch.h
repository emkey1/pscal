#ifndef PSCAL_SDL_IOS_DISPATCH_H
#define PSCAL_SDL_IOS_DISPATCH_H

#if defined(SDL) && defined(PSCAL_TARGET_IOS)

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <string.h>
#include "core/types.h"

struct VM_s;

typedef Value (*PscalSdlVmBuiltin)(struct VM_s* vm, int arg_count, Value* args);

typedef struct {
    PscalSdlVmBuiltin function;
    struct VM_s* vm;
    int arg_count;
    Value* args;
    Value result;
} PscalSdlDispatchContext;

static inline void pscalInvokeSdlBuiltin(void* context_ptr) {
    PscalSdlDispatchContext* context = (PscalSdlDispatchContext*)context_ptr;
    context->result = context->function(context->vm, context->arg_count, context->args);
}

static inline Value pscalRunSdlBuiltinOnMainQueue(PscalSdlVmBuiltin function,
                                                  struct VM_s* vm,
                                                  int arg_count,
                                                  Value* args) {
    if (pthread_main_np() != 0) {
        return function(vm, arg_count, args);
    }
    PscalSdlDispatchContext context = {
        .function = function,
        .vm = vm,
        .arg_count = arg_count,
        .args = args,
    };
    memset(&context.result, 0, sizeof(context.result));
    dispatch_sync_f(dispatch_get_main_queue(), &context, pscalInvokeSdlBuiltin);
    return context.result;
}

#define PSCAL_DEFINE_IOS_SDL_BUILTIN(name) \
    static Value name##Impl(struct VM_s* vm, int arg_count, Value* args); \
    Value name(struct VM_s* vm, int arg_count, Value* args) { \
        return pscalRunSdlBuiltinOnMainQueue(name##Impl, vm, arg_count, args); \
    } \
    static Value name##Impl(struct VM_s* vm, int arg_count, Value* args)

#else
#define PSCAL_DEFINE_IOS_SDL_BUILTIN(name) \
    Value name(struct VM_s* vm, int arg_count, Value* args)
#endif

#endif // PSCAL_SDL_IOS_DISPATCH_H
