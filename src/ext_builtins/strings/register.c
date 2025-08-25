#include "backend_ast/builtin.h"

void registerReverseStringBuiltin(void);

void pascal_ext_strings_init(void) { registerReverseStringBuiltin(); }
