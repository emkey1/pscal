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
    assert(vprocSetSid(pid, sid) == 0);
    assert(vprocSetPgid(pid, pgid) == 0);
    assert(vprocGetSid(pid) == sid);
    assert(vprocGetPgid(pid) == pgid);
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

static void assert_setpgid_rejects_cross_session(void) {
    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *vp1 = vprocCreate(&opts);
    VProc *vp2 = vprocCreate(NULL);
    assert(vp1 && vp2);
    int pid1 = vprocPid(vp1);
    int pid2 = vprocPid(vp2);

    assert(vprocSetSid(pid2, pid2) == 0);
    int rc = vprocSetPgid(pid2, pid1);
    assert(rc == -1);

    vprocMarkExit(vp1, 0);
    vprocMarkExit(vp2, 0);
    int status = 0;
    (void)vprocWaitPidShim(pid1, &status, 0);
    (void)vprocWaitPidShim(pid2, &status, 0);
    vprocDestroy(vp1);
    vprocDestroy(vp2);
}

static void assert_session_leader_cannot_change_pgid(void) {
    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    int pid = vprocPid(vp);
    int pgid = pid + 500;
    int sid = pid; /* session leader */
    assert(vprocSetSid(pid, sid) == 0);
    int rc = vprocSetPgid(pid, pgid);
    assert(rc == -1);
    vprocMarkExit(vp, 0);
    int status = 0;
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
}

static void assert_foreground_pgid_round_trip(void) {
    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *leader = vprocCreate(&opts);
    assert(leader);
    int sid = vprocPid(leader);
    assert(vprocSetSid(sid, sid) == 0);
    int fg = sid + 123;
    assert(vprocSetForegroundPgid(sid, fg) == 0);
    assert(vprocGetForegroundPgid(sid) == fg);
    size_t cap = vprocSnapshot(NULL, 0);
    VProcSnapshot *snaps = calloc(cap ? cap : 1, sizeof(VProcSnapshot));
    size_t count = vprocSnapshot(snaps, cap);
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        if (snaps[i].sid == sid && snaps[i].fg_pgid == fg) {
            found = true;
        }
    }
    assert(found);
    free(snaps);
    vprocMarkExit(leader, 0);
    int status = 0;
    (void)vprocWaitPidShim(sid, &status, 0);
    vprocDestroy(leader);
}

static void assert_foreground_updates_multiple_times(void) {
    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *leader = vprocCreate(&opts);
    assert(leader);
    int sid = vprocPid(leader);
    assert(vprocSetSid(sid, sid) == 0);
    int fg1 = sid + 11;
    int fg2 = sid + 22;
    assert(vprocSetForegroundPgid(sid, fg1) == 0);
    assert(vprocGetForegroundPgid(sid) == fg1);
    assert(vprocSetForegroundPgid(sid, fg2) == 0);
    assert(vprocGetForegroundPgid(sid) == fg2);
    vprocMarkExit(leader, 0);
    int status = 0;
    (void)vprocWaitPidShim(sid, &status, 0);
    vprocDestroy(leader);
}

int main(void) {
    assert_pgid_sid_defaults();
    assert_pgid_sid_setters();
    assert_group_kill_marks_stopped();
    assert_wait_on_pgid_exit();
    assert_signal_status_propagates();
    assert_setpgid_rejects_cross_session();
    assert_session_leader_cannot_change_pgid();
    assert_foreground_pgid_round_trip();
    assert_foreground_updates_multiple_times();
    printf("pgid/sid tests passed\n");
    return 0;
}
