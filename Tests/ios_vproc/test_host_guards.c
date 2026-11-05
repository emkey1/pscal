/* Two guards between the iOS flavour and the machine it runs on when that is a host: the real root
 * is never provisioned as a PATH_TRUNCATE prefix, and no signal reaches the host's kill. A hook and
 * the kill defined below stay in place while a guarded form is tried, so without a guard this test
 * records the attempt and fails, and still touches nothing and signals nothing. Each is proved live
 * first, and the test stops there if it is not. Inside the VM those same signals mean what they
 * mean on Linux, which is checked against the task table. */
#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define PATH_VIRTUALIZATION_NO_MACROS 1
#include "common/path_truncate.h"
#include "common/path_virtualization.h"
#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
void pscalRuntimeDebugLog(const char *message) { (void)message; }
#endif
#include "ios/vproc.h"

enum { MAX_SEEN = 64 };

static int g_fails = 0;
static char g_base[PATH_MAX];
static char g_home[PATH_MAX];

static char g_seen_prefix[MAX_SEEN][PATH_MAX];
static size_t g_seen_prefix_count = 0;
static pid_t g_seen_pid[MAX_SEEN];
static size_t g_seen_pid_count = 0;

static void record_prefix(const char *prefix) {
    if (g_seen_prefix_count < MAX_SEEN) {
        snprintf(g_seen_prefix[g_seen_prefix_count], PATH_MAX, "%s", prefix);
    }
    g_seen_prefix_count++;
}

static int record_kill(pid_t pid, int sig) {
    (void)sig;
    if (g_seen_pid_count < MAX_SEEN) {
        g_seen_pid[g_seen_pid_count] = pid;
    }
    g_seen_pid_count++;
    return 0;
}

/* These stand in for libc's in every object linked here, so a call put back in vproc lands on them
 * and signals nothing. run_ios_vproc_tests.sh stops if vproc gets to the host's kill any other way. */
int kill(pid_t pid, int sig) {
    return record_kill(pid, sig);
}

int killpg(pid_t pgrp, int sig) {
    return record_kill(-pgrp, sig);
}

static void fail(const char *what, const char *detail) {
    fprintf(stderr, "FAIL %s: %s\n", what, detail ? detail : "(null)");
    g_fails++;
}

static void reset_env(const char *home) {
    unsetenv("PATH_TRUNCATE");
    unsetenv("PSCALI_CONTAINER_ROOT");
    unsetenv("PSCALI_PATH_TRUNCATE_DISABLED");
    if (home) {
        setenv("HOME", home, 1);
    } else {
        unsetenv("HOME");
    }
    g_seen_prefix_count = 0;
}

/* The home stood in: only it was provisioned, and it is what paths now map under. */
static void expect_home(const char *what, const char *spelling) {
    if (g_seen_prefix_count == 0) {
        fail(what, spelling);
    }
    for (size_t i = 0; i < g_seen_prefix_count && i < MAX_SEEN; ++i) {
        if (strcmp(g_seen_prefix[i], g_home) != 0) {
            fprintf(stderr, "  %s provisioned [%s]\n", spelling, g_seen_prefix[i]);
            fail(what, spelling);
        }
    }
    const char *env = getenv("PATH_TRUNCATE");
    if (!env || strcmp(env, g_home) != 0) {
        fprintf(stderr, "  PATH_TRUNCATE is [%s]\n", env ? env : "(unset)");
        fail(what, spelling);
    }
}

static void expect_nothing(const char *what, const char *spelling) {
    for (size_t i = 0; i < g_seen_prefix_count && i < MAX_SEEN; ++i) {
        fprintf(stderr, "  %s provisioned [%s]\n", spelling ? spelling : "(null)", g_seen_prefix[i]);
    }
    if (g_seen_prefix_count != 0) {
        fail(what, spelling);
    }
}

