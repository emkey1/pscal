// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// What a program sees on a terminal, printed so that two runs can be compared. Run on a
// Linux machine, its output is the reference (tty_diff.linux.out, and for what needs
// privileges tty_diff.root.linux.out); run under vproc it must print the same.
//
// Only POSIX and Linux calls, in Linux's request numbers and with the kernel's struct, so
// that one program means the same thing on both. What is compared must not depend on a
// pid, a host's numbers or the time of day: errnos and signals print as names, pids only
// as how they relate, and durations only as which side of a line they fell on.
//
// Under vproc a process is a thread with a table of its own, so each opens what it needs
// by name, as nothing is inherited, and they keep in step through shared memory.
//
// Linux:  gcc -O1 -pthread tty_diff.c -o tty_diff && ./tty_diff > out
//         sudo ./tty_diff root > root-out
//
// Items are the ids in section 8.2 of the terminal specification. Stage B and C items
// are not asked about here: a non-canonical case clears ICRNL and ECHO (L2), no case
// types the flow control keys with IXON set (L6), and a key that raises a signal comes
// once per run and with a foreground group (L9).
#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(PSCAL_TARGET_IOS)
#define UNDER_VPROC 1
#define PATH_VIRTUALIZATION_NO_MACROS 1
#include "common/path_virtualization.h"
#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
void pscalRuntimeDebugLog(const char *message) {
    (void)message;
}
#endif
#include "ios/vproc_shim.h"
#else
#define UNDER_VPROC 0
#endif

/* --- Linux's numbers --- */

#define K_TCGETS 0x5401UL
#define K_TCSETS 0x5402UL
#define K_TCSETSW 0x5403UL
#define K_TCSETSF 0x5404UL
#define K_TCSBRK 0x5409UL
#define K_TCFLSH 0x540bUL
#define K_TIOCEXCL 0x540cUL
#define K_TIOCNXCL 0x540dUL
#define K_TIOCSCTTY 0x540eUL
#define K_TIOCGPGRP 0x540fUL
#define K_TIOCSPGRP 0x5410UL
#define K_TIOCOUTQ 0x5411UL
#define K_TIOCSTI 0x5412UL
#define K_TIOCGWINSZ 0x5413UL
#define K_TIOCSWINSZ 0x5414UL
#define K_FIONREAD 0x541bUL
#define K_TIOCPKT 0x5420UL
#define K_TIOCNOTTY 0x5422UL
#define K_TCSBRKP 0x5425UL
#define K_TIOCGSID 0x5429UL
#define K_TIOCGPTN 0x80045430UL
#define K_TIOCSPTLCK 0x40045431UL
#define K_TIOCGPKT 0x80045438UL
#define K_TIOCGEXCL 0x80045440UL

#define KI_IGNBRK 0x1u
#define KI_BRKINT 0x2u
#define KI_IGNPAR 0x4u
#define KI_PARMRK 0x8u
#define KI_INPCK 0x10u
#define KI_ISTRIP 0x20u
#define KI_INLCR 0x40u
#define KI_IGNCR 0x80u
#define KI_ICRNL 0x100u
#define KI_IUCLC 0x200u
#define KI_IXON 0x400u
#define KI_IMAXBEL 0x2000u
#define KI_IUTF8 0x4000u

#define KO_OPOST 0x1u
#define KO_OLCUC 0x2u
#define KO_ONLCR 0x4u
#define KO_OCRNL 0x8u
#define KO_ONOCR 0x10u
#define KO_ONLRET 0x20u
#define KO_OFILL 0x40u
#define KO_OFDEL 0x80u
#define KO_NL1 0x100u
#define KO_CR3 0x600u
#define KO_TAB1 0x800u
#define KO_TAB2 0x1000u
#define KO_TAB3 0x1800u
#define KO_BS1 0x2000u
#define KO_VT1 0x4000u
#define KO_FF1 0x8000u

#define KL_ISIG 0x1u
#define KL_ICANON 0x2u
#define KL_ECHO 0x8u
#define KL_ECHOE 0x10u
#define KL_ECHOK 0x20u
#define KL_ECHONL 0x40u
#define KL_NOFLSH 0x80u
#define KL_TOSTOP 0x100u
#define KL_ECHOCTL 0x200u
#define KL_ECHOPRT 0x400u
#define KL_ECHOKE 0x800u
#define KL_IEXTEN 0x8000u

#define KV_INTR 0
#define KV_QUIT 1
#define KV_ERASE 2
#define KV_KILL 3
#define KV_EOF 4
#define KV_TIME 5
#define KV_MIN 6
#define KV_SWTC 7
#define KV_START 8
#define KV_STOP 9
#define KV_SUSP 10
#define KV_EOL 11
#define KV_REPRINT 12
#define KV_DISCARD 13
#define KV_WERASE 14
#define KV_LNEXT 15
#define KV_EOL2 16

typedef struct {
    uint32_t i, o, c, l;
    uint8_t line;
    uint8_t cc[19];
} KTermios;

typedef struct {
    uint16_t row, col, xpixel, ypixel;
} KWinsize;

/* --- printing --- */

/* A line at a time, in one piece, as processes under vproc share the one stdout. */
static void say(const char *format, ...) {
    static __thread char line[131072];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);
    if (len >= (int)sizeof(line) - 1) {
        len = (int)sizeof(line) - 2;
    }
    line[len] = '\n';
    line[len + 1] = 0;
    fputs(line, stdout);
    fflush(stdout);
}

static const char *errName(int e) {
    switch (e) {
        case 0: return "ok";
        case EPERM: return "EPERM";
        case ENOENT: return "ENOENT";
        case ESRCH: return "ESRCH";
        case EINTR: return "EINTR";
        case EIO: return "EIO";
        case ENXIO: return "ENXIO";
        case EBADF: return "EBADF";
        case EAGAIN: return "EAGAIN";
        case ENOMEM: return "ENOMEM";
        case EACCES: return "EACCES";
        case EFAULT: return "EFAULT";
        case EBUSY: return "EBUSY";
        case EEXIST: return "EEXIST";
        case ENODEV: return "ENODEV";
        case EINVAL: return "EINVAL";
        case EMFILE: return "EMFILE";
        case ENOTTY: return "ENOTTY";
        case ENOSPC: return "ENOSPC";
        case EPIPE: return "EPIPE";
        case ETIMEDOUT: return "ETIMEDOUT";
        default: {
            static char other[32];
            snprintf(other, sizeof(other), "errno%d", e);
            return other;
        }
    }
}

/* Printable, with everything else spelled out. One shared buffer per call site's line is
 * not safe across threads, so the result goes in the caller's. */
static const char *esc(char *out, size_t cap, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    size_t at = 0;
    for (size_t i = 0; i < len && at + 8 < cap; i++) {
        const unsigned char c = p[i];
        if (c == '\r') {
            at += (size_t)snprintf(out + at, cap - at, "\\r");
        } else if (c == '\n') {
            at += (size_t)snprintf(out + at, cap - at, "\\n");
        } else if (c == '\t') {
            at += (size_t)snprintf(out + at, cap - at, "\\t");
        } else if (c == '\b') {
            at += (size_t)snprintf(out + at, cap - at, "\\b");
        } else if (c == '\\') {
            at += (size_t)snprintf(out + at, cap - at, "\\\\");
        } else if (c < 0x20 || c >= 0x7f) {
            at += (size_t)snprintf(out + at, cap - at, "\\x%02x", c);
        } else {
            out[at++] = (char)c;
        }
    }
    out[at] = 0;
    return out;
}

/* A result as what it means: a count, or a name. */
static const char *resultName(char *out, size_t cap, long result) {
    if (result < 0) {
        snprintf(out, cap, "%s", errName(errno));
    } else {
        snprintf(out, cap, "%ld", result);
    }
    return out;
}

static void sayResult(const char *label, long result) {
    char text[32];
    say("%s: %s", label, resultName(text, sizeof(text), result));
}

static const char *pollName(short events) {
    static char names[64];
    names[0] = 0;
    if (events & POLLIN) strcat(names, "IN ");
    if (events & POLLPRI) strcat(names, "PRI ");
    if (events & POLLOUT) strcat(names, "OUT ");
    if (events & POLLERR) strcat(names, "ERR ");
    if (events & POLLHUP) strcat(names, "HUP ");
    if (!names[0]) strcat(names, "- ");
    names[strlen(names) - 1] = 0;
    return names;
}

/* --- time --- */

static long long nowMs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void sleepMs(int ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

/* --- shared between processes --- */

#define MARKS 32

typedef struct {
    volatile int marks[MARKS];
    volatile int hits[MARKS];
    volatile int other_group;
} Shared;

static Shared *gShared;

static void mark(int i) {
    __atomic_store_n(&gShared->marks[i], 1, __ATOMIC_RELEASE);
}

static void clearMarks(void) {
    for (int i = 0; i < MARKS; i++) {
        gShared->marks[i] = 0;
        gShared->hits[i] = 0;
    }
}

static bool awaitMark(int i) {
    for (int waited = 0; waited < 10000; waited++) {
        if (__atomic_load_n(&gShared->marks[i], __ATOMIC_ACQUIRE)) {
            return true;
        }
        sleepMs(1);
    }
    say("TIMEOUT waiting for mark %d", i);
    return false;
}

/* --- processes --- */

typedef int (*ProcFn)(void *arg);

#if UNDER_VPROC
typedef struct {
    VProc *vp;
    ProcFn fn;
    void *arg;
} ProcStart;

static pthread_mutex_t gProcsLock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int pid;
    VProc *vp;
} gProcs[256];

static void *procThread(void *raw) {
    ProcStart *start = (ProcStart *)raw;
    const int code = start->fn(start->arg);
    vprocMarkExit(start->vp, code);
    free(start);
    return NULL;
}

static pid_t procSpawn(ProcFn fn, void *arg) {
    ProcStart *start = (ProcStart *)calloc(1, sizeof(*start));
    start->vp = vprocCreate(NULL);
    start->fn = fn;
    start->arg = arg;
    const int pid = vprocPid(start->vp);
    pthread_mutex_lock(&gProcsLock);
    for (size_t i = 0; i < sizeof(gProcs) / sizeof(gProcs[0]); i++) {
        if (!gProcs[i].vp) {
            gProcs[i].pid = pid;
            gProcs[i].vp = start->vp;
            break;
        }
    }
    pthread_mutex_unlock(&gProcsLock);
    pthread_t thread;
    VProc *vp = start->vp;
    if (vprocSpawnThread(vp, procThread, start, &thread) != 0) {
        return -1;
    }
    pthread_detach(thread);
    return pid;
}

/* A process's descriptors close when it goes, which the parent's wait stands in for. */
static void procReaped(pid_t pid) {
    pthread_mutex_lock(&gProcsLock);
    VProc *vp = NULL;
    for (size_t i = 0; i < sizeof(gProcs) / sizeof(gProcs[0]); i++) {
        if (gProcs[i].vp && gProcs[i].pid == pid) {
            vp = gProcs[i].vp;
            gProcs[i].vp = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&gProcsLock);
    if (vp) {
        vprocDestroy(vp);
    }
}

static void platformInit(void) {
    VProc *vp = vprocCreate(NULL);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
}

#else

static pid_t procSpawn(ProcFn fn, void *arg) {
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        alarm(60);
        const int code = fn(arg);
        fflush(stdout);
        _exit(code);
    }
    return pid;
}

static void procReaped(pid_t pid) {
    (void)pid;
}

static void platformInit(void) {
    alarm(600);
}

#endif

/* Waits for the child to end, and says how it did. */
static void procFinish(const char *label, pid_t pid) {
    int status = 0;
    const pid_t waited = waitpid(pid, &status, 0);
    if (waited != pid) {
        say("%s: wait: %s", label, errName(errno));
    } else if (WIFEXITED(status)) {
        say("%s: exited %d", label, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        say("%s: killed by signal", label);
    } else {
        say("%s: status other", label);
    }
    procReaped(pid);
}

static const char *signalName(int sig) {
    switch (sig) {
        case SIGTTIN: return "SIGTTIN";
        case SIGTTOU: return "SIGTTOU";
        case SIGTSTP: return "SIGTSTP";
        case SIGSTOP: return "SIGSTOP";
        case SIGCONT: return "SIGCONT";
        default: return "other";
    }
}

/* --- a pty --- */

typedef struct {
    int m;     /* non-blocking */
    int s;     /* non-blocking */
    int num;
    char path[32];
} Pty;

static bool ptyNames(Pty *p) {
    unsigned zero = 0;
    unsigned num = 0;
    if (ioctl(p->m, K_TIOCSPTLCK, &zero) != 0 || ioctl(p->m, K_TIOCGPTN, &num) != 0) {
        say("pty setup: %s", errName(errno));
        return false;
    }
    p->num = (int)num;
    snprintf(p->path, sizeof(p->path), "/dev/pts/%u", num);
    return true;
}

static bool ptyOpen(Pty *p) {
    memset(p, 0, sizeof(*p));
    p->m = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (p->m < 0) {
        say("open ptmx: %s", errName(errno));
        return false;
    }
    if (!ptyNames(p)) {
        return false;
    }
    p->s = open(p->path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (p->s < 0) {
        say("open slave: %s", errName(errno));
        return false;
    }
    return true;
}

static void ptyClose(Pty *p) {
    close(p->s);
    close(p->m);
}

static void kget(int fd, KTermios *t) {
    if (ioctl(fd, K_TCGETS, t) != 0) {
        say("TCGETS: %s", errName(errno));
    }
}

static void kset(int fd, const KTermios *t) {
    if (ioctl(fd, K_TCSETS, t) != 0) {
        say("TCSETS: %s", errName(errno));
    }
}

static void setL(int fd, uint32_t on, uint32_t off) {
    KTermios t;
    kget(fd, &t);
    t.l = (t.l | on) & ~off;
    kset(fd, &t);
}

static void setI(int fd, uint32_t on, uint32_t off) {
    KTermios t;
    kget(fd, &t);
    t.i = (t.i | on) & ~off;
    kset(fd, &t);
}

static void setO(int fd, uint32_t on, uint32_t off) {
    KTermios t;
    kget(fd, &t);
    t.o = (t.o | on) & ~off;
    kset(fd, &t);
}

static void setCc(int fd, int index, uint8_t value) {
    KTermios t;
    kget(fd, &t);
    t.cc[index] = value;
    kset(fd, &t);
}

/* What a line end of the terminal asks for here: raw, and nothing stage B would change. */
static void setRaw(int fd) {
    setL(fd, 0, KL_ICANON | KL_ECHO | KL_ISIG);
    setI(fd, 0, KI_ICRNL);
}

/* Everything readable, waiting until nothing has come for a moment. */
static size_t drain(int fd, void *out, size_t cap) {
    unsigned char *to = (unsigned char *)out;
    size_t len = 0;
    while (len < cap) {
        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, 15) <= 0 || !(pfd.revents & POLLIN)) {
            break;
        }
        const ssize_t n = read(fd, to + len, cap - len);
        if (n <= 0) {
            break;
        }
        len += (size_t)n;
    }
    return len;
}

/* Types keys, and shows what they echoed. */
static void keysAfter(Pty *p, const char *label, const void *typed, size_t len, int settle_ms) {
    if (len > 0 && write(p->m, typed, len) != (ssize_t)len) {
        say("%s: typing: %s", label, errName(errno));
    }
    if (settle_ms > 0) {
        sleepMs(settle_ms);
    }
    unsigned char echoed[8192];
    const size_t n = drain(p->m, echoed, sizeof(echoed));
    char a[512], b[4096];
    say("%s: keys \"%s\" echo \"%s\"", label, esc(a, sizeof(a), typed, len), esc(b, sizeof(b), echoed, n));
}

static void keys(Pty *p, const char *label, const void *typed, size_t len) {
    keysAfter(p, label, typed, len, 0);
}

#define KEYS(p, label, text) keys((p), (label), (text), sizeof(text) - 1)

/* A key that raises a signal is a while reaching everything it reaches, on Linux. */
#define SIGNAL_KEYS(p, label, text) keysAfter((p), (label), (text), sizeof(text) - 1, 40)

/* Reads the slave until it says there is nothing more. */
static void reads(Pty *p, const char *label) {
    char line[4096];
    size_t at = (size_t)snprintf(line, sizeof(line), "%s: read", label);
    for (int i = 0; i < 8; i++) {
        unsigned char got[512];
        const ssize_t n = read(p->s, got, sizeof(got));
        char text[1024];
        if (n > 0) {
            at += (size_t)snprintf(line + at, sizeof(line) - at, " \"%s\"", esc(text, sizeof(text), got, (size_t)n));
        } else if (n == 0) {
            at += (size_t)snprintf(line + at, sizeof(line) - at, " EOF");
        } else {
            at += (size_t)snprintf(line + at, sizeof(line) - at, " %s", errName(errno));
            break;
        }
    }
    say("%s", line);
}

static void section(const char *name) {
    say("");
    say("== %s", name);
}

/* --- L1: a new terminal --- */

static void showTermios(const char *label, int fd) {
    KTermios t;
    kget(fd, &t);
    char cc[128];
    size_t at = 0;
    for (size_t i = 0; i < sizeof(t.cc); i++) {
        at += (size_t)snprintf(cc + at, sizeof(cc) - at, "%02x", t.cc[i]);
    }
    say("%s: i=%x o=%x c=%x l=%x line=%x cc=%s", label, t.i, t.o, t.c, t.l, t.line, cc);
}

