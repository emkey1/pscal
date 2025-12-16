#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <signal.h>
#define PATH_VIRTUALIZATION_NO_MACROS 1
#include "common/path_virtualization.h"
#if defined(PSCALTST_DEBUGLOG)
void pscalRuntimeDebugLog(const char *message) { (void)message; }
#elif defined(VPROC_ENABLE_STUBS_FOR_TESTS)
void pscalRuntimeDebugLog(const char *message) { (void)message; }
#endif
#include "ios/vproc.h"

static void assert_pgid_sid_defaults(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    assert(pid > 0);
    assert(vprocGetPgid(pid) == pid);
    assert(vprocGetSid(pid) == pid);
    vprocDestroy(vp);
}

static void assert_pgid_sid_setters(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    int pgid = pid + 10;
    int sid = pid + 20;
    assert(vprocSetPgid(pid, pgid) == 0);
    assert(vprocSetSid(pid, sid) == 0);
    assert(vprocGetPgid(pid) == pgid);
    assert(vprocGetSid(pid) == sid);
    vprocDestroy(vp);
}

static void assert_group_kill_marks_stopped(void) {
    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    int pid = vprocPid(vp);
    int pgid = pid + 100;
    assert(vprocSetPgid(pid, pgid) == 0);
    assert(vprocKillShim(-pgid, SIGTSTP) == 0);
    int status = 0;
    assert(vprocWaitPidShim(pid, &status, WUNTRACED) == pid);
    assert(WIFSTOPPED(status));
    vprocMarkExit(vp, 0);
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
}

static void assert_wait_on_pgid_exit(void) {
    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    int pid = vprocPid(vp);
    int pgid = pid + 200;
    assert(vprocSetPgid(pid, pgid) == 0);
    vprocMarkExit(vp, 9);
    int status = 0;
    assert(vprocWaitPidShim(-pgid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 9);
    vprocDestroy(vp);
}

static void assert_signal_status_propagates(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    assert(vprocKillShim(pid, SIGTERM) == 0);
    int status = 0;
    assert(vprocWaitPidShim(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGTERM);
    vprocDestroy(vp);
}

int main(void) {
    assert_pgid_sid_defaults();
    assert_pgid_sid_setters();
    assert_group_kill_marks_stopped();
    assert_wait_on_pgid_exit();
    assert_signal_status_propagates();
    printf("pgid/sid tests passed\n");
    return 0;
}
