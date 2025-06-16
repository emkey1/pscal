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
#include "frontend/ast.h"

// Prototypes for AST-based handlers
Value executeBuiltinAPISend(AST *node);
Value executeBuiltinAPIReceive(AST *node);

// Add VM-native prototypes later if you convert these to the new system
struct VM_s;
Value vm_builtin_api_send(struct VM_s* vm, int arg_count, Value* args); // ADDED
Value vm_builtin_api_receive(struct VM_s* vm, int arg_count, Value* args); // ADDED

#endif // BUILTIN_NETWORK_API_H
