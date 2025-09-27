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
    const char *category = "yyjson";
    extBuiltinRegisterCategory(category);
    extBuiltinRegisterGroup(category, "document");
    extBuiltinRegisterGroup(category, "query");
    extBuiltinRegisterGroup(category, "primitives");
    extBuiltinRegisterFunction(category, "document", "YyjsonRead");
    extBuiltinRegisterFunction(category, "document", "YyjsonReadFile");
    extBuiltinRegisterFunction(category, "document", "YyjsonDocFree");
    extBuiltinRegisterFunction(category, "query", "YyjsonFreeValue");
    extBuiltinRegisterFunction(category, "query", "YyjsonGetRoot");
    extBuiltinRegisterFunction(category, "query", "YyjsonGetKey");
    extBuiltinRegisterFunction(category, "query", "YyjsonGetIndex");
    extBuiltinRegisterFunction(category, "query", "YyjsonGetLength");
    extBuiltinRegisterFunction(category, "query", "YyjsonGetType");
    extBuiltinRegisterFunction(category, "primitives", "YyjsonGetString");
    extBuiltinRegisterFunction(category, "primitives", "YyjsonGetNumber");
    extBuiltinRegisterFunction(category, "primitives", "YyjsonGetInt");
    extBuiltinRegisterFunction(category, "primitives", "YyjsonGetBool");
    extBuiltinRegisterFunction(category, "primitives", "YyjsonIsNull");

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