static void secDefaults(void) {
    section("L1 a new terminal");
    Pty p;
    if (!ptyOpen(&p)) return;
    showTermios("slave", p.s);
    showTermios("master", p.m);
    KWinsize w = {1, 1, 1, 1};
    sayResult("TIOCGWINSZ", ioctl(p.s, K_TIOCGWINSZ, &w));
    say("winsize: %u %u %u %u", w.row, w.col, w.xpixel, w.ypixel);
    ptyClose(&p);
}

/* --- L3, L4: the extended line editing keys --- */

static void scriptPair(const char *label, const void *typed, size_t len, uint32_t l_off, uint32_t i_on) {
    Pty p;
    if (!ptyOpen(&p)) return;
    setL(p.s, 0, l_off);
    setI(p.s, i_on, 0);
    keys(&p, label, typed, len);
    KEYS(&p, label, "\n");
    reads(&p, label);
    ptyClose(&p);
}

#define S(label, text, l_off) scriptPair((label), (text), sizeof(text) - 1, (l_off), 0)

static void secWordErase(void) {
    section("L3 VWERASE");
    S("plain", "foo bar\x17", 0);
    S("twice", "foo bar\x17\x17", 0);
    S("trailing space", "foo bar   \x17", 0);
    S("punctuation", "foo-bar\x17", 0);
    S("punctuation twice", "foo-bar\x17\x17", 0);
    S("underscore", "foo_bar baz\x17\x17", 0);
    S("digits", "a1 b2\x17", 0);
    S("tab", "one\ttwo\x17", 0);
    S("tab twice", "one\ttwo\x17\x17", 0);
    S("control", "ab\x01" "cd\x17", 0);
    S("high bytes", "ab\xc3\xa9" "cd\x17", 0);
    S("all space", "    \x17", 0);
    S("nothing", "\x17", 0);
    S("other line", "one\n\x17", 0);
    S("no echoe", "foo bar\x17", KL_ECHOE);
    S("no echo", "foo bar\x17", KL_ECHO);
    S("no iexten", "foo bar\x17", KL_IEXTEN);
    section("L3 VREPRINT");
    S("plain", "abc\x12", 0);
    S("then more", "abc\x12" "d", 0);
    S("empty", "\x12", 0);
    S("after a line", "one\nab\x12", 0);
    S("after a line, empty", "one\n\x12", 0);
    S("control in line", "a\x01" "b\x12", 0);
    S("tab in line", "a\tb\x12", 0);
    S("no echo", "abc\x12", KL_ECHO);
    S("no echoctl", "abc\x12", KL_ECHOCTL);
    S("no iexten", "abc\x12", KL_IEXTEN);
    section("L3 VLNEXT");
    S("control", "\x16\x01", 0);
    S("interrupt", "\x16\x03", 0);
    S("eof", "a\x16\x04", 0);
    S("erase", "ab\x16\x7f", 0);
    S("kill", "ab\x16\x15", 0);
    S("newline", "a\x16\n" "b", 0);
    S("return", "a\x16\rb", 0);
    S("itself", "\x16\x16", 0);
    S("werase", "ab\x16\x17", 0);
    S("reprint", "ab\x16\x12", 0);
    S("erased", "ab\x16\x01\x7f", 0);
    S("erased before next", "ab\x16\x7f", 0);
    S("plain letter", "\x16q", 0);
    S("no echo", "\x16\x03", KL_ECHO);
    S("no echoctl", "\x16\x03", KL_ECHOCTL);
    S("no iexten", "a\x16\x7f", KL_IEXTEN);
    S("no isig", "\x16\x03", KL_ISIG);
    S("reprint with literal", "a\x16\x01" "b\x12", 0);
    section("L3 not implemented by Linux");
    S("VDISCARD", "ab\x0f" "cd", 0);
    S("VDISCARD no iexten", "ab\x0f" "cd", KL_IEXTEN);
    {
        Pty p;
        if (ptyOpen(&p)) {
            setCc(p.s, KV_SWTC, '!');
            KEYS(&p, "VSWTC", "ab!cd\n");
            reads(&p, "VSWTC");
            ptyClose(&p);
        }
    }
    section("L3 VEOL2");
    {
        const uint32_t variants[] = {0, KL_IEXTEN, KL_ECHO};
        const char *names[] = {"iexten", "no iexten", "no echo"};
        for (int i = 0; i < 3; i++) {
            Pty p;
            if (!ptyOpen(&p)) return;
            setL(p.s, 0, variants[i]);
            setCc(p.s, KV_EOL2, '%');
            KEYS(&p, names[i], "pc%rest\n");
            reads(&p, names[i]);
            setCc(p.s, KV_EOL2, 0x18);
            KEYS(&p, names[i], "ctl\x18");
            reads(&p, names[i]);
            ptyClose(&p);
        }
    }
    section("L3 VEOL");
    {
        const uint32_t variants[] = {0, KL_IEXTEN, KL_ECHO};
        const char *names[] = {"iexten", "no iexten", "no echo"};
        for (int i = 0; i < 3; i++) {
            Pty p;
            if (!ptyOpen(&p)) return;
            setL(p.s, 0, variants[i]);
            setCc(p.s, KV_EOL, '%');
            KEYS(&p, names[i], "pc%rest\n");
            reads(&p, names[i]);
            setCc(p.s, KV_EOL, 0x18);
            KEYS(&p, names[i], "ctl\x18");
            reads(&p, names[i]);
            ptyClose(&p);
        }
    }
}

/* --- L5: echo --- */

static const struct {
    uint32_t bit;
    const char *name;
} kEchoBits[] = {
    {KL_ECHOE, "echoe"},
    {KL_ECHOK, "echok"},
    {KL_ECHOKE, "echoke"},
    {KL_ECHOPRT, "echoprt"},
    {KL_ECHOCTL, "echoctl"},
};

static void secEchoMatrix(void) {
    section("L5 erase and kill, every combination");
    const size_t bits = sizeof(kEchoBits) / sizeof(kEchoBits[0]);
    for (unsigned combo = 0; combo < (1u << bits); combo++) {
        char name[128] = "";
        uint32_t on = 0;
        uint32_t off = 0;
        for (size_t b = 0; b < bits; b++) {
            if (combo & (1u << b)) {
                on |= kEchoBits[b].bit;
                strcat(name, kEchoBits[b].name);
                strcat(name, " ");
            } else {
                off |= kEchoBits[b].bit;
            }
        }
        if (!name[0]) {
            strcpy(name, "none ");
        }
        name[strlen(name) - 1] = 0;
        Pty p;
        if (!ptyOpen(&p)) return;
        setL(p.s, on, off);
        keys(&p, name, "abc\x7f\x7f" "d", 6);
        keys(&p, name, "\x15", 1);
        keys(&p, name, "ef\x01\x7f", 4);
        keys(&p, name, "\x15", 1);
        keys(&p, name, "\x7f", 1);
        keys(&p, name, "g\x7f\n", 3);
        reads(&p, name);
        keys(&p, name, "x y\x17\x17" "z", 6);
        keys(&p, name, "\x12", 1);
        keys(&p, name, "\n", 1);
        reads(&p, name);
        ptyClose(&p);
    }
}

static void secEchoFlags(void) {
    section("L5 ECHONL");
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setL(p.s, KL_ECHONL, KL_ECHO);
        KEYS(&p, "echonl, no echo", "ab\n");
        reads(&p, "echonl, no echo");
        setL(p.s, KL_ECHO, 0);
        KEYS(&p, "echonl, echo", "ab\n");
        reads(&p, "echonl, echo");
        setL(p.s, 0, KL_ECHO | KL_ICANON);
        setI(p.s, 0, KI_ICRNL);
        ptyClose(&p);
    }
    section("L5 echo goes through output processing");
    {
        const struct {
            const char *name;
            uint32_t on, off;
        } modes[] = {
            {"defaults", 0, 0},
            {"no opost", 0, KO_OPOST},
            {"no onlcr", 0, KO_ONLCR},
            {"ocrnl", KO_OCRNL, 0},
            {"onocr", KO_ONOCR, 0},
            {"onlret", KO_ONLRET, 0},
            {"olcuc", KO_OLCUC, 0},
            {"tab3", KO_TAB3, 0},
            {"onlret no onlcr", KO_ONLRET, KO_ONLCR},
        };
        for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
            Pty p;
            if (!ptyOpen(&p)) return;
            setO(p.s, modes[i].on, modes[i].off);
            setI(p.s, 0, KI_ICRNL);
            setCc(p.s, KV_EOL, '%');
            KEYS(&p, modes[i].name, "ab\n");
            KEYS(&p, modes[i].name, "\rcd\r\r");
            KEYS(&p, modes[i].name, "\n");
            KEYS(&p, modes[i].name, "x%");
            KEYS(&p, modes[i].name, "\ty\t");
            KEYS(&p, modes[i].name, "\n");
            reads(&p, modes[i].name);
            ptyClose(&p);
        }
    }
    section("L5 control bytes");
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setI(p.s, 0, KI_IXON);
        setL(p.s, 0, KL_ISIG);
        KEYS(&p, "isig off", "\x01\x1b\x1c\x1a\x03\x7f");
        KEYS(&p, "keys", "\x15");
        const unsigned char nul[] = {0, 'a', 0x80, 0x9f, 0xc3, 0xa9, 0xff, 0x11, 0x13};
        keys(&p, "nul and high", nul, sizeof(nul));
        KEYS(&p, "keys", "\n");
        reads(&p, "nul and high");
        setL(p.s, 0, KL_ECHOCTL);
        KEYS(&p, "no echoctl", "\x01\x1b\x11\x13\n");
        reads(&p, "no echoctl");
        ptyClose(&p);
    }
    section("L5 the end-of-file key");
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        KEYS(&p, "alone", "\x04");
        reads(&p, "alone");
        KEYS(&p, "after text", "abc\x04");
        reads(&p, "after text");
        KEYS(&p, "after a line", "one\n\x04two\n");
        reads(&p, "after a line");
        setL(p.s, 0, KL_ECHOCTL);
        KEYS(&p, "no echoctl", "\x04");
        reads(&p, "no echoctl");
        ptyClose(&p);
    }
}

/* How many backspaces rub out what's typed, starting from a column some output left. */
static void tabScript(const char *label, const char *output, const void *typed, size_t len, uint32_t l_off) {
    Pty p;
    if (!ptyOpen(&p)) return;
    setL(p.s, 0, l_off);
    if (output[0]) {
        (void)!write(p.s, output, strlen(output));
        char swallowed[64];
        (void)drain(p.m, swallowed, sizeof(swallowed));
    }
    keys(&p, label, typed, len);
    KEYS(&p, label, "\x7f");
    KEYS(&p, label, "\x7f");
    KEYS(&p, label, "\n");
    reads(&p, label);
    ptyClose(&p);
}

#define T(label, output, text, l_off) tabScript((label), (output), (text), sizeof(text) - 1, (l_off))

static void secTabErase(void) {
    section("L5 erasing across a tab");
    T("at the start", "", "\t", 0);
    T("after letters", "", "ab\t", 0);
    T("full stop", "", "abcdefgh\t", 0);
    T("two tabs", "", "\t\t", 0);
    T("tab between", "", "a\tb\t", 0);
    T("after a control", "", "\x01\t", 0);
    T("after a control, no echoctl", "", "\x01\t", KL_ECHOCTL);
    T("after output", "xyz", "\t", 0);
    T("after output and letters", "xyz", "ab\t", 0);
    T("after long output", "0123456789", "\t", 0);
    T("after output with a line end", "abc\ndef", "\t", 0);
    T("after output with a return", "abc\rd", "\t", 0);
    T("after output with a tab", "a\tb", "\t", 0);
    T("after output with a backspace", "abc\b", "\t", 0);
    T("after output, control", "xyz", "\x01\t", 0);
    T("no echoe", "", "ab\t", KL_ECHOE);
    /* Typed text that has already been echoed, with output in between. */
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        KEYS(&p, "output during a line", "ab");
        (void)!write(p.s, "xyz", 3);
        char swallowed[64];
        (void)drain(p.m, swallowed, sizeof(swallowed));
        KEYS(&p, "output during a line", "\t");
        KEYS(&p, "output during a line", "\x7f");
        KEYS(&p, "output during a line", "\x7f\x7f");
        KEYS(&p, "output during a line", "\n");
        reads(&p, "output during a line");
        ptyClose(&p);
    }
    /* Output with OPOST off still moves the column, or doesn't. */
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setO(p.s, 0, KO_OPOST);
        (void)!write(p.s, "xyz", 3);
        char swallowed[64];
        (void)drain(p.m, swallowed, sizeof(swallowed));
        setO(p.s, KO_OPOST, 0);
        KEYS(&p, "output with no opost", "\t");
        KEYS(&p, "output with no opost", "\x7f");
        KEYS(&p, "output with no opost", "\n");
        reads(&p, "output with no opost");
        ptyClose(&p);
    }
    /* A second line starts from wherever the first left the cursor. */
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        KEYS(&p, "second line", "abc\n");
        KEYS(&p, "second line", "\t");
        KEYS(&p, "second line", "\x7f");
        KEYS(&p, "second line", "\n");
        reads(&p, "second line");
        setO(p.s, 0, KO_ONLCR);
        KEYS(&p, "second line, no onlcr", "abc\n");
        KEYS(&p, "second line, no onlcr", "\t");
        KEYS(&p, "second line, no onlcr", "\x7f");
        KEYS(&p, "second line, no onlcr", "\n");
        reads(&p, "second line, no onlcr");
        ptyClose(&p);
    }
    /* Echoing with the tab expanded, and erased again. */
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setO(p.s, KO_TAB3, 0);
        KEYS(&p, "tab3", "ab\t");
        KEYS(&p, "tab3", "\x7f");
        KEYS(&p, "tab3", "\x7f\x7f");
        KEYS(&p, "tab3", "\n");
        reads(&p, "tab3");
        ptyClose(&p);
    }
}

static void utf8Script(const char *label, uint32_t i_on, const void *typed, size_t len, uint32_t l_off) {
    Pty p;
    if (!ptyOpen(&p)) return;
    setI(p.s, i_on, 0);
    setL(p.s, 0, l_off);
    keys(&p, label, typed, len);
    KEYS(&p, label, "\n");
    reads(&p, label);
    ptyClose(&p);
}

#define U(label, i_on, text, l_off) utf8Script((label), (i_on), (text), sizeof(text) - 1, (l_off))

static void secUtf8(void) {
    section("L5 IUTF8");
    U("two bytes", KI_IUTF8, "a\xc3\xa9\x7f", 0);
    U("three bytes", KI_IUTF8, "a\xe6\x97\xa5\x7f", 0);
    U("four bytes", KI_IUTF8, "a\xf0\x9f\x98\x80\x7f", 0);
    U("erase twice", KI_IUTF8, "a\xc3\xa9\xc3\xa8\x7f\x7f", 0);
    U("lone continuation", KI_IUTF8, "a\xa9\x7f", 0);
    U("off", 0, "a\xc3\xa9\x7f", 0);
    U("off, twice", 0, "a\xc3\xa9\x7f\x7f", 0);
    U("no echoe", KI_IUTF8, "a\xc3\xa9\x7f" "b", KL_ECHOE);
    U("then more", KI_IUTF8, "a\xc3\xa9\x7f" "b", 0);
    U("kill", KI_IUTF8, "a\xc3\xa9" "b\x15", 0);
    U("werase", KI_IUTF8, "x a\xc3\xa9" "b\x17", 0);
    U("werase, off", 0, "x a\xc3\xa9" "b\x17", 0);
    U("tab after", KI_IUTF8, "\xc3\xa9\t\x7f", 0);
    U("tab after, off", 0, "\xc3\xa9\t\x7f", 0);
    U("reprint", KI_IUTF8, "\xc3\xa9" "b\x12", 0);
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setI(p.s, KI_IUTF8, 0);
        setO(p.s, KO_TAB3, 0);
        (void)!write(p.s, "\xc3\xa9\t|", 4);
        unsigned char out[64];
        char text[256];
        const size_t n = drain(p.m, out, sizeof(out));
        say("tab3 output, iutf8: \"%s\"", esc(text, sizeof(text), out, n));
        ptyClose(&p);
    }
}

static void longTyping(Pty *p, size_t count, char byte, size_t *letters, size_t *bells, size_t *others) {
    static unsigned char many[2048];
    static unsigned char echoed[8192];
    size_t remaining = count;
    while (remaining > 0) {
        const size_t now = remaining < sizeof(many) ? remaining : sizeof(many);
        memset(many, byte, now);
        if (write(p->m, many, now) != (ssize_t)now) {
            say("typing: %s", errName(errno));
        }
        const size_t n = drain(p->m, echoed, sizeof(echoed));
        for (size_t i = 0; i < n; i++) {
            if (echoed[i] == (unsigned char)byte) {
                ++*letters;
            } else if (echoed[i] == 7) {
                ++*bells;
            } else {
                ++*others;
            }
        }
        remaining -= now;
    }
}

