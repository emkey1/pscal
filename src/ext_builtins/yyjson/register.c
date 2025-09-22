#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"

void registerYyjsonReadBuiltin(void);
void registerYyjsonReadFileBuiltin(void);
void registerYyjsonDocFreeBuiltin(void);
void registerYyjsonFreeValueBuiltin(void);
void registerYyjsonGetRootBuiltin(void);
void registerYyjsonGetKeyBuiltin(void);
void registerYyjsonGetIndexBuiltin(void);
void registerYyjsonGetLengthBuiltin(void);
void registerYyjsonGetTypeBuiltin(void);
void registerYyjsonGetStringBuiltin(void);
void registerYyjsonGetNumberBuiltin(void);
void registerYyjsonGetIntBuiltin(void);
void registerYyjsonGetBoolBuiltin(void);
void registerYyjsonIsNullBuiltin(void);

void registerYyjsonBuiltins(void) {
    extBuiltinRegisterCategory("yyjson");
    extBuiltinRegisterFunction("yyjson", "YyjsonRead");
    extBuiltinRegisterFunction("yyjson", "YyjsonReadFile");
    extBuiltinRegisterFunction("yyjson", "YyjsonDocFree");
    extBuiltinRegisterFunction("yyjson", "YyjsonFreeValue");
    extBuiltinRegisterFunction("yyjson", "YyjsonGetRoot");
    extBuiltinRegisterFunction("yyjson", "YyjsonGetKey");
    extBuiltinRegisterFunction("yyjson", "YyjsonGetIndex");
    extBuiltinRegisterFunction("yyjson", "YyjsonGetLength");
    extBuiltinRegisterFunction("yyjson", "YyjsonGetType");
    extBuiltinRegisterFunction("yyjson", "YyjsonGetString");
    extBuiltinRegisterFunction("yyjson", "YyjsonGetNumber");
    extBuiltinRegisterFunction("yyjson", "YyjsonGetInt");
    extBuiltinRegisterFunction("yyjson", "YyjsonGetBool");
    extBuiltinRegisterFunction("yyjson", "YyjsonIsNull");

    registerYyjsonReadBuiltin();
    registerYyjsonReadFileBuiltin();
    registerYyjsonDocFreeBuiltin();
    registerYyjsonFreeValueBuiltin();
    registerYyjsonGetRootBuiltin();
    registerYyjsonGetKeyBuiltin();
    registerYyjsonGetIndexBuiltin();
    registerYyjsonGetLengthBuiltin();
    registerYyjsonGetTypeBuiltin();
    registerYyjsonGetStringBuiltin();
    registerYyjsonGetNumberBuiltin();
    registerYyjsonGetIntBuiltin();
    registerYyjsonGetBoolBuiltin();
    registerYyjsonIsNullBuiltin();
}