static void check_root_prefixes(void) {
    char link_a[PATH_MAX], link_b[PATH_MAX], slash[PATH_MAX], dot[PATH_MAX], via_b[PATH_MAX];
    snprintf(link_a, sizeof(link_a), "%s/to_root", g_base);
    snprintf(link_b, sizeof(link_b), "%s/to_link", g_base);
    snprintf(slash, sizeof(slash), "%s/", link_a);
    snprintf(dot, sizeof(dot), "%s/.", link_a);
    snprintf(via_b, sizeof(via_b), "%s//", link_b);
    if (symlink("/", link_a) != 0 || symlink(link_a, link_b) != 0) {
        fail("symlinks", strerror(errno));
        return;
    }

    const char *roots[] = {
        "/", "//", "///", "/.", "/./", "/.//.", "/..", "/../", "/../..", "/./../.",
        "/tmp/..", "/tmp/./..//", "/zz-host-guards-no-such-dir/..",
        link_a, link_b, slash, dot, via_b,
    };
    const size_t nroots = sizeof(roots) / sizeof(roots[0]);

    char under_link[PATH_MAX], up_one[PATH_MAX];
    snprintf(under_link, sizeof(under_link), "%s/usr", link_a);
    snprintf(up_one, sizeof(up_one), "%s/..", g_home);
    const char *not_roots[] = { g_home, g_base, up_one, under_link, "/usr", "/usr/bin", "relative", "./", "", NULL };
    for (size_t i = 0; i < nroots; ++i) {
        if (!pathTruncatePrefixIsRoot(roots[i])) {
            fail("the root not recognised", roots[i]);
        }
    }
    for (size_t i = 0; i < sizeof(not_roots) / sizeof(not_roots[0]); ++i) {
        if (pathTruncatePrefixIsRoot(not_roots[i])) {
            fail("not the root, but taken for it", not_roots[i]);
        }
    }

    for (size_t i = 0; i < nroots; ++i) {
        const char *s = roots[i];

        reset_env(g_home);
        pathTruncateApplyEnvironment(s);
        expect_home("pathTruncateApplyEnvironment", s);

        reset_env(g_home);
        setenv("PSCALI_PATH_TRUNCATE_DISABLED", "1", 1);
        vprocApplyPathTruncation(s);
        expect_home("vprocApplyPathTruncation", s);
        if (getenv("PSCALI_PATH_TRUNCATE_DISABLED")) {
            fail("vprocApplyPathTruncation stayed disabled", s);
        }

        reset_env(g_home);
        setenv("PATH_TRUNCATE", s, 1);
        vprocRunPathTruncationDefaultForTests();
        expect_home("the default from PATH_TRUNCATE", s);

        /* No home to stand in: nothing is provisioned at all. */
        reset_env("/");
        pathTruncateApplyEnvironment(s);
        expect_nothing("apply with HOME=/", s);
        if (getenv("PATH_TRUNCATE")) {
            fail("apply with HOME=/ left PATH_TRUNCATE set", s);
        }

        reset_env(NULL);
        vprocApplyPathTruncation(s);
        expect_nothing("apply with no HOME", s);

        reset_env(s);
        vprocRunPathTruncationDefaultForTests();
        expect_nothing("the default from HOME", s);

        reset_env(g_home);
        setenv("PSCALI_CONTAINER_ROOT", s, 1);
        vprocRunPathTruncationDefaultForTests();
        expect_home("the default from the container root", s);

        /* Not "<root>/Documents" either, when the container root is the root as well. */
        reset_env(g_home);
        setenv("PSCALI_CONTAINER_ROOT", "/", 1);
        vprocApplyPathTruncation(s);
        expect_home("apply with a container root of /", s);

        reset_env(g_home);
        setenv("PSCALI_CONTAINER_ROOT", "/", 1);
        setenv("PATH_TRUNCATE", s, 1);
        vprocRunPathTruncationDefaultForTests();
        expect_home("the default with a container root of /", s);

        reset_env(g_home);
        pathTruncateProvisionDev(s);
        expect_nothing("pathTruncateProvisionDev", s);
        reset_env(g_home);
        pathTruncateProvisionProc(s);
        expect_nothing("pathTruncateProvisionProc", s);
    }

    /* Looking up /proc refreshes it under the prefix in force, which is not always the home. */
    char out[PATH_MAX];
    reset_env(g_home);
    setenv("PATH_TRUNCATE", "/", 1);
    if (!pathTruncateExpand("/proc", out, sizeof(out))) {
        fail("expanding /proc", strerror(errno));
    }
    if (g_seen_prefix_count == 0) {
        fail("looking up /proc did not reach the refresh, so the next checks say nothing", g_home);
    }
    for (size_t i = 0; i < g_seen_prefix_count && i < MAX_SEEN; ++i) {
        if (strcmp(g_seen_prefix[i], g_home) != 0) {
            fail("looking up /proc refreshed", g_seen_prefix[i]);
        }
    }
    reset_env(g_home);
    setenv("PATH_TRUNCATE", "/", 1);
    if (!pathTruncateExpand("/zz", out, sizeof(out)) ||
        strncmp(out, g_home, strlen(g_home)) != 0) {
        fail("a path under PATH_TRUNCATE=/ does not map under the home", out);
    }
    reset_env("/");
    (void)pathTruncateExpand("/proc", out, sizeof(out));
    expect_nothing("looking up /proc with HOME=/", "/proc");
    reset_env(NULL);
    setenv("PATH_TRUNCATE", "/", 1);
    (void)pathTruncateExpand("/proc/self/status", out, sizeof(out));
    expect_nothing("looking up /proc with PATH_TRUNCATE=/ and no HOME", "/proc/self/status");

    reset_env(g_home);
    setenv("PSCALI_PATH_TRUNCATE_DISABLED", "1", 1);
    vprocRunPathTruncationDefaultForTests();
    expect_nothing("the default while disabled", "");
    reset_env(g_home);
    vprocApplyPathTruncation(NULL);
    expect_nothing("apply of NULL", NULL);
    if (!getenv("PSCALI_PATH_TRUNCATE_DISABLED") || getenv("PATH_TRUNCATE")) {
        fail("apply of NULL no longer disables", NULL);
    }
}