static void secLongLine(void) {
    section("L5 L7 a long line");
    Pty p;
    if (!ptyOpen(&p)) return;
    size_t letters = 0, bells = 0, others = 0;
    longTyping(&p, 6000, 'x', &letters, &bells, &others);
    say("echoed letters %zu, bells %s, others %zu", letters, bells ? "some" : "none", others);
    KEYS(&p, "erase at the limit", "\x7f");
    KEYS(&p, "more at the limit", "y");
    KEYS(&p, "one too many", "z");
    KEYS(&p, "newline", "\n");
    static unsigned char got[8192];
    char text[64];
    const ssize_t r = read(p.s, got, sizeof(got));
    say("read %zd, last two \"%s\"", r, r >= 2 ? esc(text, sizeof(text), got + r - 2, 2) : "");
    reads(&p, "after");
    /* Each of the keys that ends a line still can. */
    letters = bells = others = 0;
    longTyping(&p, 4200, 'x', &letters, &bells, &others);
    KEYS(&p, "the end-of-file key at the limit", "\x04");
    const ssize_t r2 = read(p.s, got, sizeof(got));
    say("read %zd", r2);
    reads(&p, "after");
    longTyping(&p, 4200, 'x', &letters, &bells, &others);
    setCc(p.s, KV_EOL, '%');
    KEYS(&p, "VEOL at the limit", "%");
    const ssize_t r3 = read(p.s, got, sizeof(got));
    say("read %zd", r3);
    reads(&p, "after");
    longTyping(&p, 4200, 'x', &letters, &bells, &others);
    setL(p.s, 0, KL_ECHO);
    KEYS(&p, "kill at the limit", "\x15" "a\n");
    setL(p.s, KL_ECHO, 0);
    reads(&p, "after");
    ptyClose(&p);
}

/* --- L7: the other flags --- */

static void inputScript(const char *label, uint32_t i_on, uint32_t i_off, bool raw, const void *typed, size_t len, uint32_t l_off) {
    Pty p;
    if (!ptyOpen(&p)) return;
    if (raw) {
        setRaw(p.s);
    }
    setI(p.s, i_on, i_off);
    setL(p.s, 0, l_off);
    keys(&p, label, typed, len);
    if (!raw) {
        KEYS(&p, label, "\n");
    }
    reads(&p, label);
    ptyClose(&p);
}

#define IN(label, on, off, text) inputScript((label), (on), (off), false, (text), sizeof(text) - 1, 0)
#define INR(label, on, off, text) inputScript((label), (on), (off), true, (text), sizeof(text) - 1, 0)
#define INX(label, on, off, text, l_off) inputScript((label), (on), (off), false, (text), sizeof(text) - 1, (l_off))

static void secInputFlags(void) {
    section("L7 input flags, in line mode");
    IN("istrip", KI_ISTRIP, 0, "a\xe1\xb1");
    IN("istrip return", KI_ISTRIP, 0, "\x8d" "b");
    IN("istrip key", KI_ISTRIP, 0, "ab\xff" "c");
    IN("iuclc", KI_IUCLC, 0, "aBcD");
    INX("iuclc no iexten", KI_IUCLC, 0, "aBcD", KL_IEXTEN);
    IN("inlcr", KI_INLCR, KI_ICRNL, "a\nb");
    IN("igncr", KI_IGNCR, 0, "a\rb");
    IN("igncr no icrnl", KI_IGNCR, KI_ICRNL, "a\rb");
    IN("inlcr icrnl", KI_INLCR | KI_ICRNL, 0, "a\nb\rc");
    IN("inlcr igncr", KI_INLCR | KI_IGNCR, 0, "a\nb\rc");
    IN("no translation", 0, KI_ICRNL, "a\rb");
    IN("ignbrk", KI_IGNBRK, 0, "a");
    IN("brkint", KI_BRKINT, 0, "a");
    IN("inpck", KI_INPCK, 0, "a\xff" "b");
    IN("ignpar", KI_IGNPAR, 0, "a\xff" "b");
    IN("parmrk", KI_PARMRK, 0, "a\xff" "b");
    IN("parmrk inpck", KI_PARMRK | KI_INPCK, 0, "a\xff" "b");
    IN("parmrk istrip", KI_PARMRK | KI_ISTRIP, 0, "a\xff" "b");
    IN("imaxbel", KI_IMAXBEL, 0, "a");
    section("L7 input flags, raw");
    INR("istrip", KI_ISTRIP, 0, "a\xe1\xb1");
    INR("iuclc", KI_IUCLC, 0, "aBcD");
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setRaw(p.s);
        setI(p.s, KI_IUCLC, 0);
        setL(p.s, 0, KL_IEXTEN);
        KEYS(&p, "iuclc no iexten", "aBcD");
        reads(&p, "iuclc no iexten");
        ptyClose(&p);
    }
    INR("parmrk", KI_PARMRK, 0, "a\xff" "b");
    INR("parmrk istrip", KI_PARMRK | KI_ISTRIP, 0, "a\xff" "b");
}

static void outputScript(const char *label, uint32_t o_on, uint32_t o_off, const void *data, size_t len) {
    Pty p;
    if (!ptyOpen(&p)) return;
    setO(p.s, o_on, o_off);
    const ssize_t wrote = write(p.s, data, len);
    unsigned char out[4096];
    const size_t n = drain(p.m, out, sizeof(out));
    char a[512], b[1024];
    say("%s: wrote %zd of \"%s\" gives \"%s\"", label, wrote, esc(a, sizeof(a), data, len), esc(b, sizeof(b), out, n));
    /* The same again a byte at a time, which must come to the same thing. */
    Pty q;
    if (!ptyOpen(&q)) return;
    setO(q.s, o_on, o_off);
    for (size_t i = 0; i < len; i++) {
        (void)!write(q.s, (const char *)data + i, 1);
    }
    unsigned char out2[4096];
    const size_t n2 = drain(q.m, out2, sizeof(out2));
    if (n2 != n || memcmp(out, out2, n) != 0) {
        say("%s: byte at a time gives \"%s\"", label, esc(b, sizeof(b), out2, n2));
    }
    ptyClose(&q);
    ptyClose(&p);
}

#define OUT(label, on, off, text) outputScript((label), (on), (off), (text), sizeof(text) - 1)

static void secOutputFlags(void) {
    section("L7 output flags");
    const char sample[] = "ab\rcd\n\tx\r\r\n\nyZ\b\tq";
    const struct {
        const char *name;
        uint32_t on, off;
    } modes[] = {
        {"defaults", 0, 0},
        {"no opost", 0, KO_OPOST},
        {"no opost, all on", KO_OLCUC | KO_OCRNL | KO_ONOCR | KO_ONLRET | KO_TAB3, KO_OPOST},
        {"no onlcr", 0, KO_ONLCR},
        {"ocrnl", KO_OCRNL, 0},
        {"ocrnl no onlcr", KO_OCRNL, KO_ONLCR},
        {"onocr", KO_ONOCR, 0},
        {"onocr no onlcr", KO_ONOCR, KO_ONLCR},
        {"onlret", KO_ONLRET, 0},
        {"onlret no onlcr", KO_ONLRET, KO_ONLCR},
        {"onlret onocr", KO_ONLRET | KO_ONOCR, KO_ONLCR},
        {"ocrnl onlret", KO_OCRNL | KO_ONLRET, 0},
        {"ocrnl onlret onocr", KO_OCRNL | KO_ONLRET | KO_ONOCR, 0},
        {"olcuc", KO_OLCUC, 0},
        {"tab1", KO_TAB1, 0},
        {"tab2", KO_TAB2, 0},
        {"tab3", KO_TAB3, 0},
        {"tab3 onlret", KO_TAB3 | KO_ONLRET, KO_ONLCR},
        {"tab3 olcuc", KO_TAB3 | KO_OLCUC, 0},
        {"ofill", KO_OFILL, 0},
        {"ofill ofdel", KO_OFILL | KO_OFDEL, 0},
        {"nl1", KO_NL1, 0},
        {"cr3", KO_CR3, 0},
        {"bs1", KO_BS1, 0},
        {"vt1", KO_VT1, 0},
        {"ff1", KO_FF1, 0},
    };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        outputScript(modes[i].name, modes[i].on, modes[i].off, sample, sizeof(sample) - 1);
    }
    OUT("tab3 columns", KO_TAB3, 0, "\t|a\t|abcdefg\t|abcdefgh\t|");
    OUT("tab3 control doesn't move", KO_TAB3, 0, "\x01\x02\t|");
    OUT("tab3 del doesn't move", KO_TAB3, 0, "\x7f\t|");
    OUT("tab3 high bytes", KO_TAB3, 0, "\xc3\xa9\t|");
    OUT("tab3 backspace at the start", KO_TAB3, 0, "\b\t|");
    OUT("tab3 backspace", KO_TAB3, 0, "abc\b\b\t|");
    OUT("tab3 return", KO_TAB3, 0, "abc\r\t|");
    OUT("tab3 line feed no onlcr", KO_TAB3, KO_ONLCR, "abc\n\t|");
    OUT("tab3 line feed no onlcr onlret", KO_TAB3 | KO_ONLRET, KO_ONLCR, "abc\n\t|");
    OUT("tab3 line feed", KO_TAB3, 0, "abc\n\t|");
    OUT("onocr after text", KO_ONOCR, 0, "\rabc\r\rdef\n\r");
    OUT("olcuc high bytes", KO_OLCUC, 0, "az\xe9{");
    /* The column outlives a write, and a change of flags. */
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setO(p.s, KO_TAB3, 0);
        (void)!write(p.s, "abc", 3);
        (void)!write(p.s, "\t|", 2);
        setO(p.s, 0, KO_OPOST);
        (void)!write(p.s, "abc", 3);
        setO(p.s, KO_OPOST, 0);
        (void)!write(p.s, "\t|", 2);
        unsigned char out[256];
        char text[512];
        const size_t n = drain(p.m, out, sizeof(out));
        say("column across writes: \"%s\"", esc(text, sizeof(text), out, n));
        ptyClose(&p);
    }
    /* What a master's write is subject to: nothing of the slave's. */
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setRaw(p.s);
        setO(p.s, KO_OLCUC | KO_TAB3, 0);
        const ssize_t wrote = write(p.m, "a\tb\n", 4);
        sayResult("master write", wrote);
        reads(&p, "master write");
        char none[16];
        say("master echo: %zu", drain(p.m, none, sizeof(none)));
        ptyClose(&p);
    }
}

/* --- L8: VMIN and VTIME --- */

typedef struct {
    int fd;
    const char *text;
    size_t len;
    int after_ms;
    int then_ms;
    const char *more;
} Later;

static void *typeLater(void *raw) {
    Later *later = (Later *)raw;
    sleepMs(later->after_ms);
    (void)!write(later->fd, later->text, later->len);
    if (later->more) {
        sleepMs(later->then_ms);
        (void)!write(later->fd, later->more, strlen(later->more));
    }
    return NULL;
}

/* A read of the blocking descriptor, and whether it took about the timer's time. */
static void timedRead(const char *label, int fd, size_t want, Later *later) {
    pthread_t thread;
    if (later) {
        pthread_create(&thread, NULL, typeLater, later);
    }
    const long long start = nowMs();
    unsigned char got[64];
    const ssize_t n = read(fd, got, want);
    const long long took = nowMs() - start;
    int saved = errno;
    if (later) {
        pthread_join(thread, NULL);
    }
    errno = saved;
    char text[128];
    char result[32];
    const char *when = took < 200 ? "soon" : (took < 500 ? "timer" : "long");
    say("%s: %s \"%s\" %s", label, resultName(result, sizeof(result), n),
        n > 0 ? esc(text, sizeof(text), got, (size_t)n) : "", when);
}

static void showPoll(const char *label, int fd) {
    struct pollfd pfd = {fd, POLLIN | POLLOUT | POLLPRI, 0};
    const int r = poll(&pfd, 1, 0);
    say("%s: poll %d %s", label, r, pollName(pfd.revents));
}

static void secVminVtime(void) {
    section("L8 VMIN and VTIME");
    Pty p;
    if (!ptyOpen(&p)) return;
    setRaw(p.s);
    const int b = open(p.path, O_RDWR | O_NOCTTY);
    /* VMIN 0, VTIME 0 */
    setCc(p.s, KV_MIN, 0);
    setCc(p.s, KV_TIME, 0);
    timedRead("0,0 empty", b, 8, NULL);
    timedRead("0,0 empty, non-blocking", p.s, 8, NULL);
    showPoll("0,0 empty", p.s);
    KEYS(&p, "0,0", "abc");
    showPoll("0,0 bytes", p.s);
    timedRead("0,0 bytes, asks 2", b, 2, NULL);
    timedRead("0,0 rest", b, 8, NULL);
    /* VMIN 0, VTIME 3 */
    setCc(p.s, KV_TIME, 3);
    timedRead("0,3 empty", b, 8, NULL);
    timedRead("0,3 empty, non-blocking", p.s, 8, NULL);
    showPoll("0,3 empty", p.s);
    KEYS(&p, "0,3", "abc");
    showPoll("0,3 bytes", p.s);
    timedRead("0,3 bytes", b, 8, NULL);
    {
        Later later = {p.m, "k", 1, 50, 0, NULL};
        timedRead("0,3 byte after a moment", b, 8, &later);
    }
    {
        Later later = {p.m, "k", 1, 50, 50, "l"};
        timedRead("0,3 two after a moment", b, 8, &later);
        reads(&p, "0,3 two after a moment");
    }
    /* VMIN 2, VTIME 0 */
    setCc(p.s, KV_MIN, 2);
    setCc(p.s, KV_TIME, 0);
    timedRead("2,0 empty, non-blocking", p.s, 8, NULL);
    showPoll("2,0 empty", p.s);
    KEYS(&p, "2,0", "a");
    showPoll("2,0 one byte", p.s);
    timedRead("2,0 one byte, non-blocking", p.s, 8, NULL);
    KEYS(&p, "2,0", "a");
    timedRead("2,0 asks for one, one there", b, 1, NULL);
    {
        KEYS(&p, "2,0", "a");
        Later later = {p.m, "b", 1, 300, 0, NULL};
        timedRead("2,0 second comes later", b, 8, &later);
    }
    KEYS(&p, "2,0", "abcde");
    showPoll("2,0 five bytes", p.s);
    timedRead("2,0 five bytes", b, 3, NULL);
    timedRead("2,0 rest", b, 8, NULL);
    /* VMIN 2, VTIME 3 */
    setCc(p.s, KV_TIME, 3);
    timedRead("2,3 empty, non-blocking", p.s, 8, NULL);
    showPoll("2,3 empty", p.s);
    {
        Later later = {p.m, "a", 1, 600, 0, NULL};
        timedRead("2,3 no timer before the first", b, 8, &later);
    }
    KEYS(&p, "2,3", "a");
    showPoll("2,3 one byte", p.s);
    timedRead("2,3 one byte", b, 8, NULL);
    KEYS(&p, "2,3", "ab");
    timedRead("2,3 two bytes", b, 8, NULL);
    KEYS(&p, "2,3", "a");
    timedRead("2,3 asks for one, one there", b, 1, NULL);
    KEYS(&p, "2,3", "abc");
    timedRead("2,3 asks for two, three there", b, 2, NULL);
    timedRead("2,3 rest", p.s, 8, NULL);
    /* The timer starts again at each byte. */
    setCc(p.s, KV_MIN, 3);
    {
        Later later = {p.m, "a", 1, 0, 200, "b"};
        const long long start = nowMs();
        pthread_t thread;
        pthread_create(&thread, NULL, typeLater, &later);
        unsigned char got[8];
        const ssize_t n = read(b, got, sizeof(got));
        const long long took = nowMs() - start;
        pthread_join(thread, NULL);
        say("3,3 restart: %zd after %s", n, took < 400 ? "one timer" : "two timers");
    }
    close(b);
    ptyClose(&p);
}

/* --- L11: FIONREAD --- */

static void showInq(const char *label, int fd) {
    int v = -1;
    const int r = ioctl(fd, K_FIONREAD, &v);
    if (r != 0) {
        say("%s: FIONREAD %s", label, errName(errno));
    } else {
        say("%s: FIONREAD %d", label, v);
    }
}

static void secFionread(void) {
    section("L11 FIONREAD");
    Pty p;
    if (!ptyOpen(&p)) return;
    setL(p.s, 0, KL_ECHO);
    showInq("empty", p.s);
    KEYS(&p, "typed", "abc");
    showInq("unfinished", p.s);
    KEYS(&p, "typed", "\n");
    showInq("a line", p.s);
    KEYS(&p, "typed", "de");
    showInq("a line and unfinished", p.s);
    reads(&p, "first");
    showInq("unfinished again", p.s);
    KEYS(&p, "typed", "\x04");
    showInq("released by the key", p.s);
    reads(&p, "released");
    KEYS(&p, "typed", "\x04");
    showInq("the key alone", p.s);
    reads(&p, "the key alone");
    KEYS(&p, "typed", "one\n\x04two\n");
    showInq("a line, the key, a line", p.s);
    reads(&p, "all");
    KEYS(&p, "typed", "hello\n");
    {
        char two[2];
        (void)!read(p.s, two, 2);
    }
    showInq("part of a line read", p.s);
    reads(&p, "rest");
    /* With the mode changed, what is there counts another way. */
    KEYS(&p, "typed", "abc");
    setL(p.s, 0, KL_ICANON);
    setI(p.s, 0, KI_ICRNL);
    showInq("raw, was unfinished", p.s);
    reads(&p, "raw, was unfinished");
    KEYS(&p, "typed", "xy");
    showInq("raw", p.s);
    setL(p.s, KL_ICANON, 0);
    showInq("line mode, was raw", p.s);
    KEYS(&p, "typed", "\n");
    showInq("line mode, raw text ended", p.s);
    reads(&p, "line mode, raw text ended");
    /* A master counts what a read of it would get. */
    (void)!write(p.s, "a\nb", 3);
    {
        char none[1];
        (void)drain(p.s, none, 0);
    }
    sleepMs(20);
    showInq("master", p.m);
    showInq("slave, master has output", p.s);
    ptyClose(&p);
}

