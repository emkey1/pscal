#include <string.h>
#include "backend_ast/builtin_network_api.h"
#include "vm/vm.h"

static Value networkBuiltinUnavailable(struct VM_s* vm, const char *name) {
    runtimeError(vm,
                 "%s is unavailable: this build omits libcurl-based networking.",
                 name);
    Value result;
    memset(&result, 0, sizeof(result));
    result.type = TYPE_NIL;
    return result;
}

#define PSCAL_DEFINE_NETWORK_STUB(fn)                              \
    Value fn(struct VM_s* vm, int arg_count, Value* args) {        \
        (void)arg_count;                                          \
        (void)args;                                               \
        return networkBuiltinUnavailable(vm, #fn);                \
    }

PSCAL_DEFINE_NETWORK_STUB(vmBuiltinApiSend)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinApiReceive)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinSocketCreate)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinSocketClose)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinSocketConnect)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinSocketBind)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinSocketBindAddr)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinSocketListen)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinSocketAccept)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinSocketSend)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinSocketReceive)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinSocketSetBlocking)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinSocketPoll)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinDnsLookup)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinSocketLastError)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpSession)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpClose)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpSetHeader)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpClearHeaders)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpSetOption)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpRequest)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpRequestToFile)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpGetLastHeaders)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpErrorCode)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpGetHeader)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpRequestAsync)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpRequestAsyncToFile)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpAwait)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpIsDone)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpTryAwait)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpCancel)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpGetAsyncProgress)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpGetAsyncTotal)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinHttpLastError)
PSCAL_DEFINE_NETWORK_STUB(vmBuiltinJsonGet)

#undef PSCAL_DEFINE_NETWORK_STUB
