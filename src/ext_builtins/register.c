#include "backend_ast/builtin.h"

void pascal_ext_math_init(void);
void pascal_ext_strings_init(void);
void pascal_ext_system_init(void);
void pascal_ext_user_init(void);

void registerExtendedBuiltins(void) {
#ifdef ENABLE_EXT_BUILTIN_MATH
  pascal_ext_math_init();
#endif
#ifdef ENABLE_EXT_BUILTIN_STRINGS
  pascal_ext_strings_init();
#endif
#ifdef ENABLE_EXT_BUILTIN_SYSTEM
  pascal_ext_system_init();
#endif
#ifdef ENABLE_EXT_BUILTIN_USER
  pascal_ext_user_init();
#endif
}