/* --- L13: packet mode --- */

static void showPacket(const char *label, Pty *p, size_t want) {
    showPoll(label, p->m);
    unsigned char got[64];
    const ssize_t n = read(p->m, got, want);
    char text[256];
    if (n < 0) {
        say("%s: read %s", label, errName(errno));
    } else {
        say("%s: read %zd \"%s\"", label, n, esc(text, sizeof(text), got, (size_t)n));
    }
}

static void secPacket(void) {
    section("L13 packet mode");
    Pty p;
    if (!ptyOpen(&p)) return;
    int v = -1;
    sayResult("TIOCGPKT", ioctl(p.m, K_TIOCGPKT, &v));
    say("packet is %d", v);
    int on = 1;
    sayResult("TIOCPKT on", ioctl(p.m, K_TIOCPKT, &on));
    sayResult("TIOCGPKT", ioctl(p.m, K_TIOCGPKT, &v));
    say("packet is %d", v);
    sayResult("TIOCPKT on slave", ioctl(p.s, K_TIOCPKT, &on));
    showPacket("nothing", &p, 16);
    (void)!write(p.s, "pk", 2);
    sleepMs(20);
    showPacket("data", &p, 16);
    (void)!write(p.s, "xy", 2);
    sleepMs(20);
    showPacket("data, asks 2", &p, 2);
    showPacket("data rest", &p, 16);
    (void)!write(p.s, "z", 1);
    sleepMs(20);
    showPacket("data, asks 1", &p, 1);
    showPacket("data rest", &p, 16);

    /* Flushes, from each side, in each direction. */
    const struct {
        const char *name;
        long which;
    } kinds[] = {{"TCIFLUSH", 0}, {"TCOFLUSH", 1}, {"TCIOFLUSH", 2}};
    for (size_t i = 0; i < 3; i++) {
        char label[64];
        snprintf(label, sizeof(label), "slave %s", kinds[i].name);
        sayResult(label, ioctl(p.s, K_TCFLSH, kinds[i].which));
        showPacket(label, &p, 16);
        showPacket(label, &p, 16);
        snprintf(label, sizeof(label), "master %s", kinds[i].name);
        sayResult(label, ioctl(p.m, K_TCFLSH, kinds[i].which));
        showPacket(label, &p, 16);
        showPacket(label, &p, 16);
    }
    /* Two flushes together, and a flush with data waiting. */
    sayResult("slave TCIFLUSH", ioctl(p.s, K_TCFLSH, 0L));
    sayResult("slave TCOFLUSH", ioctl(p.s, K_TCFLSH, 1L));
    showPacket("both", &p, 16);
    showPacket("both", &p, 16);
    (void)!write(p.s, "data", 4);
    sleepMs(20);
    sayResult("slave TCIFLUSH with data", ioctl(p.s, K_TCFLSH, 0L));
    showPacket("status first", &p, 16);
    showPacket("then data", &p, 16);
    (void)!write(p.s, "data", 4);
    sleepMs(20);
    sayResult("slave TCOFLUSH with data", ioctl(p.s, K_TCFLSH, 1L));
    showPacket("status first", &p, 16);
    showPacket("then data", &p, 16);
    (void)!write(p.s, "data", 4);
    sleepMs(20);
    sayResult("master TCIFLUSH with data", ioctl(p.m, K_TCFLSH, 0L));
    showPacket("status first", &p, 16);
    showPacket("then data", &p, 16);
    /* A flush clears what is queued for the reader on that side. */
    KEYS(&p, "typed", "one\n");
    sayResult("slave TCIFLUSH with keys", ioctl(p.s, K_TCFLSH, 0L));
    showPacket("keys", &p, 16);
    showPacket("keys", &p, 16);
    reads(&p, "keys");
    KEYS(&p, "typed", "one\n");
    showPacket("echo", &p, 16);
    sayResult("master TCOFLUSH with keys", ioctl(p.m, K_TCFLSH, 1L));
    showPacket("keys", &p, 16);
    reads(&p, "keys");

    /* The flow control state. */
    KTermios t;
    kget(p.s, &t);
    t.i &= ~KI_IXON;
    kset(p.s, &t);
    showPacket("ixon off", &p, 16);
    t.i |= KI_IXON;
    kset(p.s, &t);
    showPacket("ixon on", &p, 16);
    t.cc[KV_START] = 0x01;
    kset(p.s, &t);
    showPacket("vstart changed", &p, 16);
    t.cc[KV_START] = 0x11;
    kset(p.s, &t);
    showPacket("vstart back", &p, 16);
    t.cc[KV_STOP] = 0x01;
    kset(p.s, &t);
    showPacket("vstop changed", &p, 16);
    t.cc[KV_STOP] = 0x13;
    t.i &= ~KI_IXON;
    kset(p.s, &t);
    showPacket("ixon off, vstop back", &p, 16);
    t.i |= KI_IXON;
    kset(p.m, &t);
    showPacket("ixon on through the master", &p, 16);
    kset(p.m, &t);
    showPacket("nothing changed", &p, 16);
    sayResult("TCSETSF", ioctl(p.s, K_TCSETSF, &t));
    showPacket("TCSETSF", &p, 16);
    showPacket("TCSETSF", &p, 16);
    sayResult("TCSETSW", ioctl(p.s, K_TCSETSW, &t));
    showPacket("TCSETSW", &p, 16);

    /* Turning it off with a status waiting. */
    sayResult("slave TCIFLUSH", ioctl(p.s, K_TCFLSH, 0L));
    on = 0;
    sayResult("TIOCPKT off", ioctl(p.m, K_TIOCPKT, &on));
    showPacket("off, status waiting", &p, 16);
    on = 1;
    sayResult("TIOCPKT on", ioctl(p.m, K_TIOCPKT, &on));
    showPacket("on again", &p, 16);
    sayResult("slave TCIFLUSH", ioctl(p.s, K_TCFLSH, 0L));
    sayResult("TIOCPKT on again", ioctl(p.m, K_TIOCPKT, &on));
    showPacket("on twice", &p, 16);
    showPacket("on twice", &p, 16);
    ptyClose(&p);

    /* Output flushed with no packet mode. */
    if (!ptyOpen(&p)) return;
    (void)!write(p.s, "data", 4);
    sleepMs(20);
    sayResult("no packet, slave TCOFLUSH", ioctl(p.s, K_TCFLSH, 1L));
    showPacket("no packet", &p, 16);
    KEYS(&p, "typed", "keys\n");
    sayResult("no packet, master TCOFLUSH", ioctl(p.m, K_TCFLSH, 1L));
    reads(&p, "no packet");
    (void)!write(p.s, "data", 4);
    sleepMs(20);
    sayResult("no packet, master TCIFLUSH", ioctl(p.m, K_TCFLSH, 0L));
    showPacket("no packet", &p, 16);
    ptyClose(&p);
}

/* --- L14: errors reach the caller as what they are --- */

static int noSessionErrors(void *arg) {
    (void)arg;
    if (setsid() < 0) {
        say("setsid: %s", errName(errno));
    }
    const int fd = open("/dev/tty", O_RDWR);
    if (fd >= 0) {
        say("open /dev/tty: opened");
        close(fd);
    } else {
        say("open /dev/tty: %s", errName(errno));
    }
    return 0;
}

static void secErrors(void) {
    section("L14 errors");
    Pty p;
    if (!ptyOpen(&p)) return;
    struct stat st;
    /* A number well above what anything here has open. */
    const char missing[] = "/dev/pts/3999";
    const int fd = open(missing, O_RDWR);
    sayResult("open a free number", fd);
    sayResult("stat a free number", stat(missing, &st));
    sayResult("chmod a free number", chmod(missing, 0600));
    sayResult("chown a free number", chown(missing, (uid_t)-1, (gid_t)-1));
    sayResult("TIOCGPTN on a slave", ioctl(p.s, K_TIOCGPTN, &st));
    int zero = 0;
    sayResult("TIOCSPTLCK on a slave", ioctl(p.s, K_TIOCSPTLCK, &zero));
    sayResult("TIOCGPKT on a slave", ioctl(p.s, K_TIOCGPKT, &zero));
    sayResult("unknown request", ioctl(p.s, 0x5499UL, &zero));
    sayResult("TCFLSH of nothing known", ioctl(p.s, K_TCFLSH, 7L));
    sayResult("TCGETS with no struct", ioctl(p.s, K_TCGETS, NULL));
    sayResult("TCSETS with no struct", ioctl(p.s, K_TCSETS, NULL));
    sayResult("TIOCGWINSZ with no struct", ioctl(p.s, K_TIOCGWINSZ, NULL));
    sayResult("TIOCSWINSZ with no struct", ioctl(p.s, K_TIOCSWINSZ, NULL));
    sayResult("FIONREAD with no int", ioctl(p.s, K_FIONREAD, NULL));
    sayResult("TIOCGPGRP with no int", ioctl(p.m, K_TIOCGPGRP, NULL));
    sayResult("ioctl on a closed descriptor", ioctl(500, K_TCGETS, &zero));
    int fds[2];
    if (pipe(fds) == 0) {
        sayResult("TCGETS on a pipe", ioctl(fds[0], K_TCGETS, &zero));
        close(fds[0]);
        close(fds[1]);
    }
    ptyClose(&p);

    /* A locked slave, and one whose master has gone. */
    const int locked = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    unsigned num = 0;
    sayResult("TIOCGPTN", ioctl(locked, K_TIOCGPTN, &num));
    char path[32];
    snprintf(path, sizeof(path), "/dev/pts/%u", num);
    const int early = open(path, O_RDWR | O_NOCTTY);
    sayResult("open a locked slave", early);
    close(locked);
    const int late = open(path, O_RDWR | O_NOCTTY);
    sayResult("open after the master went", late);
    sayResult("stat after the master went", stat(path, &st));

    /* No terminal for /dev/tty to mean. */
    const pid_t child = procSpawn(noSessionErrors, NULL);
    procFinish("no session", child);
}

/* --- L15, L16, L18, L19, L25: a session --- */

typedef void (*LeaderFn)(Pty *p);

typedef struct {
    LeaderFn fn;
    Pty pty;
} LeaderStart;

static int leaderMain(void *raw) {
    LeaderStart *start = (LeaderStart *)raw;
    Pty *p = &start->pty;
    if (setsid() < 0) {
        say("setsid: %s", errName(errno));
        return 1;
    }
    p->m = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (p->m < 0 || !ptyNames(p)) {
        return 1;
    }
    /* Without O_NOCTTY: this is what makes it the session's. */
    p->s = open(p->path, O_RDWR | O_NONBLOCK);
    if (p->s < 0) {
        say("open slave: %s", errName(errno));
        return 1;
    }
    /* A shell's habit, and what lets this go on using the terminal once in the background. */
    signal(SIGTTOU, SIG_IGN);
    start->fn(p);
    return 0;
}

static void sessionCase(const char *name, LeaderFn fn) {
    say("-- %s", name);
    clearMarks();
    LeaderStart *start = (LeaderStart *)calloc(1, sizeof(*start));
    start->fn = fn;
    const pid_t pid = procSpawn(leaderMain, start);
    procFinish("leader", pid);
    free(start);
}

/* What a child in the background is to do. */
typedef struct {
    Pty *p;
    bool ignore;
    bool block;
    bool handler;
    bool restart;
    bool nonblocking;
    bool zero;
    bool grandchild;
    int sig;
} Job;

static int hitIndex(int sig) {
    switch (sig) {
        case SIGTTIN: return 1;
        case SIGTTOU: return 2;
        case SIGWINCH: return 3;
        case SIGINT: return 4;
        default: return 5;
    }
}

static void countSignal(int sig) {
    __atomic_add_fetch(&gShared->hits[hitIndex(sig)], 1, __ATOMIC_RELAXED);
}

static int hitsOf(int sig) {
    return __atomic_load_n(&gShared->hits[hitIndex(sig)], __ATOMIC_RELAXED);
}

static void installHandler(int sig, bool restart) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = countSignal;
    sa.sa_flags = restart ? SA_RESTART : 0;
    sigaction(sig, &sa, NULL);
}

static void jobDisposition(const Job *job) {
    signal(job->sig, job->ignore ? SIG_IGN : SIG_DFL);
    if (job->block) {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, job->sig);
        sigprocmask(SIG_BLOCK, &set, NULL);
    }
    if (job->handler) {
        installHandler(job->sig, job->restart);
    }
}

static void jobSayPending(const Job *job) {
    if (job->block) {
        sigset_t set;
        sigemptyset(&set);
        sigpending(&set);
        say("job: signal pending: %s", sigismember(&set, job->sig) ? "yes" : "no");
    }
}

static int bgReader(void *raw) {
    const Job *job = (const Job *)raw;
    setpgid(0, 0);
    jobDisposition(job);
    const int s = open(job->p->path, O_RDWR | O_NOCTTY | (job->nonblocking ? O_NONBLOCK : 0));
    mark(1);
    if (job->grandchild) {
        /* Waits for the parent that kept this group from being orphaned to go. */
        awaitMark(2);
    }
    char buf[64];
    const ssize_t n = read(s, buf, job->zero ? 0 : sizeof(buf));
    char text[128];
    char result[32];
    say("job: read %s \"%s\"", resultName(result, sizeof(result), n), n > 0 ? esc(text, sizeof(text), buf, (size_t)n) : "");
    jobSayPending(job);
    if (job->handler) {
        say("job: handler ran: %s", hitsOf(job->sig) > 0 ? "yes" : "no");
    }
    close(s);
    return 0;
}

static int bgWriter(void *raw) {
    const Job *job = (const Job *)raw;
    setpgid(0, 0);
    jobDisposition(job);
    const int s = open(job->p->path, O_RDWR | O_NOCTTY);
    mark(1);
    if (job->grandchild) {
        awaitMark(2);
    }
    const ssize_t n = write(s, "out", job->zero ? 0 : 3);
    const int saved = errno;
    if (job->restart) {
        /* It goes on the moment it's in the foreground: let that be said first. */
        awaitMark(14);
    }
    errno = saved;
    char result[32];
    say("job: write %s", resultName(result, sizeof(result), n));
    jobSayPending(job);
    if (job->handler) {
        say("job: handler ran: %s", hitsOf(job->sig) > 0 ? "yes" : "no");
    }
    close(s);
    return 0;
}

static void masterOutput(const char *label, Pty *p) {
    unsigned char out[64];
    char text[256];
    const size_t n = drain(p->m, out, sizeof(out));
    say("%s: master has \"%s\"", label, esc(text, sizeof(text), out, n));
}

static void setForeground(const char *label, Pty *p, pid_t group) {
    int value = (int)group;
    sayResult(label, ioctl(p->s, K_TIOCSPGRP, &value));
}

/* Runs a job in the background and sees it through, for a read. */
static void runBgRead(Pty *p, Job job) {
    job.p = p;
    job.sig = SIGTTIN;
    const pid_t me = getpgrp();
    const pid_t pid = procSpawn(bgReader, &job);
    setpgid(pid, pid);
    awaitMark(1);
    /* It either stops, or ends by itself, or goes round its handler for ever. */
    int status = 0;
    if (job.handler && job.restart) {
        for (int i = 0; i < 2000 && hitsOf(SIGTTIN) == 0; i++) {
            sleepMs(1);
        }
        say("leader: handler has run: %s", hitsOf(SIGTTIN) > 0 ? "yes" : "no");
        setForeground("leader: job to the foreground", p, pid);
        (void)!write(p->m, "line\n", 5);
        procFinish("job", pid);
        setForeground("leader: back", p, me);
        return;
    }
    const pid_t waited = waitpid(pid, &status, WUNTRACED);
    if (waited != pid) {
        say("leader: wait: %s", errName(errno));
        return;
    }
    if (WIFSTOPPED(status)) {
        say("leader: job stopped by %s", signalName(WSTOPSIG(status)));
        setForeground("leader: job to the foreground", p, pid);
        (void)!write(p->m, "line\n", 5);
        sleepMs(30);
        kill(-pid, SIGCONT);
        procFinish("job", pid);
        setForeground("leader: back", p, me);
    } else if (WIFEXITED(status)) {
        say("leader: job exited %d", WEXITSTATUS(status));
        procReaped(pid);
    } else {
        say("leader: job other");
        procReaped(pid);
    }
    char drained[64];
    (void)drain(p->m, drained, sizeof(drained));
}

