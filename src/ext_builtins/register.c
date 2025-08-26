#include "backend_ast/builtin.h"

void registerMathBuiltins(void);
void registerStringBuiltins(void);
void registerSystemBuiltins(void);
void registerMandelbrotRowBuiltin(void);
void registerChudnovskyBuiltin(void);

void registerExtendedBuiltins(void) {
#ifdef ENABLE_EXT_BUILTIN_MATH
    registerMathBuiltins();
#endif
#ifdef ENABLE_EXT_BUILTIN_STRINGS
    registerStringBuiltins();
#endif
#ifdef ENABLE_EXT_BUILTIN_SYSTEM
    registerSystemBuiltins();
#endif
    registerMandelbrotRowBuiltin();
    registerChudnovskyBuiltin();
}
