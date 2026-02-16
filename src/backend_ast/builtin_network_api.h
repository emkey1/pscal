// src/backend_ast/builtin_network_api.h
//
//  builtin_network_api.h
//  Pscal
//
//  Created by Michael Miller on 6/10/25.
//
#ifndef BUILTIN_NETWORK_API_H
#define BUILTIN_NETWORK_API_H

#include "core/types.h"
#include "ast/ast.h"

// Prototypes for AST-based handlers

// Add VM-native prototypes later if you convert these to the new system
struct VM_s;
Value vmBuiltinApiSend(struct VM_s* vm, int arg_count, Value* args); // ADDED
Value vmBuiltinApiReceive(struct VM_s* vm, int arg_count, Value* args); // ADDED

// Socket / DNS API
Value vmBuiltinSocketCreate(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSocketClose(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSocketConnect(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSocketBind(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSocketBindAddr(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSocketListen(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSocketAccept(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSocketPeerAddr(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSocketSend(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSocketReceive(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSocketSetBlocking(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSocketPoll(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinDnsLookup(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSocketLastError(struct VM_s* vm, int arg_count, Value* args);

// HTTP Session API (sync)
Value vmBuiltinHttpSession(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHttpClose(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHttpSetHeader(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHttpClearHeaders(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHttpSetOption(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHttpRequest(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHttpRequestToFile(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHttpGetLastHeaders(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHttpErrorCode(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHttpGetHeader(struct VM_s* vm, int arg_count, Value* args);

// HTTP Async API
Value vmBuiltinHttpRequestAsync(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHttpRequestAsyncToFile(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHttpAwait(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHttpIsDone(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHttpTryAwait(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHttpCancel(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHttpGetAsyncProgress(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHttpGetAsyncTotal(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHttpLastError(struct VM_s* vm, int arg_count, Value* args);

// JSON helpers (minimal)
Value vmBuiltinJsonGet(struct VM_s* vm, int arg_count, Value* args);

#endif // BUILTIN_NETWORK_API_H
