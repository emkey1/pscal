#include "backend_ast/builtin.h"
#include <pthread.h>

void registerMathBuiltins(void);
void registerStringBuiltins(void);
void registerSystemBuiltins(void);
void registerUserBuiltins(void);

void registerYyjsonBuiltins(void);
void registerHasExtBuiltin(void);

void registerExtBuiltinQueryBuiltins(void);

static pthread_once_t s_ext_builtin_once = PTHREAD_ONCE_INIT;

static void registerExtendedBuiltinsOnce(void) {
  registerExtBuiltinQueryBuiltins();
#ifdef ENABLE_EXT_BUILTIN_MATH
  registerMathBuiltins();
#endif
#ifdef ENABLE_EXT_BUILTIN_STRINGS
  registerStringBuiltins();
#endif
#ifdef ENABLE_EXT_BUILTIN_SYSTEM
  registerSystemBuiltins();
#endif
#ifdef ENABLE_EXT_BUILTIN_YYJSON
  registerYyjsonBuiltins();
#endif
#ifdef ENABLE_EXT_BUILTIN_USER
  registerUserBuiltins();
#endif
}

void registerExtendedBuiltins(void) {
  pthread_once(&s_ext_builtin_once, registerExtendedBuiltinsOnce);
}