static void runBgWrite(Pty *p, Job job, bool tostop) {
    job.p = p;
    job.sig = SIGTTOU;
    setL(p->s, tostop ? KL_TOSTOP : 0, tostop ? 0 : KL_TOSTOP);
    const pid_t me = getpgrp();
    const pid_t pid = procSpawn(bgWriter, &job);
    setpgid(pid, pid);
    awaitMark(1);
    int status = 0;
    if (job.handler && job.restart) {
        for (int i = 0; i < 2000 && hitsOf(SIGTTOU) == 0; i++) {
            sleepMs(1);
        }
        say("leader: handler has run: %s", hitsOf(SIGTTOU) > 0 ? "yes" : "no");
        setForeground("leader: job to the foreground", p, pid);
        mark(14);
        procFinish("job", pid);
        masterOutput("leader", p);
        setForeground("leader: back", p, me);
        return;
    }
    const pid_t waited = waitpid(pid, &status, WUNTRACED);
    if (waited != pid) {
        say("leader: wait: %s", errName(errno));
        return;
    }
    if (WIFSTOPPED(status)) {
        say("leader: job stopped by %s", signalName(WSTOPSIG(status)));
        masterOutput("leader, stopped", p);
        setForeground("leader: job to the foreground", p, pid);
        sleepMs(30);
        kill(-pid, SIGCONT);
        procFinish("job", pid);
        masterOutput("leader", p);
        setForeground("leader: back", p, me);
    } else if (WIFEXITED(status)) {
        say("leader: job exited %d", WEXITSTATUS(status));
        procReaped(pid);
        masterOutput("leader", p);
    } else {
        say("leader: job other");
        procReaped(pid);
    }
}

static void caseBgReadDefault(Pty *p) { runBgRead(p, (Job){0}); }
static void caseBgReadIgnored(Pty *p) { runBgRead(p, (Job){.ignore = true}); }
static void caseBgReadBlocked(Pty *p) { runBgRead(p, (Job){.block = true}); }
static void caseBgReadHandler(Pty *p) { runBgRead(p, (Job){.handler = true}); }
static void caseBgReadRestart(Pty *p) { runBgRead(p, (Job){.handler = true, .restart = true}); }
static void caseBgReadNonblocking(Pty *p) { runBgRead(p, (Job){.nonblocking = true}); }
static void caseBgReadZero(Pty *p) { runBgRead(p, (Job){.zero = true}); }

static void caseBgWriteOff(Pty *p) { runBgWrite(p, (Job){0}, false); }
static void caseBgWriteDefault(Pty *p) { runBgWrite(p, (Job){0}, true); }
static void caseBgWriteIgnored(Pty *p) { runBgWrite(p, (Job){.ignore = true}, true); }
static void caseBgWriteBlocked(Pty *p) { runBgWrite(p, (Job){.block = true}, true); }
static void caseBgWriteHandler(Pty *p) { runBgWrite(p, (Job){.handler = true}, true); }
static void caseBgWriteRestart(Pty *p) { runBgWrite(p, (Job){.handler = true, .restart = true}, true); }
static void caseBgWriteZero(Pty *p) { runBgWrite(p, (Job){.zero = true}, true); }

