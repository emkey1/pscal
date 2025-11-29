#include "common/builtin_shared.h"
#include "ext_builtins/register.h"
#include "smallclue/smallclue.h"
#include <pthread.h>

static pthread_once_t g_extended_builtin_once = PTHREAD_ONCE_INIT;

static void sharedRegisterExtendedBuiltinsOnce(void) {
    registerExtendedBuiltins();
    smallclueRegisterBuiltins();
}

void sharedRegisterExtendedBuiltins(void) {
    pthread_once(&g_extended_builtin_once, sharedRegisterExtendedBuiltinsOnce);
}