/* A prefix that is not the root still gets everything. Real files, inside the throwaway directory. */
static void check_ordinary_prefix_provisions(void) {
    char real[PATH_MAX], path[PATH_MAX + 32];
    snprintf(real, sizeof(real), "%s/real", g_base);
    if (mkdir(real, 0777) != 0) {
        fail("mkdir", real);
        return;
    }
    reset_env(g_home);
    pathTruncateSetProvisionHookForTests(NULL);
    pathTruncateApplyEnvironment(real);
    pathTruncateSetProvisionHookForTests(record_prefix);

    const char *made[] = { "tmp", "var/tmp", "var/log", "var/local", "dev", "dev/pts", "proc", "usr/bin" };
    for (size_t i = 0; i < sizeof(made) / sizeof(made[0]); ++i) {
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", real, made[i]);
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fail("an ordinary prefix did not get", path);
        }
    }
    struct stat st;
    snprintf(path, sizeof(path), "%s/dev/null", real);
    if (lstat(path, &st) != 0 || !S_ISLNK(st.st_mode)) {
        fail("an ordinary prefix did not get", path);
    }
}

static void expect_refused(const char *what, pid_t pid, int sig) {
    char detail[64];
    snprintf(detail, sizeof(detail), "pid %d sig %d", (int)pid, sig);
    g_seen_pid_count = 0;
    errno = 0;
    int rc = vprocKillShim(pid, sig);
    int err = errno;
    for (size_t i = 0; i < g_seen_pid_count && i < MAX_SEEN; ++i) {
        fprintf(stderr, "  would have sent the host kill(%d)\n", (int)g_seen_pid[i]);
    }
    if (g_seen_pid_count != 0) {
        fail(what, detail);
    } else if (rc != -1 || err != ESRCH) {
        fprintf(stderr, "  rc %d errno %d\n", rc, err);
        fail(what, detail);
    }
}

static void check_host_kill(void) {
    if (vprocCurrent() != NULL || vprocGetShellSelfPid() > 0) {
        fail("kill checks need no virtual context", NULL);
        return;
    }
    const pid_t targets[] = { -1, 0, -2, -5, -77777 };
    const int sigs[] = { SIGHUP, SIGWINCH, SIGKILL, SIGUSR1, SIGTERM, 0 };
    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); ++t) {
        for (size_t s = 0; s < sizeof(sigs) / sizeof(sigs[0]); ++s) {
            expect_refused("no virtual context", targets[t], sigs[s]);
        }
    }

    /* A task whose table entry is gone has no group of its own to send to. */
    VProc *vp = vprocCreate(NULL);
    if (!vp) {
        fail("vprocCreate", strerror(errno));
        return;
    }
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
    vprocDiscard(vprocPid(vp));
    expect_refused("a caller with no group", 0, SIGHUP);
    expect_refused("a caller with no group", 0, SIGWINCH);
    vprocDeactivate();

    /* A table with tasks in it answers these itself and leaves the host out of it. */
    VProc *other = vprocCreate(NULL);
    if (!other) {
        fail("vprocCreate", strerror(errno));
        return;
    }
    g_seen_pid_count = 0;
    (void)vprocKillShim(-1, SIGWINCH);
    (void)vprocKillShim(-vprocPid(other), SIGWINCH);
    if (g_seen_pid_count != 0) {
        fail("a signal the table holds a target for went to the host", NULL);
    }
}

