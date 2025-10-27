#include <limits.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/utils.h"
#include "backend_ast/builtin.h"

static Value vmBuiltinGetCurrentDir(struct VM_s* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "GetCurrentDir expects no arguments.");
        return makeString("");
    }
#ifdef _WIN32
    char buffer[MAX_PATH];
    DWORD len = GetCurrentDirectoryA(MAX_PATH, buffer);
    if (len == 0 || len >= MAX_PATH) {
        runtimeError(vm, "GetCurrentDir failed.");
        return makeString("");
    }
    return makeString(buffer);
#else
    char buffer[PATH_MAX];
    if (!getcwd(buffer, sizeof(buffer))) {
        runtimeError(vm, "GetCurrentDir failed.");
        return makeString("");
    }
    return makeString(buffer);
#endif
}

void registerGetCurrentDirBuiltin(void) {
    registerVmBuiltin("getcurrentdir", vmBuiltinGetCurrentDir,
                      BUILTIN_TYPE_FUNCTION, "GetCurrentDir");
}
