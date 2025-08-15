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

// Add VM-native prototypes later if you convert these to the new system
struct VM_s;
Value vmBuiltinApiSend(struct VM_s* vm, int arg_count, Value* args); // ADDED
Value vmBuiltinApiReceive(struct VM_s* vm, int arg_count, Value* args); // ADDED

#endif // BUILTIN_NETWORK_API_H