/* The other terminal is nobody's: nothing about the session applies to it. */
static int bgOtherTerminal(void *raw) {
    Pty *other = (Pty *)raw;
    setpgid(0, 0);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    const int s = open(other->path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    char buf[8];
    sayResult("job: read of another terminal", read(s, buf, sizeof(buf)));
    sayResult("job: write to another terminal", write(s, "x", 1));
    int value = 0;
    sayResult("job: TIOCGPGRP on another terminal", ioctl(s, K_TIOCGPGRP, &value));
    close(s);
    return 0;
}

static void caseBgOtherTerminal(Pty *p) {
    Pty other;
    if (!ptyOpen(&other)) return;
    setL(other.s, KL_TOSTOP, 0);
    setL(p->s, KL_TOSTOP, 0);
    const pid_t pid = procSpawn(bgOtherTerminal, &other);
    setpgid(pid, pid);
    procFinish("job", pid);
    ptyClose(&other);
}

/* An orphaned group: the child that held it to the session goes. */
typedef struct {
    Pty *p;
    bool write_it;
    bool ignore;
    bool block;
    pid_t volatile grandchild;
} OrphanStart;

static int orphanMiddle(void *raw) {
    OrphanStart *start = (OrphanStart *)raw;
    Job *job = (Job *)calloc(1, sizeof(*job));
    job->p = start->p;
    job->ignore = start->ignore;
    job->block = start->block;
    job->grandchild = true;
    job->sig = start->write_it ? SIGTTOU : SIGTTIN;
    const pid_t pid = procSpawn(start->write_it ? bgWriter : bgReader, job);
    setpgid(pid, pid);
    start->grandchild = pid;
    awaitMark(1);
    return 0;
}

static void runOrphan(Pty *p, bool write_it, bool ignore, bool block) {
    if (write_it) {
        setL(p->s, KL_TOSTOP, 0);
    }
    /* In shared memory: the middle process is a copy on Linux, and has to say who it made. */
    OrphanStart *start = (OrphanStart *)mmap(NULL, sizeof(OrphanStart), PROT_READ | PROT_WRITE,
                                             MAP_SHARED | MAP_ANON, -1, 0);
    start->p = p;
    start->write_it = write_it;
    start->ignore = ignore;
    start->block = block;
    const pid_t middle = procSpawn(orphanMiddle, start);
    procFinish("middle", middle);
    /* Now its child's parent is outside the session, as far as the group is concerned. */
    mark(2);
    /* It reports for itself; nothing here can wait for it, as it is not this one's child. */
    sleepMs(100);
    masterOutput("leader", p);
    munmap(start, sizeof(OrphanStart));
}

static void caseOrphanRead(Pty *p) { runOrphan(p, false, false, false); }
static void caseOrphanReadIgnored(Pty *p) { runOrphan(p, false, true, false); }
static void caseOrphanReadBlocked(Pty *p) { runOrphan(p, false, false, true); }
static void caseOrphanWrite(Pty *p) { runOrphan(p, true, false, false); }
static void caseOrphanWriteIgnored(Pty *p) { runOrphan(p, true, true, false); }
static void caseOrphanWriteBlocked(Pty *p) { runOrphan(p, true, false, true); }

/* A foreground reader and writer, for comparison. */
static void caseForeground(Pty *p) {
    setL(p->s, KL_TOSTOP, KL_ECHO);
    sayResult("write", write(p->s, "fg", 2));
    masterOutput("leader", p);
    KEYS(p, "typed", "ok\n");
    reads(p, "leader");
}

/* --- L18 --- */

static void relation(const char *label, int value, pid_t self) {
    if (value == (int)self) {
        say("%s: self", label);
    } else if (value == 0) {
        say("%s: 0", label);
    } else {
        say("%s: another", label);
    }
}

static int childSleeps(void *raw) {
    (void)raw;
    awaitMark(3);
    return 0;
}

static int otherSession(void *raw) {
    (void)raw;
    setsid();
    gShared->other_group = (int)getpgrp();
    mark(4);
    awaitMark(3);
    return 0;
}

static void caseForegroundGroups(Pty *p) {
    int value = -1;
    const pid_t me = getpgrp();
    sayResult("TIOCGPGRP", ioctl(p->s, K_TIOCGPGRP, &value));
    relation("the group", value, me);
    sayResult("TIOCGPGRP on the master", ioctl(p->m, K_TIOCGPGRP, &value));
    relation("the group", value, me);
    const pid_t tc = tcgetpgrp(p->s);
    relation("tcgetpgrp", (int)tc, me);

    value = -5;
    sayResult("TIOCSPGRP negative", ioctl(p->s, K_TIOCSPGRP, &value));
    value = 0;
    sayResult("TIOCSPGRP zero", ioctl(p->s, K_TIOCSPGRP, &value));
    value = 2000000000;
    sayResult("TIOCSPGRP nobody", ioctl(p->s, K_TIOCSPGRP, &value));
    const pid_t other = procSpawn(otherSession, NULL);
    awaitMark(4);
    value = gShared->other_group;
    sayResult("TIOCSPGRP another session's", ioctl(p->s, K_TIOCSPGRP, &value));
    value = (int)me;
    sayResult("TIOCSPGRP its own", ioctl(p->s, K_TIOCSPGRP, &value));
    sayResult("tcsetpgrp its own", tcsetpgrp(p->s, me));
    sayResult("TIOCSPGRP through the master", ioctl(p->m, K_TIOCSPGRP, &value));

    /* A group of this session, and then back. */
    const pid_t member = procSpawn(childSleeps, NULL);
    setpgid(member, member);
    value = (int)member;
    sayResult("TIOCSPGRP a member's", ioctl(p->s, K_TIOCSPGRP, &value));
    int seen = -1;
    (void)ioctl(p->s, K_TIOCGPGRP, &seen);
    say("foreground is the member: %s", seen == (int)member ? "yes" : "no");
    value = (int)me;
    sayResult("TIOCSPGRP back", ioctl(p->s, K_TIOCSPGRP, &value));
    mark(3);
    procFinish("member", member);
    procFinish("other", other);

    /* Another terminal is not this session's. */
    Pty q;
    if (!ptyOpen(&q)) return;
    value = (int)me;
    sayResult("TIOCSPGRP on another", ioctl(q.s, K_TIOCSPGRP, &value));
    sayResult("TIOCGPGRP on another", ioctl(q.s, K_TIOCGPGRP, &value));
    sayResult("TIOCSPGRP on another's master", ioctl(q.m, K_TIOCSPGRP, &value));
    value = -1;
    sayResult("TIOCGPGRP on another's master", ioctl(q.m, K_TIOCGPGRP, &value));
    relation("the group", value, me);
    sayResult("TIOCGSID on another", ioctl(q.s, K_TIOCGSID, &value));
    sayResult("TIOCGSID on another's master", ioctl(q.m, K_TIOCGSID, &value));
    ptyClose(&q);

    value = -1;
    sayResult("TIOCGSID", ioctl(p->s, K_TIOCGSID, &value));
    relation("the session", value, getsid(0));
    value = -1;
    sayResult("TIOCGSID on the master", ioctl(p->m, K_TIOCGSID, &value));
    relation("the session", value, getsid(0));
}

/* Gives up the terminal as a process that doesn't lead the session. */
static int notLeaderDetaches(void *raw) {
    Pty *p = (Pty *)raw;
    signal(SIGTTIN, SIG_DFL);
    const int s = open(p->path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    int value = -1;
    sayResult("member: TIOCGPGRP", ioctl(s, K_TIOCGPGRP, &value));
    sayResult("member: TIOCNOTTY", ioctl(s, K_TIOCNOTTY));
    sayResult("member: TIOCGPGRP after", ioctl(s, K_TIOCGPGRP, &value));
    sayResult("member: TIOCGSID after", ioctl(s, K_TIOCGSID, &value));
    sayResult("member: TIOCNOTTY again", ioctl(s, K_TIOCNOTTY));
    const int tty = open("/dev/tty", O_RDWR);
    sayResult("member: open /dev/tty after", tty >= 0 ? 0 : -1);
    if (tty >= 0) {
        close(tty);
    }
    /* Out of the foreground and with no controlling terminal: no check applies. */
    setpgid(0, 0);
    char buf[8];
    sayResult("member: read after", read(s, buf, sizeof(buf)));
    close(s);
    return 0;
}

static void caseNoTtyMember(Pty *p) {
    int value = 0;
    sayResult("TIOCNOTTY on the master", ioctl(p->m, K_TIOCNOTTY));
    Pty q;
    if (!ptyOpen(&q)) return;
    sayResult("TIOCNOTTY on another", ioctl(q.s, K_TIOCNOTTY));
    ptyClose(&q);
    const pid_t pid = procSpawn(notLeaderDetaches, p);
    procFinish("member", pid);
    /* It stays this session's. */
    sayResult("TIOCGPGRP", ioctl(p->s, K_TIOCGPGRP, &value));
    const int tty = open("/dev/tty", O_RDWR);
    sayResult("open /dev/tty", tty >= 0 ? 0 : -1);
    if (tty >= 0) {
        close(tty);
    }
}

static int memberAfterLeaderDetached(void *raw) {
    Pty *p = (Pty *)raw;
    const int s = open(p->path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    awaitMark(5);
    int value = -1;
    sayResult("member: TIOCGPGRP", ioctl(s, K_TIOCGPGRP, &value));
    const int tty = open("/dev/tty", O_RDWR);
    sayResult("member: open /dev/tty", tty >= 0 ? 0 : -1);
    if (tty >= 0) {
        close(tty);
    }
    close(s);
    return 0;
}

/* The session loses its terminal. What else a leader's TIOCNOTTY does is L22's. */
static void caseNoTtyLeader(Pty *p) {
    signal(SIGHUP, SIG_IGN);
    const pid_t pid = procSpawn(memberAfterLeaderDetached, p);
    sayResult("TIOCNOTTY", ioctl(p->s, K_TIOCNOTTY));
    mark(5);
    procFinish("member", pid);
    int value = -1;
    sayResult("TIOCGPGRP", ioctl(p->s, K_TIOCGPGRP, &value));
    sayResult("TIOCGSID", ioctl(p->s, K_TIOCGSID, &value));
    sayResult("TIOCGSID on the master", ioctl(p->m, K_TIOCGSID, &value));
    const int tty = open("/dev/tty", O_RDWR);
    sayResult("open /dev/tty", tty >= 0 ? 0 : -1);
    if (tty >= 0) {
        close(tty);
    }
    sayResult("TIOCNOTTY again", ioctl(p->s, K_TIOCNOTTY));
    /* And can take it again. */
    sayResult("TIOCSCTTY", ioctl(p->s, K_TIOCSCTTY, 0L));
    value = -1;
    sayResult("TIOCGSID", ioctl(p->s, K_TIOCGSID, &value));
    relation("the session", value, getsid(0));
}

static void showBusy(const char *label, const char *path);

static void secExclusive(void) {
    section("L18 TIOCEXCL");
    Pty p;
    if (!ptyOpen(&p)) return;
    int value = -1;
    sayResult("TIOCGEXCL", ioctl(p.s, K_TIOCGEXCL, &value));
    say("exclusive: %d", value);
    showBusy("open", p.path);
    sayResult("TIOCEXCL", ioctl(p.s, K_TIOCEXCL));
    sayResult("TIOCGEXCL", ioctl(p.s, K_TIOCGEXCL, &value));
    say("exclusive: %d", value);
    value = -1;
    sayResult("TIOCGEXCL on the master", ioctl(p.m, K_TIOCGEXCL, &value));
    say("exclusive: %d", value);
    showBusy("open", p.path);
    sayResult("TIOCNXCL", ioctl(p.s, K_TIOCNXCL));
    sayResult("TIOCGEXCL", ioctl(p.s, K_TIOCGEXCL, &value));
    say("exclusive: %d", value);
    showBusy("open", p.path);
    sayResult("TIOCEXCL on the master", ioctl(p.m, K_TIOCEXCL));
    sayResult("TIOCGEXCL on the master", ioctl(p.m, K_TIOCGEXCL, &value));
    say("exclusive: %d", value);
    sayResult("TIOCGEXCL on the slave", ioctl(p.s, K_TIOCGEXCL, &value));
    say("exclusive: %d", value);
    showBusy("open", p.path);
    sayResult("TIOCNXCL on the master", ioctl(p.m, K_TIOCNXCL));
    sayResult("TIOCGEXCL on the master", ioctl(p.m, K_TIOCGEXCL, &value));
    say("exclusive: %d", value);
    ptyClose(&p);
}

static void caseExclusiveControlling(Pty *p) {
    int value = -1;
    sayResult("TIOCEXCL", ioctl(p->s, K_TIOCEXCL));
    const int tty = open("/dev/tty", O_RDWR);
    sayResult("open /dev/tty", tty >= 0 ? 0 : -1);
    if (tty >= 0) {
        close(tty);
    }
    sayResult("TIOCGEXCL", ioctl(p->s, K_TIOCGEXCL, &value));
    say("exclusive: %d", value);
    sayResult("TIOCNXCL", ioctl(p->s, K_TIOCNXCL));
}

static void secQueuesAndBreaks(void) {
    section("L18 TIOCOUTQ");
    Pty p;
    if (!ptyOpen(&p)) return;
    int value = -1;
    sayResult("slave, empty", ioctl(p.s, K_TIOCOUTQ, &value));
    say("value %d", value);
    value = -1;
    sayResult("master, empty", ioctl(p.m, K_TIOCOUTQ, &value));
    say("value %d", value);
    (void)!write(p.s, "abcd", 4);
    sleepMs(20);
    value = -1;
    sayResult("slave, output unread", ioctl(p.s, K_TIOCOUTQ, &value));
    say("value %d", value);
    value = -1;
    sayResult("master, output unread", ioctl(p.m, K_TIOCOUTQ, &value));
    say("value %d", value);
    (void)!write(p.m, "abcd", 4);
    sleepMs(20);
    value = -1;
    sayResult("slave, keys unread", ioctl(p.s, K_TIOCOUTQ, &value));
    say("value %d", value);
    value = -1;
    sayResult("master, keys unread", ioctl(p.m, K_TIOCOUTQ, &value));
    say("value %d", value);
    ptyClose(&p);

    section("L18 TCSBRK");
    if (!ptyOpen(&p)) return;
    (void)!write(p.s, "abcd", 4);
    sleepMs(20);
    const struct {
        const char *name;
        unsigned long request;
        long arg;
    } breaks[] = {
        {"TCSBRK 0", K_TCSBRK, 0},
        {"TCSBRK 1", K_TCSBRK, 1},
        {"TCSBRKP 0", K_TCSBRKP, 0},
        {"TCSBRKP 3", K_TCSBRKP, 3},
    };
    for (size_t i = 0; i < 4; i++) {
        char label[64];
        long long start = nowMs();
        long result = ioctl(p.s, breaks[i].request, breaks[i].arg);
        snprintf(label, sizeof(label), "%s on the slave", breaks[i].name);
        sayResult(label, result);
        say("%s", nowMs() - start < 200 ? "soon" : "not soon");
        start = nowMs();
        result = ioctl(p.m, breaks[i].request, breaks[i].arg);
        snprintf(label, sizeof(label), "%s on the master", breaks[i].name);
        sayResult(label, result);
        say("%s", nowMs() - start < 200 ? "soon" : "not soon");
    }
    unsigned char out[16];
    char text[64];
    const size_t n = drain(p.m, out, sizeof(out));
    say("output after: \"%s\"", esc(text, sizeof(text), out, n));
    ptyClose(&p);

    section("L18 TIOCSTI");
    if (!ptyOpen(&p)) return;
    const char byte = 'k';
    sayResult("not the controlling terminal", ioctl(p.s, K_TIOCSTI, &byte));
    reads(&p, "not the controlling terminal");
    ptyClose(&p);
}

static void caseInjectControlling(Pty *p) {
    const char byte = '\n';
    sayResult("TIOCSTI", ioctl(p->s, K_TIOCSTI, &byte));
    reads(p, "TIOCSTI");
    char echoed[16];
    say("echo %zu", drain(p->m, echoed, sizeof(echoed)));
}

/* --- L19 --- */

static int masterNeverControls(void *raw) {
    (void)raw;
    setsid();
    /* Neither open claims it. */
    const int m1 = open("/dev/ptmx", O_RDWR);
    sayResult("open master with no O_NOCTTY", m1 >= 0 ? 0 : -1);
    int value = -1;
    sayResult("TIOCGSID on the master", ioctl(m1, K_TIOCGSID, &value));
    int tty = open("/dev/tty", O_RDWR);
    sayResult("open /dev/tty", tty >= 0 ? 0 : -1);
    if (tty >= 0) {
        close(tty);
    }
    /* Its slave, with O_NOCTTY, isn't claimed either. */
    Pty p;
    memset(&p, 0, sizeof(p));
    p.m = m1;
    if (!ptyNames(&p)) return 1;
    p.s = open(p.path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    sayResult("TIOCGSID on the slave", ioctl(p.s, K_TIOCGSID, &value));
    /* A request on the master acts on the slave. */
    sayResult("TIOCSCTTY on the master", ioctl(p.m, K_TIOCSCTTY, 0L));
    value = -1;
    sayResult("TIOCGSID on the slave", ioctl(p.s, K_TIOCGSID, &value));
    relation("the session", value, getsid(0));
    tty = open("/dev/tty", O_RDWR | O_NONBLOCK);
    sayResult("open /dev/tty", tty >= 0 ? 0 : -1);
    if (tty >= 0) {
        unsigned char echoed[16];
        char text[64];
        (void)!write(tty, "t", 1);
        const size_t n = drain(p.m, echoed, sizeof(echoed));
        say("/dev/tty writes to \"%s\" on the master", esc(text, sizeof(text), echoed, n));
        close(tty);
    }
    return 0;
}

static int claimCases(void *raw) {
    (void)raw;
    setsid();
    Pty a;
    memset(&a, 0, sizeof(a));
    a.m = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (!ptyNames(&a)) return 1;
    a.s = open(a.path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    int value = -1;
    sayResult("TIOCGPGRP before any claim", ioctl(a.s, K_TIOCGPGRP, &value));
    /* An open without O_NOCTTY claims it, for a leader with none. */
    const int claiming = open(a.path, O_RDWR | O_NONBLOCK);
    sayResult("TIOCGPGRP after an open", ioctl(a.s, K_TIOCGPGRP, &value));
    relation("the group", value, getpgrp());
    close(claiming);
    sayResult("TIOCSCTTY, its own", ioctl(a.s, K_TIOCSCTTY, 0L));
    /* A second terminal isn't claimed by an open, or by asking. */
    Pty b;
    memset(&b, 0, sizeof(b));
    b.m = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (!ptyNames(&b)) return 1;
    b.s = open(b.path, O_RDWR | O_NONBLOCK);
    sayResult("TIOCGSID on a second", ioctl(b.s, K_TIOCGSID, &value));
    sayResult("TIOCSCTTY on a second", ioctl(b.s, K_TIOCSCTTY, 0L));
    return 0;
}

static int nonLeaderClaims(void *raw) {
    Pty *a = (Pty *)raw;
    /* A process that doesn't lead its session can't. */
    const int s = open(a->path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    int value = -1;
    sayResult("not a leader: TIOCSCTTY", ioctl(s, K_TIOCSCTTY, 0L));
    sayResult("not a leader: TIOCSCTTY force", ioctl(s, K_TIOCSCTTY, 1L));
    close(s);
    const int t = open(a->path, O_RDWR | O_NONBLOCK);
    sayResult("not a leader, after an open: TIOCGSID", ioctl(t, K_TIOCGSID, &value));
    close(t);
    return 0;
}

static int otherLeaderClaims(void *raw) {
    Pty *a = (Pty *)raw;
    setsid();
    const int s = open(a->path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    int value = -1;
    sayResult("another session: TIOCSCTTY", ioctl(s, K_TIOCSCTTY, 0L));
    sayResult("another session: TIOCGSID", ioctl(s, K_TIOCGSID, &value));
    /* An open by a leader with none doesn't take one that's taken. */
    const int t = open(a->path, O_RDWR | O_NONBLOCK);
    sayResult("another session, after an open: TIOCGSID", ioctl(t, K_TIOCGSID, &value));
    close(t);
    close(s);
    return 0;
}

static void caseClaimsFromOutside(Pty *p) {
    const pid_t member = procSpawn(nonLeaderClaims, p);
    procFinish("member", member);
    const pid_t other = procSpawn(otherLeaderClaims, p);
    procFinish("other", other);
}

/* --- L25, L26 --- */

static void showSize(const char *label, int fd) {
    KWinsize w = {0, 0, 0, 0};
    if (ioctl(fd, K_TIOCGWINSZ, &w) != 0) {
        say("%s: TIOCGWINSZ %s", label, errName(errno));
    } else {
        say("%s: size %u %u %u %u", label, w.row, w.col, w.xpixel, w.ypixel);
    }
}

static void resize(const char *label, int fd, uint16_t row, uint16_t col, uint16_t x, uint16_t y) {
    const int before = hitsOf(SIGWINCH);
    KWinsize w = {row, col, x, y};
    const int result = ioctl(fd, K_TIOCSWINSZ, &w);
    /* Any shim call delivers what is waiting. */
    (void)poll(NULL, 0, 20);
    say("%s: %s, signals %d", label, errName(result == 0 ? 0 : errno), hitsOf(SIGWINCH) - before);
}

static void countJobWinch(int sig) {
    (void)sig;
    __atomic_add_fetch(&gShared->hits[7], 1, __ATOMIC_RELAXED);
}

static int waitsForWinch(void *raw) {
    (void)raw;
    setpgid(0, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = countJobWinch;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);
    mark(6);
    awaitMark(7);
    (void)poll(NULL, 0, 20);
    say("job: signals %d", __atomic_load_n(&gShared->hits[7], __ATOMIC_RELAXED));
    mark(8);
    awaitMark(9);
    (void)poll(NULL, 0, 20);
    say("job: signals %d", __atomic_load_n(&gShared->hits[7], __ATOMIC_RELAXED));
    return 0;
}

static void caseWinsize(Pty *p) {
    installHandler(SIGWINCH, true);
    showSize("fresh", p->s);
    resize("the same, zeros", p->s, 0, 0, 0, 0);
    resize("new", p->s, 24, 80, 0, 0);
    showSize("slave", p->s);
    showSize("master", p->m);
    resize("the same", p->s, 24, 80, 0, 0);
    resize("row", p->s, 25, 80, 0, 0);
    resize("col", p->s, 25, 81, 0, 0);
    resize("xpixel", p->s, 25, 81, 640, 0);
    resize("ypixel", p->s, 25, 81, 640, 480);
    showSize("slave", p->s);
    showSize("master", p->m);
    resize("the same", p->s, 25, 81, 640, 480);
    resize("through the master", p->m, 30, 90, 1, 2);
    showSize("slave", p->s);
    showSize("master", p->m);
    resize("the same through the master", p->m, 30, 90, 1, 2);
    resize("the same through the slave", p->s, 30, 90, 1, 2);
}

/* The foreground group gets it, and nobody else. */
static void caseWinsizeTarget(Pty *p) {
    installHandler(SIGWINCH, true);
    const pid_t me = getpgrp();
    const pid_t pid = procSpawn(waitsForWinch, NULL);
    setpgid(pid, pid);
    awaitMark(6);
    resize("leader in the foreground", p->s, 10, 20, 0, 0);
    mark(7);
    awaitMark(8);
    setForeground("job to the foreground", p, pid);
    resize("job in the foreground", p->s, 11, 21, 0, 0);
    setForeground("back", p, me);
    mark(9);
    procFinish("job", pid);
}

/* A terminal that's nobody's: the size is kept, and no signal goes anywhere. */
static void secWinsizeNoSession(void) {
    section("L25 L26 no foreground group");
    Pty p;
    if (!ptyOpen(&p)) return;
    clearMarks();
    installHandler(SIGWINCH, true);
    resize("new", p.s, 24, 80, 3, 4);
    showSize("slave", p.s);
    showSize("master", p.m);
    resize("through the master", p.m, 25, 81, 5, 6);
    showSize("slave", p.s);
    showSize("master", p.m);
    signal(SIGWINCH, SIG_DFL);
    ptyClose(&p);
}

/* --- the host's numbers, where a program asks in those --- */

static void showBusy(const char *label, const char *path) {
    const int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    sayResult(label, fd >= 0 ? 0 : -1);
    if (fd >= 0) {
        close(fd);
    }
}

/* What the C library's calls come down to on the host each flavour is built for. */
#if UNDER_VPROC
static int hostGet(int fd, struct termios *t) {
    return ioctl(fd, TIOCGETA, t);
}
static int hostSet(int fd, const struct termios *t) {
    return ioctl(fd, TIOCSETA, t);
}
static int hostFlushInput(int fd) {
    int which = FREAD;
    return ioctl(fd, TIOCFLUSH, &which);
}
static int hostDrain(int fd) {
    return ioctl(fd, TIOCDRAIN, 0L);
}
#else
static int hostGet(int fd, struct termios *t) {
    return tcgetattr(fd, t);
}
static int hostSet(int fd, const struct termios *t) {
    return tcsetattr(fd, TCSANOW, t);
}
static int hostFlushInput(int fd) {
    return tcflush(fd, TCIFLUSH);
}
static int hostDrain(int fd) {
    return tcdrain(fd);
}
#endif

static void secHostNumbers(void) {
    section("L1 L5 L7 L16 through the host's own numbers");
    Pty p;
    if (!ptyOpen(&p)) return;
    struct termios t;
    if (hostGet(p.s, &t) != 0) {
        say("get: %s", errName(errno));
        ptyClose(&p);
        return;
    }
    say("speed is 38400: %s %s", cfgetospeed(&t) == B38400 ? "yes" : "no", cfgetispeed(&t) == B38400 ? "yes" : "no");
    say("cs8 %d cread %d", (t.c_cflag & CSIZE) == CS8, (t.c_cflag & CREAD) != 0);
#define SHOW(field, flag) say(#flag " %d", (t.field & (flag)) != 0)
    SHOW(c_iflag, ICRNL);
    SHOW(c_iflag, IXON);
    SHOW(c_iflag, ISTRIP);
    SHOW(c_iflag, IUTF8);
    SHOW(c_oflag, OPOST);
    SHOW(c_oflag, ONLCR);
    SHOW(c_lflag, ISIG);
    SHOW(c_lflag, ICANON);
    SHOW(c_lflag, ECHO);
    SHOW(c_lflag, ECHOE);
    SHOW(c_lflag, ECHOK);
    SHOW(c_lflag, ECHONL);
    SHOW(c_lflag, NOFLSH);
    SHOW(c_lflag, TOSTOP);
    SHOW(c_lflag, ECHOCTL);
    SHOW(c_lflag, ECHOPRT);
    SHOW(c_lflag, ECHOKE);
    SHOW(c_lflag, IEXTEN);
#undef SHOW
    /* Everything set through these, read back in Linux's. */
#define SET(field, flag, kfield, kflag)                                                 \
    do {                                                                                \
        struct termios change = t;                                                      \
        change.field |= (flag);                                                         \
        (void)hostSet(p.s, &change);                                                    \
        KTermios k;                                                                     \
        kget(p.s, &k);                                                                  \
        say(#flag " set: %d", (k.kfield & (kflag)) != 0);                               \
        change.field &= ~(tcflag_t)(flag);                                              \
        (void)hostSet(p.s, &change);                                                    \
        kget(p.s, &k);                                                                  \
        say(#flag " clear: %d", (k.kfield & (kflag)) != 0);                             \
    } while (0)
    SET(c_iflag, ISTRIP, i, KI_ISTRIP);
    SET(c_iflag, IUTF8, i, KI_IUTF8);
    SET(c_iflag, IGNBRK, i, KI_IGNBRK);
    SET(c_iflag, BRKINT, i, KI_BRKINT);
    SET(c_iflag, IGNPAR, i, KI_IGNPAR);
    SET(c_iflag, PARMRK, i, KI_PARMRK);
    SET(c_iflag, INPCK, i, KI_INPCK);
    SET(c_iflag, IMAXBEL, i, KI_IMAXBEL);
    SET(c_oflag, TAB3, o, KO_TAB3);
    SET(c_lflag, ECHONL, l, KL_ECHONL);
    SET(c_lflag, TOSTOP, l, KL_TOSTOP);
    SET(c_lflag, ECHOPRT, l, KL_ECHOPRT);
    SET(c_lflag, ECHOKE, l, KL_ECHOKE);
    SET(c_cflag, CSTOPB, c, 0x40u);
    SET(c_cflag, PARENB, c, 0x100u);
#undef SET
    {
        struct termios change = t;
        change.c_cflag = (change.c_cflag & ~(tcflag_t)CSIZE) | CS7;
        (void)hostSet(p.s, &change);
        KTermios k;
        kget(p.s, &k);
        say("cs7: size %x", k.c & 0x30u);
        cfsetospeed(&change, B9600);
        cfsetispeed(&change, B9600);
        (void)hostSet(p.s, &change);
        kget(p.s, &k);
        say("9600: %x", k.c & 0x100fu);
        struct termios back;
        (void)hostGet(p.s, &back);
        say("9600 reads back: %d", cfgetospeed(&back) == B9600);
    }
    ptyClose(&p);

    if (!ptyOpen(&p)) return;
    KEYS(&p, "typed", "one\n");
    sayResult("tcflush", hostFlushInput(p.s));
    reads(&p, "tcflush");
    sayResult("tcdrain", hostDrain(p.s));
    int value = -1;
    sayResult("TIOCOUTQ", ioctl(p.s, TIOCOUTQ, &value));
    say("value %d", value);
    sayResult("TIOCEXCL", ioctl(p.s, TIOCEXCL));
    showBusy("open", p.path);
    sayResult("TIOCNXCL", ioctl(p.s, TIOCNXCL));
    showBusy("open", p.path);
    sayResult("TIOCNOTTY", ioctl(p.s, TIOCNOTTY));
    const char byte = 'k';
    sayResult("TIOCSTI", ioctl(p.s, TIOCSTI, &byte));
    ptyClose(&p);
}

/* --- the keys of a session --- */

static void caseInterruptKey(Pty *p) {
    /* The foreground is this process: a key's signal comes here. */
    installHandler(SIGINT, true);
    setL(p->s, 0, KL_ECHO);
    SIGNAL_KEYS(p, "a line, then the key", "one\ntwo\x03");
    (void)poll(NULL, 0, 20);
    say("signals %d", hitsOf(SIGINT));
    reads(p, "read");
    setL(p->s, 0, KL_ISIG);
    SIGNAL_KEYS(p, "isig clear", "one\ntwo\x03" "\n");
    (void)poll(NULL, 0, 20);
    say("signals %d", hitsOf(SIGINT));
    reads(p, "read");
}

static void caseSignalKey(Pty *p) {
    /* The foreground is this process: a key's signal comes here. */
    installHandler(SIGQUIT, true);
    setL(p->s, 0, KL_ECHO);
    SIGNAL_KEYS(p, "a line, then the key", "one\ntwo\x1c");
    (void)poll(NULL, 0, 20);
    say("signals %d", hitsOf(SIGQUIT));
    reads(p, "read");
    setL(p->s, KL_NOFLSH, 0);
    SIGNAL_KEYS(p, "the same with noflsh", "one\ntwo\x1c");
    (void)poll(NULL, 0, 20);
    say("signals %d", hitsOf(SIGQUIT));
    KEYS(p, "the rest of the line", "\n");
    reads(p, "read");
    setL(p->s, 0, KL_NOFLSH | KL_ISIG);
    SIGNAL_KEYS(p, "isig clear", "one\ntwo\x1c" "\n");
    (void)poll(NULL, 0, 20);
    say("signals %d", hitsOf(SIGQUIT));
    reads(p, "read");
    setL(p->s, KL_ISIG | KL_ECHO | KL_ECHOPRT, 0);
    KEYS(p, "a key to erase", "ab");
    KEYS(p, "erased", "\x7f");
    SIGNAL_KEYS(p, "the key with the erase open", "\x1c");
    (void)poll(NULL, 0, 20);
    say("signals %d", hitsOf(SIGQUIT));
    KEYS(p, "the next", "z\n");
    reads(p, "read");
}


/* --- finer than the above --- */

/* Keys one at a time, to see when each thing shows. */
static void stepKeys(Pty *p, const char *label, const char *const *steps, size_t count) {
    for (size_t i = 0; i < count; i++) {
        keys(p, label, steps[i], strlen(steps[i]));
    }
}

#define STEPS(p, label, ...)                                                    \
    do {                                                                        \
        const char *const steps[] = {__VA_ARGS__};                              \
        stepKeys((p), (label), steps, sizeof(steps) / sizeof(steps[0]));        \
    } while (0)

static void prtScript(const char *label, uint32_t l_on, uint32_t l_off, uint32_t i_on, const char *const *steps, size_t count) {
    Pty p;
    if (!ptyOpen(&p)) return;
    setL(p.s, l_on, l_off);
    setI(p.s, i_on, 0);
    stepKeys(&p, label, steps, count);
    reads(&p, label);
    ptyClose(&p);
}

#define PRT(label, l_on, l_off, i_on, ...)                                      \
    do {                                                                        \
        const char *const steps[] = {__VA_ARGS__};                              \
        prtScript((label), (l_on), (l_off), (i_on), steps, sizeof(steps) / sizeof(steps[0])); \
    } while (0)

static void secEchoPrt(void) {
    section("L5 ECHOPRT in steps");
    PRT("all gone, then more", KL_ECHOPRT, 0, 0, "a", "\x7f", "\x7f", "b", "\n");
    PRT("two gone, then more", KL_ECHOPRT, 0, 0, "ab", "\x7f", "\x7f", "c", "\n");
    PRT("one gone, then more", KL_ECHOPRT, 0, 0, "ab", "\x7f", "c", "\n");
    PRT("newline while open", KL_ECHOPRT, 0, 0, "ab", "\x7f", "\n");
    PRT("eof while open", KL_ECHOPRT, 0, 0, "ab", "\x7f", "\x04", "\n");
    PRT("kill while open", KL_ECHOPRT, 0, 0, "ab", "\x7f", "\x15", "c\n");
    PRT("kill while open, echoke", KL_ECHOPRT | KL_ECHOE | KL_ECHOK | KL_ECHOKE, 0, 0, "abc", "\x7f", "\x15", "c\n");
    PRT("werase while open", KL_ECHOPRT, 0, 0, "x yz", "\x7f", "\x17", "c\n");
    PRT("reprint while open", KL_ECHOPRT, 0, 0, "abc", "\x7f", "\x12", "c\n");
    PRT("lnext while open", KL_ECHOPRT, 0, 0, "abc", "\x7f", "\x16", "\x7f", "\n");
    PRT("a tab", KL_ECHOPRT, 0, 0, "a\tb", "\x7f", "\x7f", "\x7f", "c\n");
    PRT("control, echoctl", KL_ECHOPRT, 0, 0, "a\x01" "b", "\x7f", "\x7f", "\x7f", "c\n");
    PRT("control, no echoctl", KL_ECHOPRT, KL_ECHOCTL, 0, "a\x01" "b", "\x7f", "\x7f", "\x7f", "c\n");
    PRT("multibyte", KL_ECHOPRT, 0, KI_IUTF8, "a\xc3\xa9" "b", "\x7f", "\x7f", "\x7f", "c\n");
    PRT("multibyte, off", KL_ECHOPRT, 0, 0, "a\xc3\xa9" "b", "\x7f", "\x7f", "\x7f", "\x7f", "c\n");
    PRT("no echo", KL_ECHOPRT, KL_ECHO, 0, "ab", "\x7f", "c\n");
    PRT("echoctl key closing", KL_ECHOPRT, 0, 0, "ab", "\x7f", "\x01", "\n");
    PRT("newline while open, then more", KL_ECHOPRT, 0, 0, "ab", "\x7f", "\n", "c", "\n");
    PRT("eof while open, then more", KL_ECHOPRT, 0, 0, "ab", "\x7f", "\x04", "c", "\n");
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setL(p.s, KL_ECHOPRT, 0);
        setCc(p.s, KV_EOL, '%');
        setCc(p.s, KV_EOL2, '&');
        STEPS(&p, "VEOL while open", "ab", "\x7f", "%", "c", "\x7f", "&", "d", "\n");
        reads(&p, "VEOL while open");
        ptyClose(&p);
    }
    section("L5 the kill key with nothing to kill");
    PRT("no echok", 0, KL_ECHOK | KL_ECHOKE, 0, "\x15", "\n");
    PRT("echok", 0, KL_ECHOKE, 0, "\x15", "\n");
    PRT("echok echoke", 0, 0, 0, "\x15", "\n");
    PRT("echoprt", KL_ECHOPRT, 0, 0, "\x15", "\n");
    section("L5 the column with OPOST clear");
    {
        const struct {
            const char *name;
            const char *typed;
        } typings[] = {
            {"letters", "ab"},
            {"a control", "\x01"},
            {"a tab", "\t"},
            {"a letter erased", "ab\x7f"},
            {"a tab erased", "a\t\x7f"},
            {"a control erased", "a\x01\x7f"},
            {"a letter, then a line", "ab\n"},
        };
        for (size_t i = 0; i < sizeof(typings) / sizeof(typings[0]); i++) {
            Pty p;
            if (!ptyOpen(&p)) return;
            setO(p.s, KO_TAB3, KO_OPOST);
            keys(&p, typings[i].name, typings[i].typed, strlen(typings[i].typed));
            setO(p.s, KO_OPOST, 0);
            (void)!write(p.s, "\t|", 2);
            unsigned char out[64];
            char text[256];
            const size_t n = drain(p.m, out, sizeof(out));
            say("%s: then a tab: \"%s\"", typings[i].name, esc(text, sizeof(text), out, n));
            ptyClose(&p);
        }
    }
    section("L5 a mode with the column kept");
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setO(p.s, KO_TAB3, 0);
        KEYS(&p, "a key, then raw", "ab");
        setL(p.s, 0, KL_ICANON | KL_ECHO);
        (void)!write(p.s, "\t|", 2);
        unsigned char out[64];
        char text[256];
        const size_t n = drain(p.m, out, sizeof(out));
        say("then a tab: \"%s\"", esc(text, sizeof(text), out, n));
        ptyClose(&p);
    }
    section("L3 L5 VLNEXT and the column");
    PRT("tab after a literal letter", 0, 0, 0, "\x16q\t", "\x7f", "\x7f", "\n");
    PRT("tab after a literal control", 0, 0, 0, "\x16\x01\t", "\x7f", "\x7f", "\n");
    PRT("a literal tab", 0, 0, 0, "\x16\t", "\x7f", "\n");
    PRT("a literal erase erased", 0, 0, 0, "ab\x16\x7f", "\x7f", "\n");
    PRT("erase of a next that's pending", 0, 0, 0, "ab\x16", "\x7f", "\x7f", "\n");
    PRT("newline after next", 0, 0, 0, "ab\x16", "\n", "\n");
    PRT("no echoctl, the key", 0, KL_ECHOCTL, 0, "ab\x16", "c", "\n");
    PRT("literal at werase", 0, 0, 0, "x y\x16\x01", "\x17", "\n");
    PRT("literal space at werase", 0, 0, 0, "x y\x16 ", "\x17", "\n");
    section("L5 the line's first column");
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        (void)!write(p.s, "xyz", 3);
        char swallowed[64];
        (void)drain(p.m, swallowed, sizeof(swallowed));
        STEPS(&p, "reprint with output before", "ab", "\x12", "\t", "\x7f", "\x7f\x7f", "\n");
        reads(&p, "reprint with output before");
        ptyClose(&p);
    }
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        STEPS(&p, "kill and output", "ab", "\x15");
        (void)!write(p.s, "xyz", 3);
        char swallowed[64];
        (void)drain(p.m, swallowed, sizeof(swallowed));
        STEPS(&p, "kill and output", "c\t", "\x7f", "\x7f", "\n");
        reads(&p, "kill and output");
        ptyClose(&p);
    }
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        STEPS(&p, "erase and output", "ab", "\x7f\x7f");
        (void)!write(p.s, "xyz", 3);
        char swallowed[64];
        (void)drain(p.m, swallowed, sizeof(swallowed));
        STEPS(&p, "erase and output", "c\t", "\x7f", "\x7f", "\n");
        reads(&p, "erase and output");
        ptyClose(&p);
    }
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setL(p.s, 0, KL_ECHO);
        STEPS(&p, "first key unseen", "ab");
        setL(p.s, KL_ECHO, 0);
        STEPS(&p, "first key unseen", "c\t", "\x7f", "\x7f", "\n");
        reads(&p, "first key unseen");
        ptyClose(&p);
    }
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setI(p.s, 0, KI_ICRNL);
        setL(p.s, 0, KL_ECHOCTL);
        (void)!write(p.s, "xyz", 3);
        char swallowed[64];
        (void)drain(p.m, swallowed, sizeof(swallowed));
        STEPS(&p, "typed return, no echoctl", "\r");
        STEPS(&p, "typed return, no echoctl", "\t", "\x7f", "\x7f", "\n");
        reads(&p, "typed return, no echoctl");
        setO(p.s, KO_ONOCR, 0);
        STEPS(&p, "typed return at column 0, onocr", "\r", "a\r", "\n");
        reads(&p, "typed return at column 0, onocr");
        setO(p.s, KO_OCRNL, KO_ONOCR);
        STEPS(&p, "typed return, ocrnl", "a\r", "\n");
        reads(&p, "typed return, ocrnl");
        ptyClose(&p);
    }
    section("L3 a character set to nothing");
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setCc(p.s, KV_ERASE, 0);
        const unsigned char nul[] = {'a', 0, 'b', '\n'};
        keys(&p, "VERASE 0", nul, sizeof(nul));
        reads(&p, "VERASE 0");
        setCc(p.s, KV_ERASE, 0x7f);
        setCc(p.s, KV_KILL, 0);
        keys(&p, "VKILL 0", nul, sizeof(nul));
        reads(&p, "VKILL 0");
        setCc(p.s, KV_EOF, 0);
        keys(&p, "VEOF 0", nul, sizeof(nul));
        reads(&p, "VEOF 0");
        setCc(p.s, KV_EOL2, 0);
        setCc(p.s, KV_WERASE, 0);
        setCc(p.s, KV_REPRINT, 0);
        setCc(p.s, KV_LNEXT, 0);
        keys(&p, "the rest 0", nul, sizeof(nul));
        reads(&p, "the rest 0");
        ptyClose(&p);
    }
    section("L1 cflags on a pty");
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        const uint32_t values[] = {0, 0xbf, 0x1f, 0x10, 0x20, 0x30, 0x40, 0x100, 0x200, 0x400, 0x800, 0x1000, 0x1001, 0x7fffffff, 0xd, 0x8d};
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            KTermios t;
            kget(p.s, &t);
            t.c = values[i];
            kset(p.s, &t);
            kget(p.s, &t);
            say("set c=%x reads %x", values[i], t.c);
        }
        ptyClose(&p);
    }
    section("L7 flags Linux keeps and doesn't act on");
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        KTermios t;
        kget(p.s, &t);
        t.i = 0xffffffffu & ~(KI_IXON | KI_IUTF8 | KI_INLCR | KI_IGNCR | KI_ICRNL | KI_ISTRIP | KI_IUCLC | KI_PARMRK);
        t.o = 0xffffffffu & ~(KO_OPOST);
        t.l = 0xffffffffu & ~(KL_ISIG);
        kset(p.s, &t);
        kget(p.s, &t);
        say("all kept: i=%x o=%x l=%x", t.i, t.o, t.l);
        ptyClose(&p);
    }
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setL(p.s, 0x4u /* XCASE */, 0);
        setI(p.s, KI_IUCLC, 0);
        setO(p.s, KO_OLCUC, 0);
        KEYS(&p, "xcase", "aB\\C\n");
        reads(&p, "xcase");
        (void)!write(p.s, "aB\\C", 4);
        unsigned char out[32];
        char text[64];
        const size_t n = drain(p.m, out, sizeof(out));
        say("xcase output \"%s\"", esc(text, sizeof(text), out, n));
        ptyClose(&p);
    }
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setL(p.s, 0x1000u /* FLUSHO */ | 0x4000u /* PENDIN */, 0);
        KEYS(&p, "flusho pendin", "aB\n");
        reads(&p, "flusho pendin");
        (void)!write(p.s, "out", 3);
        unsigned char out[32];
        char text[64];
        const size_t n = drain(p.m, out, sizeof(out));
        say("flusho output \"%s\"", esc(text, sizeof(text), out, n));
        KTermios t;
        kget(p.s, &t);
        say("l=%x", t.l);
        ptyClose(&p);
    }
}