/* A real process with a pid the table does not know. It ends by itself when its pipe closes, so how
 * it ended says whether anything else reached it, however that got to the host. */
static void check_real_process_stays_out_of_reach(void) {
    int gate[2];
    if (pipe(gate) != 0) {
        fail("pipe", strerror(errno));
        return;
    }
    fflush(stdout);
    fflush(stderr);
    pid_t child = fork();
    if (child < 0) {
        fail("fork", strerror(errno));
        return;
    }
    if (child == 0) {
        char byte;
        close(gate[1]);
        alarm(30);
        _exit(read(gate[0], &byte, 1) == 0 ? 7 : 1);
    }
    close(gate[0]);
    const int sigs[] = { SIGHUP, SIGUSR1, SIGUSR2, SIGABRT, SIGKILL, SIGWINCH, SIGTERM, 0 };
    for (size_t s = 0; s < sizeof(sigs) / sizeof(sigs[0]); ++s) {
        expect_refused("a real process's pid", child, sigs[s]);
    }
    close(gate[1]);
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 7) {
        fail("the child was reached", "it did not end by itself");
    }
}

/* Inside the VM a program gets Linux's answers: the host is out of it, the table is not. */

static volatile int g_usr1_hits = 0;

static void on_usr1(int sig) {
    (void)sig;
    g_usr1_hits++;
}

static void catch_usr1(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_usr1;
    if (vprocSigactionShim(SIGUSR1, &sa, NULL) != 0) {
        fail("sigaction", strerror(errno));
    }
}

static VProc *become_task(void) {
    VProc *vp = vprocCreate(NULL);
    if (!vp) {
        fail("vprocCreate", strerror(errno));
        _exit(1);
    }
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
    return vp;
}

/* Another task in the table, in a group of its own unless a pgid is given. */
static int other_task(int pgid) {
    VProc *vp = vprocCreate(NULL);
    if (!vp) {
        fail("vprocCreate", strerror(errno));
        _exit(1);
    }
    int pid = vprocPid(vp);
    if (vprocSetPgid(pid, pgid > 0 ? pgid : pid) != 0) {
        fail("vprocSetPgid", strerror(errno));
    }
    return pid;
}

static int signal_of(int pid) {
    VProcSnapshot snap[64];
    size_t n = vprocSnapshot(snap, 64);
    for (size_t i = 0; i < n; ++i) {
        if (snap[i].pid == pid) {
            return snap[i].exit_signal;
        }
    }
    return -1;
}

static void expect_signal(const char *what, int pid, int want) {
    int got = signal_of(pid);
    if (got != want) {
        char detail[64];
        snprintf(detail, sizeof(detail), "pid %d has signal %d, wanted %d", pid, got, want);
        fail(what, detail);
    }
}

static void expect_kill(const char *what, pid_t pid, int sig, int want_rc, int want_errno) {
    errno = 0;
    int rc = vprocKillShim(pid, sig);
    if (rc != want_rc || (want_rc != 0 && errno != want_errno)) {
        char detail[80];
        snprintf(detail, sizeof(detail), "kill(%d, %d) = %d errno %d", (int)pid, sig, rc, errno);
        fail(what, detail);
    }
    if (g_seen_pid_count != 0) {
        fail("went to the host", what);
    }
}

static void vm_broadcast_reaches_all_but_caller_and_kernel(void) {
    int kernel = vprocEnsureKernelPid();
    VProc *me = become_task();
    int a = other_task(0), b = other_task(0);
    expect_kill("kill(-1, 0) with others", -1, 0, 0, 0);
    expect_kill("kill(-1, SIGTERM)", -1, SIGTERM, 0, 0);
    expect_signal("kill(-1) and another task", a, SIGTERM);
    expect_signal("kill(-1) and another task", b, SIGTERM);
    expect_signal("kill(-1) and the caller", vprocPid(me), 0);
    expect_signal("kill(-1) and the kernel", kernel, 0);
}

