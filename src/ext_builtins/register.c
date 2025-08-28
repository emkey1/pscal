#include "backend_ast/builtin.h"

void registerMathBuiltins(void);
void registerStringBuiltins(void);
void registerSystemBuiltins(void);
void registerUserBuiltins(void);
void registerHasExtBuiltin(void);

void registerExtendedBuiltins(void) {
  registerHasExtBuiltin();
#ifdef ENABLE_EXT_BUILTIN_MATH
  registerMathBuiltins();
#endif
#ifdef ENABLE_EXT_BUILTIN_STRINGS
  registerStringBuiltins();
#endif
#ifdef ENABLE_EXT_BUILTIN_SYSTEM
  registerSystemBuiltins();
#endif
#ifdef ENABLE_EXT_BUILTIN_USER
  registerUserBuiltins();
#endif
}
