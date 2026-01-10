#include "common/builtin_shared.h"
#include "ext_builtins/register.h"
#if defined(PSCAL_TARGET_IOS)
#include "smallclue/smallclue.h"
#endif
#include <pthread.h>

static pthread_once_t g_extended_builtin_once = PTHREAD_ONCE_INIT;

static void sharedRegisterExtendedBuiltinsOnce(void) {
    registerExtendedBuiltins();
#if defined(PSCAL_TARGET_IOS)
    smallclueRegisterBuiltins();
#endif
}

void sharedRegisterExtendedBuiltins(void) {
    pthread_once(&g_extended_builtin_once, sharedRegisterExtendedBuiltinsOnce);
}