static void vm_broadcast_with_no_one_to_reach(void) {
    int kernel = vprocEnsureKernelPid();
    VProc *me = become_task();
    expect_kill("kill(-1, 0) with only the caller and the kernel", -1, 0, -1, ESRCH);
    expect_kill("kill(-1, SIGTERM) with only the caller and the kernel", -1, SIGTERM, -1, ESRCH);
    expect_signal("kill(-1) and the caller", vprocPid(me), 0);
    expect_signal("kill(-1) and the kernel", kernel, 0);
}

static void vm_broadcast_with_no_context(void) {
    int kernel = vprocEnsureKernelPid();
    int a = other_task(0);
    int b = other_task(0);
    expect_kill("kill(-a, SIGHUP) with no context", -a, SIGHUP, 0, 0);
    expect_signal("a group with no context", a, SIGHUP);
    expect_signal("a group with no context", b, 0);
    expect_kill("kill(-1, SIGHUP) with no context", -1, SIGHUP, 0, 0);
    expect_signal("kill(-1) with no context", b, SIGHUP);
    expect_signal("kill(-1) with no context and the kernel", kernel, 0);
}

/* Signals no one asked the VM to handle specially, from a thread that is no task: they reach the task. */
static void vm_pid_with_no_context(void) {
    int kernel = vprocEnsureKernelPid();
    int a = other_task(0);
    int b = other_task(0);
    int c = other_task(0);
    expect_kill("kill(a, 0) with no context", a, 0, 0, 0);
    expect_kill("kill(a, SIGHUP) with no context", a, SIGHUP, 0, 0);
    expect_signal("a task with no context", a, SIGHUP);
    expect_signal("another task with no context", c, 0);
    expect_kill("kill(b, SIGKILL) with no context", b, SIGKILL, 0, 0);
    expect_signal("a task with no context", b, SIGKILL);
    expect_kill("kill(kernel, SIGKILL) with no context", kernel, SIGKILL, 0, 0);
    expect_signal("the kernel with no context", kernel, 0);
    /* Above any pid the host can give out. */
    expect_kill("kill(an unknown pid, SIGKILL) with no context", 200000, SIGKILL, -1, ESRCH);
    expect_kill("kill(an unknown pid, 0) with no context", 200000, 0, -1, ESRCH);
}

static void vm_kill_zero_is_the_callers_group(void) {
    (void)vprocEnsureKernelPid();
    VProc *me = become_task();
    int pid = vprocPid(me);
    int mate = other_task(vprocGetPgid(pid));
    int outsider = other_task(0);
    catch_usr1();
    expect_kill("kill(0, 0)", 0, 0, 0, 0);
    expect_kill("kill(0, SIGUSR1)", 0, SIGUSR1, 0, 0);
    if (g_usr1_hits != 1) {
        fail("kill(0) and the caller's handler", "it did not run once");
    }
    expect_signal("kill(0) and a task in the group", mate, SIGUSR1);
    expect_signal("kill(0) and a task outside it", outsider, 0);
}

/* The same words with the group spelled out, on the caller's own thread: both wait to be polled. */
static void vm_kill_zero_acts_as_the_group_form(void) {
    (void)vprocEnsureKernelPid();
    VProc *me = become_task();
    int pid = vprocPid(me);
    expect_kill("kill(0, SIGINT)", 0, SIGINT, 0, 0);
    expect_signal("kill(0, SIGINT) on its own thread", pid, 0);
    sigset_t pending;
    sigemptyset(&pending);
    if (vprocSigpending(pid, &pending) != 0 || !sigismember(&pending, SIGINT)) {
        fail("kill(0, SIGINT) on its own thread", "not pending");
    }
}

/* With no kernel yet the first task is pid 1, in group 1. Its kill(0) is still only that group. */
static void vm_group_one_is_not_everything(void) {
    VProc *me = become_task();
    int pid = vprocPid(me);
    int outsider = other_task(0);
    if (pid != 1 || vprocGetPgid(pid) != 1) {
        fail("this check needs the first task to be pid 1", NULL);
        return;
    }
    catch_usr1();
    expect_kill("kill(0, 0) in group 1", 0, 0, 0, 0);
    expect_kill("kill(0, SIGUSR1) in group 1", 0, SIGUSR1, 0, 0);
    expect_signal("kill(0) in group 1 and a task outside it", outsider, 0);
    expect_kill("kill(-1, SIGUSR1) from pid 1", -1, SIGUSR1, 0, 0);
    expect_signal("kill(-1) from pid 1 and another task", outsider, SIGUSR1);
}

