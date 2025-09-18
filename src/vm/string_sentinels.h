#ifndef VM_STRING_SENTINELS_H
#define VM_STRING_SENTINELS_H

#include "ast/ast.h"

// Special sentinel values stored in Value.base_type_node to distinguish
// pointers that reference characters within a string buffer (as opposed to
// general pointers to Value instances) or the synthetic element 0 used to
// expose a string's length via indexing.
#define STRING_CHAR_PTR_SENTINEL   ((AST*)0xDEADBEEF)
#define STRING_LENGTH_SENTINEL     ((AST*)0xFEEDBEEF)

#endif // VM_STRING_SENTINELS_H
