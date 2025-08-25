#include "backend_ast/builtin.h"

void registerFactorialBuiltin(void);
void registerFibonacciBuiltin(void);
void registerMandelbrotRowBuiltin(void);
void registerChudnovskyBuiltin(void);

void pascal_ext_math_init(void) {
  registerFactorialBuiltin();
  registerFibonacciBuiltin();
  registerMandelbrotRowBuiltin();
  registerChudnovskyBuiltin();
}
