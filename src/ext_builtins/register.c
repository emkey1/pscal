#include "backend_ast/builtin.h"

void registerMathBuiltins(void);
void registerStringBuiltins(void);
void registerSystemBuiltins(void);
void registerMandelbrotRowBuiltin(void);
void registerChudnovskyBuiltin(void);

void registerExtendedBuiltins(void) {
    registerMathBuiltins();
    registerStringBuiltins();
    registerSystemBuiltins();
    registerMandelbrotRowBuiltin();
    registerChudnovskyBuiltin();
}
