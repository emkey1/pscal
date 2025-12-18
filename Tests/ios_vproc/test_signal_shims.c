#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
/* Pull in the iOS syscall/signal shims so we exercise the macro layer. */
#include "ios/vproc_shim.h"

#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
void pscalRuntimeDebugLog(const char *message) {
    (void)message;
}
#endif

static volatile sig_atomic_t g_hits_usr1 = 0;
static volatile sig_atomic_t g_hits_usr2 = 0;

static void handler_usr1(int sig) {
    (void)sig;
    g_hits_usr1++;
}

static void handler_usr2(int sig) {
    (void)sig;
    g_hits_usr2++;
}

static void assert_sigaction_and_kill_route_through_vproc(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handler_usr1;
    assert(sigaction(SIGUSR1, &sa, NULL) == 0);

    g_hits_usr1 = 0;
    assert(kill(getpid(), SIGUSR1) == 0);
    assert(g_hits_usr1 == 1);

    vprocDeactivate();
    vprocMarkExit(vp, 0);
    int status = 0;
    (void)waitpid(vprocPid(vp), &status, 0);
    vprocDestroy(vp);
}

static void assert_sigprocmask_blocks_and_unblocks_pending(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handler_usr1;
    assert(sigaction(SIGUSR1, &sa, NULL) == 0);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);

    g_hits_usr1 = 0;
    assert(sigprocmask(SIG_BLOCK, &set, NULL) == 0);
    assert(kill(getpid(), SIGUSR1) == 0);
    assert(g_hits_usr1 == 0);

    sigset_t pending;
    sigemptyset(&pending);
    assert(sigpending(&pending) == 0);
    assert(sigismember(&pending, SIGUSR1));

    assert(sigprocmask(SIG_UNBLOCK, &set, NULL) == 0);
    assert(g_hits_usr1 == 1);

    sigemptyset(&pending);
    assert(sigpending(&pending) == 0);
    assert(!sigismember(&pending, SIGUSR1));

    vprocDeactivate();
    vprocMarkExit(vp, 0);
    int status = 0;
    (void)waitpid(vprocPid(vp), &status, 0);
    vprocDestroy(vp);
}

static void assert_signal_and_raise_route_through_vproc(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    errno = 0;
    assert(signal(SIGUSR2, handler_usr2) != SIG_ERR);
    g_hits_usr2 = 0;
    assert(raise(SIGUSR2) == 0);
    assert(g_hits_usr2 == 1);

    vprocDeactivate();
    vprocMarkExit(vp, 0);
    int status = 0;
    (void)waitpid(vprocPid(vp), &status, 0);
    vprocDestroy(vp);
}

static void assert_pthread_sigmask_uses_vproc_mask(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    assert(signal(SIGUSR2, handler_usr2) != SIG_ERR);
    g_hits_usr2 = 0;

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);

    sigset_t old;
    assert(pthread_sigmask(SIG_BLOCK, &set, &old) == 0);
    assert(kill(getpid(), SIGUSR2) == 0);
    assert(g_hits_usr2 == 0);

    /* Restore the old mask; pending SIGUSR2 should now be delivered. */
    assert(pthread_sigmask(SIG_SETMASK, &old, NULL) == 0);
    assert(g_hits_usr2 == 1);

    vprocDeactivate();
    vprocMarkExit(vp, 0);
    int status = 0;
    (void)waitpid(vprocPid(vp), &status, 0);
    vprocDestroy(vp);
}

int main(void) {
    assert_sigaction_and_kill_route_through_vproc();
    assert_sigprocmask_blocks_and_unblocks_pending();
    assert_signal_and_raise_route_through_vproc();
    assert_pthread_sigmask_uses_vproc_mask();
    return 0;
}
