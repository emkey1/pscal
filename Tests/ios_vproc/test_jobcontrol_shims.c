#include <assert.h>
#include <errno.h>
#include <stdio.h>

#define PATH_VIRTUALIZATION_NO_MACROS 1
#include "common/path_virtualization.h"
#if defined(PSCALTST_DEBUGLOG)
void pscalRuntimeDebugLog(const char *message) { (void)message; }
#elif defined(VPROC_ENABLE_STUBS_FOR_TESTS)
void pscalRuntimeDebugLog(const char *message) { (void)message; }
#endif

#include "ios/vproc_shim.h"

#include <unistd.h>
#include <sys/wait.h>

static void assert_pgrp_pgid_roundtrip(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);

    pid_t pid = getpid();
    assert(pid > 0);
    assert(getpgid(0) == getpgrp());
    assert(getpgid(pid) == getpgrp());

    vprocDeactivate();
    vprocMarkExit(vp, 0);
    int status = 0;
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
}

static void assert_setsid_requires_not_pgrp_leader(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);

    pid_t pid = getpid();
    errno = 0;
    pid_t sid = setsid();
    assert(sid == (pid_t)-1);
    assert(errno == EPERM);

    /* Move into a different process group so setsid may succeed. */
    assert(setpgid(0, pid + 1000) == 0);
    errno = 0;
    sid = setsid();
    assert(sid == pid);
    assert(getsid(0) == pid);
    assert(getpgrp() == pid);

    vprocDeactivate();
    vprocMarkExit(vp, 0);
    int status = 0;
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
}

static void assert_tcsetpgrp_tracks_session_foreground(void) {
    VProc *shell = vprocCreate(NULL);
    assert(shell);
    vprocActivate(shell);
    pid_t shell_pid = getpid();

    /* Ensure setsid can succeed. */
    assert(setpgid(0, shell_pid + 2000) == 0);
    assert(setsid() == shell_pid);
    assert(tcgetpgrp(0) == shell_pid);

    VProc *job = vprocCreate(NULL);
    assert(job);
    vprocActivate(job);
    pid_t job_pid = getpid();

    /* Child starts in shell session and pgrp by inheritance. */
    assert(getsid(0) == shell_pid);
    assert(getpgrp() == shell_pid);

    /* Create a new process group for the job and make it foreground. */
    assert(setpgid(0, 0) == 0);
    assert(getpgrp() == job_pid);
    assert(tcsetpgrp(0, job_pid) == 0);
    assert(tcgetpgrp(0) == job_pid);

    vprocDeactivate(); /* back to shell */
    assert(getpid() == shell_pid);
    assert(tcgetpgrp(0) == job_pid);

    /* Restore shell to foreground. */
    assert(tcsetpgrp(0, shell_pid) == 0);
    assert(tcgetpgrp(0) == shell_pid);

    vprocDeactivate();

    vprocMarkExit(job, 0);
    vprocMarkExit(shell, 0);
    int status = 0;
    (void)vprocWaitPidShim(job_pid, &status, 0);
    (void)vprocWaitPidShim(shell_pid, &status, 0);
    vprocDestroy(job);
    vprocDestroy(shell);
}

static void assert_getppid_tracks_virtual_parent(void) {
    VProc *shell = vprocCreate(NULL);
    assert(shell);
    vprocActivate(shell);
    pid_t shell_pid = getpid();

    VProc *child = vprocCreate(NULL);
    assert(child);
    vprocActivate(child);
    pid_t child_pid = getpid();
    assert(child_pid > 0);
    assert(getppid() == shell_pid);

    vprocDeactivate(); /* back to shell */
    vprocDeactivate(); /* back to no vproc */

    vprocMarkExit(child, 0);
    vprocMarkExit(shell, 0);
    int status = 0;
    (void)vprocWaitPidShim(child_pid, &status, 0);
    (void)vprocWaitPidShim(shell_pid, &status, 0);
    vprocDestroy(child);
    vprocDestroy(shell);
}

int main(void) {
    assert_pgrp_pgid_roundtrip();
    assert_setsid_requires_not_pgrp_leader();
    assert_tcsetpgrp_tracks_session_foreground();
    assert_getppid_tracks_virtual_parent();
    printf("job-control shim tests passed\n");
    return 0;
}
