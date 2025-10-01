#ifndef PSCAL_GL_H
#define PSCAL_GL_H

#ifdef SDL
#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct VM_s;

Value vmBuiltinGlbegin(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlclear(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlclearcolor(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlcleardepth(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlcolor3f(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlcolor4f(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGldepthtest(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlcullface(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlend(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlenable(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGldisable(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlloadidentity(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlnormal3f(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlmatrixmode(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlpopmatrix(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlpushmatrix(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlrotatef(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlscalef(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlshademodel(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlfrustum(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlperspective(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGllightfv(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlmaterialfv(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlmaterialf(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlcolormaterial(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlblendfunc(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGltranslatef(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlvertex3f(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlviewport(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGlishardwareaccelerated(struct VM_s* vm, int arg_count, Value* args);

#ifdef __cplusplus
}
#endif

#endif // SDL
#endif // PSCAL_GL_H