/* As init on Linux: the call succeeds and nothing happens. */
static void vm_nothing_kills_the_kernel(void) {
    int kernel = vprocEnsureKernelPid();
    (void)become_task();
    expect_kill("kill(kernel, SIGTERM)", kernel, SIGTERM, 0, 0);
    expect_kill("kill(kernel, SIGKILL)", kernel, SIGKILL, 0, 0);
    expect_kill("kill(kernel, 0)", kernel, 0, 0, 0);
    expect_signal("the kernel", kernel, 0);
}

/* Each from a clean table, in a child, so that a task killed in one cannot answer for another. */
static void in_child(const char *name, void (*check)(void)) {
    fflush(stdout);
    fflush(stderr);
    pid_t child = fork();
    if (child < 0) {
        fail("fork", name);
        return;
    }
    if (child == 0) {
        alarm(30);
        g_fails = 0;
        g_seen_pid_count = 0;
        check();
        fflush(stderr);
        _exit(g_fails == 0 ? 0 : 1);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(name, "failed in its child");
    }
}

static void check_vm_signals(void) {
    in_child("broadcast", vm_broadcast_reaches_all_but_caller_and_kernel);
    in_child("broadcast to no one", vm_broadcast_with_no_one_to_reach);
    in_child("broadcast with no context", vm_broadcast_with_no_context);
    in_child("a pid with no context", vm_pid_with_no_context);
    in_child("kill(0)", vm_kill_zero_is_the_callers_group);
    in_child("kill(0) and the group form", vm_kill_zero_acts_as_the_group_form);
    in_child("group 1", vm_group_one_is_not_everything);
    in_child("the kernel", vm_nothing_kills_the_kernel);
}

static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *ftw) {
    (void)st;
    (void)ftw;
    if (strncmp(path, g_base, strlen(g_base)) != 0) {
        return -1;
    }
    return (type == FTW_DP) ? rmdir(path) : unlink(path);
}

int main(void) {
    /* Armed before anything can ask, and proved live before a guarded form is tried. */
    pathTruncateSetProvisionHookForTests(record_prefix);

    const char *tmp = getenv("TMPDIR");
    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof(tmpl), "%s/pscal-host-guards.XXXXXX", (tmp && tmp[0] == '/') ? tmp : "/tmp");
    if (!mkdtemp(tmpl) || !realpath(tmpl, g_base) || !strstr(g_base, "/pscal-host-guards.")) {
        fprintf(stderr, "FAIL no throwaway directory: %s\n", strerror(errno));
        return 1;
    }
    snprintf(g_home, sizeof(g_home), "%s/home", g_base);
    if (mkdir(g_home, 0777) != 0) {
        fprintf(stderr, "FAIL mkdir %s: %s\n", g_home, strerror(errno));
        rmdir(g_base);
        return 1;
    }

    reset_env(g_home);
    pathTruncateApplyEnvironment(g_home);
    bool prefix_hook_live = (g_seen_prefix_count > 0 && strcmp(g_seen_prefix[0], g_home) == 0);
    bool kill_stands_in = (dlsym(RTLD_DEFAULT, "kill") == (void *)kill);
    if (!prefix_hook_live || !kill_stands_in) {
        fprintf(stderr, "FAIL the hooks are not live (prefix %d, kill %d); stopping here\n",
                (int)prefix_hook_live, (int)kill_stands_in);
        g_fails++;
    } else {
        check_root_prefixes();
        check_ordinary_prefix_provisions();
        check_vm_signals();
        check_host_kill();
        check_real_process_stays_out_of_reach();
    }

    pathTruncateSetProvisionHookForTests(record_prefix);
    nftw(g_base, remove_entry, 16, FTW_DEPTH | FTW_PHYS);

    if (g_fails != 0) {
        fprintf(stderr, "test_host_guards: %d failures\n", g_fails);
        return 1;
    }
    printf("test_host_guards: ok\n");
    return 0;
}