/* Every byte, for what a byte's class decides. */
static bool scanSkip(unsigned b) {
    return b == 0x7f || b == '\n' || b == '\r' || b == 0x15;
}

static void scanModes(Pty *p, uint32_t i_on, bool echoctl) {
    KTermios t;
    kget(p->s, &t);
    t.i = i_on;
    t.l = KL_ICANON | KL_ECHO | KL_ECHOE | KL_IEXTEN | (echoctl ? KL_ECHOCTL : 0);
    t.o = KO_OPOST;
    for (int i = 0; i < 19; i++) {
        t.cc[i] = 0;
    }
    t.cc[KV_ERASE] = 0x7f;
    t.cc[KV_KILL] = 0x15;
    t.cc[KV_WERASE] = 0x17;
    t.cc[KV_MIN] = 1;
    kset(p->s, &t);
}

static void secByteScans(void) {
    section("L5 every byte: what erasing it echoes");
    for (int echoctl = 1; echoctl >= 0; echoctl--) {
        for (int utf8 = 0; utf8 < 2; utf8++) {
            Pty p;
            if (!ptyOpen(&p)) return;
            scanModes(&p, utf8 ? KI_IUTF8 : 0, echoctl != 0);
            char line[65536];
            size_t at = 0;
            for (unsigned b = 0; b < 256; b++) {
                if (scanSkip(b) || b == 0x17) continue;
                const unsigned char typed[3] = {'q', (unsigned char)b, 0x7f};
                const unsigned char kill[1] = {0x15};
                if (write(p.m, typed, 3) != 3) break;
                unsigned char echoed[64];
                const size_t n = drain(p.m, echoed, sizeof(echoed));
                char text[256];
                at += (size_t)snprintf(line + at, sizeof(line) - at, "%02x:%s ", b, esc(text, sizeof(text), echoed, n));
                /* Erase what's left: the kill isn't what's looked at here. */
                (void)!write(p.m, kill, 1);
                (void)drain(p.m, echoed, sizeof(echoed));
            }
            say("echoctl %d iutf8 %d: %s", echoctl, utf8, line);
            ptyClose(&p);
        }
    }
    section("L5 every byte: how far it moves a tab's erase");
    for (int echoctl = 1; echoctl >= 0; echoctl--) {
        for (int utf8 = 0; utf8 < 2; utf8++) {
            Pty p;
            if (!ptyOpen(&p)) return;
            scanModes(&p, utf8 ? KI_IUTF8 : 0, echoctl != 0);
            char line[8192];
            size_t at = 0;
            for (unsigned b = 0; b < 256; b++) {
                if (scanSkip(b) || b == 0x17 || b == '\t') continue;
                const unsigned char typed[3] = {(unsigned char)b, '\t', 0x7f};
                const unsigned char kill[1] = {0x15};
                if (write(p.m, typed, 3) != 3) break;
                unsigned char echoed[64];
                const size_t n = drain(p.m, echoed, sizeof(echoed));
                size_t backs = 0;
                for (size_t i = 0; i < n; i++) {
                    if (echoed[i] == '\b') backs++;
                }
                at += (size_t)snprintf(line + at, sizeof(line) - at, "%02x:%zu ", b, backs);
                (void)!write(p.m, kill, 1);
                (void)drain(p.m, echoed, sizeof(echoed));
                /* Back to column 0 whatever the erasing left. */
                (void)!write(p.s, "\r", 1);
                (void)drain(p.m, echoed, sizeof(echoed));
            }
            say("echoctl %d iutf8 %d: %s", echoctl, utf8, line);
            ptyClose(&p);
        }
    }
    section("L3 every byte: is it part of a word");
    for (int utf8 = 0; utf8 < 2; utf8++) {
        Pty p;
        if (!ptyOpen(&p)) return;
        scanModes(&p, utf8 ? KI_IUTF8 : 0, true);
        char line[8192];
        size_t at = 0;
        for (unsigned b = 0; b < 256; b++) {
            if (scanSkip(b) || b == 0x17) continue;
            /* The byte between two letters: gone with the last, if it is a word's. */
            const unsigned char typed[5] = {'a', (unsigned char)b, 'z', 0x17, '\n'};
            if (write(p.m, typed, 5) != 5) break;
            unsigned char sink[128];
            (void)drain(p.m, sink, sizeof(sink));
            unsigned char got[64];
            const ssize_t n = read(p.s, got, sizeof(got));
            at += (size_t)snprintf(line + at, sizeof(line) - at, "%02x:%zd ", b, n);
        }
        say("iutf8 %d: %s", utf8, line);
        ptyClose(&p);
    }
    section("L7 every byte: OLCUC, IUCLC");
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setO(p.s, KO_OLCUC, KO_ONLCR);
        unsigned char all[256];
        for (unsigned b = 0; b < 256; b++) {
            all[b] = (unsigned char)b;
        }
        char line[8192];
        size_t at = 0;
        if (write(p.s, all, 256) != 256) say("write: %s", errName(errno));
        unsigned char out[1024];
        const size_t n = drain(p.m, out, sizeof(out));
        for (size_t i = 0; i < n; i++) {
            at += (size_t)snprintf(line + at, sizeof(line) - at, "%02x", out[i]);
        }
        say("olcuc %zu: %s", n, line);
        ptyClose(&p);
    }
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setRaw(p.s);
        setI(p.s, KI_IUCLC, KI_IXON);
        unsigned char all[256];
        for (unsigned b = 0; b < 256; b++) {
            all[b] = (unsigned char)b;
        }
        char line[8192];
        size_t at = 0;
        if (write(p.m, all, 256) != 256) say("write: %s", errName(errno));
        unsigned char out[1024];
        sleepMs(30);
        const size_t n = drain(p.s, out, sizeof(out));
        for (size_t i = 0; i < n; i++) {
            at += (size_t)snprintf(line + at, sizeof(line) - at, "%02x", out[i]);
        }
        say("iuclc %zu: %s", n, line);
        ptyClose(&p);
    }
    section("L7 every byte: does it move the output column");
    for (int utf8 = 0; utf8 < 2; utf8++) {
        Pty p;
        if (!ptyOpen(&p)) return;
        setO(p.s, KO_TAB3, KO_ONLCR);
        setI(p.s, utf8 ? KI_IUTF8 : 0, 0);
        char line[8192];
        size_t at = 0;
        for (unsigned b = 0; b < 256; b++) {
            if (b == '\t' || b == '\r' || b == '\n') continue;
            /* From column 1, so that a backspace shows. */
            const unsigned char data[5] = {'x', (unsigned char)b, '\t', '|', '\r'};
            if (write(p.s, data, 5) != 5) break;
            unsigned char out[64];
            const size_t n = drain(p.m, out, sizeof(out));
            size_t spaces = 0;
            for (size_t i = 0; i < n; i++) {
                if (out[i] == ' ' && !(b == ' ' && i == 1)) spaces++;
            }
            at += (size_t)snprintf(line + at, sizeof(line) - at, "%02x:%zu ", b, spaces);
        }
        say("iutf8 %d: %s", utf8, line);
        ptyClose(&p);
    }
}

