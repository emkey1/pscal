#ifndef VM_STRING_SENTINELS_H
#define VM_STRING_SENTINELS_H

#include "ast/ast.h"

// Special sentinel values stored in Value.base_type_node to distinguish
// pointers that reference characters within a string buffer, packed byte
// array elements, or the synthetic element 0 used to expose a string's
// length via indexing.
#define STRING_CHAR_PTR_SENTINEL   ((AST*)0xDEADBEEF)
#define STRING_LENGTH_SENTINEL     ((AST*)0xFEEDBEEF)
#define BYTE_ARRAY_PTR_SENTINEL    ((AST*)0xBAADF00D)
#define SERIALIZED_CHAR_PTR_SENTINEL ((AST*)0xC0DEC0DE)
#define SHELL_FUNCTION_PTR_SENTINEL ((AST*)0x5155464EU)
#define OPAQUE_POINTER_SENTINEL    ((AST*)0x0BADC0DE)

#endif // VM_STRING_SENTINELS_H
