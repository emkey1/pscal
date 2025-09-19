#ifndef CLIKE_BUILTINS_H
#define CLIKE_BUILTINS_H

const char* clikeCanonicalBuiltinName(const char *name);
int clikeGetBuiltinID(const char *name);
void clikeRegisterBuiltins(void);

#endif