/* --- a change of mode, with something queued --- */

static void secModeChanges(void) {
    section("L11 changes of mode with something queued");
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setL(p.s, 0, KL_ECHO);
        setI(p.s, 0, KI_ICRNL);
        KEYS(&p, "to raw", "one\ntwo");
        showInq("line mode", p.s);
        setL(p.s, 0, KL_ICANON);
        showInq("raw", p.s);
        showPoll("raw", p.s);
        reads(&p, "raw");
        ptyClose(&p);
    }
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setL(p.s, 0, KL_ECHO);
        setI(p.s, 0, KI_ICRNL);
        KEYS(&p, "there and back", "one\ntwo\nthree");
        setL(p.s, 0, KL_ICANON);
        setL(p.s, KL_ICANON, 0);
        showInq("back", p.s);
        showPoll("back", p.s);
        reads(&p, "back");
        ptyClose(&p);
    }
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setL(p.s, 0, KL_ECHO);
        setI(p.s, 0, KI_ICRNL);
        KEYS(&p, "there and back, then more", "one\ntw");
        setL(p.s, 0, KL_ICANON);
        setL(p.s, KL_ICANON, 0);
        KEYS(&p, "there and back, then more", "o\n");
        showInq("back", p.s);
        reads(&p, "back");
        ptyClose(&p);
    }
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setL(p.s, 0, KL_ECHO);
        setI(p.s, 0, KI_ICRNL);
        setL(p.s, 0, KL_ICANON);
        KEYS(&p, "raw text, to line mode", "a\nb");
        showPoll("raw", p.s);
        setL(p.s, KL_ICANON, 0);
        showPoll("line mode", p.s);
        showInq("line mode", p.s);
        KEYS(&p, "raw text, to line mode", "\x7f");
        KEYS(&p, "raw text, to line mode", "c");
        showInq("line mode, c", p.s);
        reads(&p, "line mode");
        KEYS(&p, "raw text, to line mode", "\n");
        reads(&p, "line mode");
        ptyClose(&p);
    }
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setL(p.s, 0, KL_ECHO);
        setI(p.s, 0, KI_ICRNL);
        KEYS(&p, "the key, to raw", "ab\x04" "cd");
        showInq("line mode", p.s);
        setL(p.s, 0, KL_ICANON);
        showInq("raw", p.s);
        reads(&p, "raw");
        ptyClose(&p);
    }
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setL(p.s, 0, KL_ECHO);
        setI(p.s, 0, KI_ICRNL);
        KEYS(&p, "the key alone, to raw", "\x04");
        setL(p.s, 0, KL_ICANON);
        showInq("raw", p.s);
        showPoll("raw", p.s);
        reads(&p, "raw");
        ptyClose(&p);
    }
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setL(p.s, 0, KL_ECHO);
        setI(p.s, 0, KI_ICRNL);
        KEYS(&p, "the key, to raw, byte by byte", "ab\x04" "cd");
        setL(p.s, 0, KL_ICANON);
        for (int i = 0; i < 6; i++) {
            unsigned char c;
            const ssize_t n = read(p.s, &c, 1);
            char text[16];
            char result[16];
            say("read %s \"%s\"", resultName(result, sizeof(result), n), n > 0 ? esc(text, sizeof(text), &c, 1) : "");
        }
        ptyClose(&p);
    }
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setL(p.s, 0, KL_ECHO);
        setI(p.s, 0, KI_ICRNL);
        KEYS(&p, "the key, there and back", "ab\x04" "cd");
        setL(p.s, 0, KL_ICANON);
        setL(p.s, KL_ICANON, 0);
        showInq("back", p.s);
        reads(&p, "back");
        ptyClose(&p);
    }
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setL(p.s, 0, KL_ECHO);
        setI(p.s, 0, KI_ICRNL);
        KEYS(&p, "the key alone, there and back", "\x04");
        setL(p.s, 0, KL_ICANON);
        setL(p.s, KL_ICANON, 0);
        showInq("back", p.s);
        reads(&p, "back");
        ptyClose(&p);
    }
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        setL(p.s, 0, KL_ECHO);
        setI(p.s, 0, KI_ICRNL);
        KEYS(&p, "line, the key, to raw, short read", "one\n\x04" "x");
        setL(p.s, 0, KL_ICANON);
        unsigned char two[2];
        const ssize_t n = read(p.s, two, 2);
        say("read %zd", n);
        reads(&p, "rest");
        ptyClose(&p);
    }
    {
        /* Without echo the first letter's column is not in question. */
        Pty p;
        if (!ptyOpen(&p)) return;
        setL(p.s, 0, KL_ICANON | KL_ECHO);
        setI(p.s, 0, KI_ICRNL);
        KEYS(&p, "raw, empty, to line mode", "");
        setL(p.s, KL_ICANON | KL_ECHO, 0);
        showPoll("line mode", p.s);
        reads(&p, "line mode");
        ptyClose(&p);
    }
    section("L13 packet mode, again");
    {
        Pty p;
        if (!ptyOpen(&p)) return;
        int on = 1;
        (void)ioctl(p.m, K_TIOCPKT, &on);
        KTermios t;
        kget(p.s, &t);
        t.i &= ~KI_IXON;
        kset(p.s, &t);
        t.i |= KI_IXON;
        kset(p.s, &t);
        showPacket("off and on, unread", &p, 16);
        showPacket("off and on, unread", &p, 16);
        sayResult("slave TCIFLUSH", ioctl(p.s, K_TCFLSH, 0L));
        t.i &= ~KI_IXON;
        kset(p.s, &t);
        showPacket("a flush and off", &p, 16);
        sayResult("TCSETSF through the master", ioctl(p.m, K_TCSETSF, &t));
        showPacket("TCSETSF through the master", &p, 16);
        sayResult("TCFLSH bad", ioctl(p.s, K_TCFLSH, 9L));
        showPacket("TCFLSH bad", &p, 16);
        /* Reading the status with no room for more than the status. */
        sayResult("slave TCIFLUSH", ioctl(p.s, K_TCFLSH, 0L));
        showPacket("asks 1", &p, 1);
        showPacket("asks 1", &p, 1);
        /* Reading nothing. */
        sayResult("slave TCIFLUSH", ioctl(p.s, K_TCFLSH, 0L));
        showPacket("asks 0", &p, 0);
        showPacket("asks 16", &p, 16);
        /* FIONREAD with a status waiting. */
        sayResult("slave TCIFLUSH", ioctl(p.s, K_TCFLSH, 0L));
        showInq("status waiting", p.m);
        showPacket("drain", &p, 16);
        (void)!write(p.s, "abc", 3);
        sleepMs(20);
        showInq("data waiting", p.m);
        showPacket("drain", &p, 16);
        ptyClose(&p);
    }
}

static void caseSignalKeyPacket(Pty *p) {
    int on = 1;
    sayResult("TIOCPKT", ioctl(p->m, K_TIOCPKT, &on));
    installHandler(SIGQUIT, true);
    SIGNAL_KEYS(p, "the key", "\x1c");
    (void)poll(NULL, 0, 20);
    say("signals %d", hitsOf(SIGQUIT));
    (void)!write(p->s, "out", 3);
    sleepMs(20);
    SIGNAL_KEYS(p, "the key with output queued", "\x1c");
    (void)poll(NULL, 0, 20);
    say("signals %d", hitsOf(SIGQUIT));
    KTermios t;
    kget(p->s, &t);
    t.l |= KL_NOFLSH;
    kset(p->s, &t);
    SIGNAL_KEYS(p, "the key with noflsh", "\x1c");
    (void)poll(NULL, 0, 20);
    say("signals %d", hitsOf(SIGQUIT));
}

/* --- as root: taking a terminal from another session --- */

/* Copies only the name: on Linux the two are separate processes. */
typedef struct {
    char path[32];
} SharedPath;

static int robberTakes(void *raw) {
    SharedPath *shared = (SharedPath *)raw;
    setsid();
    awaitMark(10);
    const int s = open(shared->path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    int value = -1;
    sayResult("robber: TIOCSCTTY", ioctl(s, K_TIOCSCTTY, 0L));
    sayResult("robber: TIOCSCTTY force", ioctl(s, K_TIOCSCTTY, 1L));
    sayResult("robber: TIOCGSID", ioctl(s, K_TIOCGSID, &value));
    relation("robber: the session", value, getsid(0));
    sayResult("robber: TIOCGPGRP", ioctl(s, K_TIOCGPGRP, &value));
    relation("robber: the group", value, getpgrp());
    const int tty = open("/dev/tty", O_RDWR);
    sayResult("robber: open /dev/tty", tty >= 0 ? 0 : -1);
    if (tty >= 0) {
        close(tty);
    }
    mark(11);
    awaitMark(12);
    mark(13);
    close(s);
    return 0;
}

static int victimWithName(void *raw) {
    SharedPath *shared = (SharedPath *)raw;
    Pty p;
    memset(&p, 0, sizeof(p));
    setsid();
    p.m = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (p.m < 0 || !ptyNames(&p)) return 1;
    strcpy(shared->path, p.path);
    const int s = open(p.path, O_RDWR | O_NONBLOCK);
    int value = -1;
    sayResult("victim: TIOCGSID", ioctl(s, K_TIOCGSID, &value));
    relation("victim: the session", value, getsid(0));
    mark(10);
    awaitMark(11);
    value = -1;
    sayResult("victim, robbed: TIOCGSID", ioctl(s, K_TIOCGSID, &value));
    sayResult("victim, robbed: TIOCGPGRP", ioctl(s, K_TIOCGPGRP, &value));
    const int tty = open("/dev/tty", O_RDWR);
    sayResult("victim, robbed: open /dev/tty", tty >= 0 ? 0 : -1);
    if (tty >= 0) {
        close(tty);
    }
    mark(12);
    awaitMark(13);
    close(s);
    close(p.m);
    return 0;
}

static void secRoot(void) {
    section("L19 TIOCSCTTY with force");
    clearMarks();
    SharedPath *shared = (SharedPath *)mmap(NULL, sizeof(SharedPath), PROT_READ | PROT_WRITE,
                                            MAP_SHARED | MAP_ANON, -1, 0);
    const pid_t victim = procSpawn(victimWithName, shared);
    const pid_t robber = procSpawn(robberTakes, shared);
    procFinish("victim", victim);
    procFinish("robber", robber);
    munmap(shared, sizeof(SharedPath));
}

int main(int argc, char **argv) {
    gShared = (Shared *)mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    platformInit();
    if (argc > 1 && strcmp(argv[1], "root") == 0) {
        secRoot();
        return 0;
    }
    secDefaults();
    secHostNumbers();
    secWordErase();
    secEchoMatrix();
    secEchoFlags();
    secEchoPrt();
    secTabErase();
    secByteScans();
    secUtf8();
    secLongLine();
    secInputFlags();
    secOutputFlags();
    secVminVtime();
    secFionread();
    secPacket();
    secModeChanges();
    secErrors();
    secExclusive();
    secQueuesAndBreaks();
    section("sessions");
    sessionCase("L15 background read, SIGTTIN by default", caseBgReadDefault);
    sessionCase("L15 background read, ignored", caseBgReadIgnored);
    sessionCase("L15 background read, blocked", caseBgReadBlocked);
    sessionCase("L15 background read, a handler", caseBgReadHandler);
    sessionCase("L15 background read, a handler with SA_RESTART", caseBgReadRestart);
    sessionCase("L15 background read, non-blocking", caseBgReadNonblocking);
    sessionCase("L15 background read of nothing", caseBgReadZero);
    sessionCase("L15 orphaned group, read", caseOrphanRead);
    sessionCase("L15 orphaned group, read, ignored", caseOrphanReadIgnored);
    sessionCase("L15 orphaned group, read, blocked", caseOrphanReadBlocked);
    sessionCase("L16 background write, no TOSTOP", caseBgWriteOff);
    sessionCase("L16 background write, TOSTOP", caseBgWriteDefault);
    sessionCase("L16 background write, ignored", caseBgWriteIgnored);
    sessionCase("L16 background write, blocked", caseBgWriteBlocked);
    sessionCase("L16 background write, a handler", caseBgWriteHandler);
    sessionCase("L16 background write, a handler with SA_RESTART", caseBgWriteRestart);
    sessionCase("L16 background write of nothing", caseBgWriteZero);
    sessionCase("L16 orphaned group, write", caseOrphanWrite);
    sessionCase("L16 orphaned group, write, ignored", caseOrphanWriteIgnored);
    sessionCase("L16 orphaned group, write, blocked", caseOrphanWriteBlocked);
    sessionCase("L15 L16 another terminal", caseBgOtherTerminal);
    sessionCase("L15 L16 in the foreground", caseForeground);
    sessionCase("L18 foreground groups", caseForegroundGroups);
    sessionCase("L18 TIOCNOTTY, not the leader", caseNoTtyMember);
    sessionCase("L18 TIOCNOTTY, the leader", caseNoTtyLeader);
    sessionCase("L18 TIOCEXCL and /dev/tty", caseExclusiveControlling);
    sessionCase("L18 TIOCSTI on the controlling terminal", caseInjectControlling);
    {
        say("-- L19 a master never controls");
        clearMarks();
        const pid_t pid = procSpawn(masterNeverControls, NULL);
        procFinish("leader", pid);
        say("-- L19 claiming");
        const pid_t pid2 = procSpawn(claimCases, NULL);
        procFinish("leader", pid2);
    }
    sessionCase("L19 claims from outside", caseClaimsFromOutside);
    sessionCase("L25 L26 window size", caseWinsize);
    sessionCase("L25 who gets it", caseWinsizeTarget);
    sessionCase("the interrupt key", caseInterruptKey);
    sessionCase("keys that raise a signal", caseSignalKey);
    sessionCase("L13 keys that raise a signal, in packet mode", caseSignalKeyPacket);
    secWinsizeNoSession();
    return 0;
}
