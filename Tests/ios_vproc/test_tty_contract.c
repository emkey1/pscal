// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Drives the terminal layer directly, through ops and tty_input, with fake process
// hooks and no vproc. Build without PSCAL_TARGET_IOS, so vproc.h gives plain host stubs.
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/path_truncate.h"
#include "runtime/vproc/tty/pscal_pty.h"
#include "runtime/vproc/tty/pscal_tty_host.h"

/* --- fake process --- */
static int gSid = -1;
static int gPgid = -1;
static bool gLeader = false;
static int gRecordedFg = -1;
static int gLastSetFgSid = 0, gLastSetFg = 0;
static struct {
    int pgid, sig;
} gSent[16];
static int gSentCount = 0;
static int gSendResult = 0;
static bool gKernel = false;
static bool gRefused = false;
static bool gOrphaned = false;
static bool gGaveUp = false;
static int gGroupCheck = 0;
static int gAfterSignal = _EINTR;
static int gAfterSignals = 0;
static int gGroupAfterSignal = -1;

bool pscalTtyCallerIsKernel(void) { return gKernel; }
bool pscalTtySignalIgnoredOrBlocked(int sig) {
    (void)sig;
    return gRefused;
}
bool pscalTtyCurrentGroupOrphaned(void) { return gOrphaned; }
int pscalTtyCheckGroup(int sid, int pgid) {
    (void)sid;
    (void)pgid;
    return gGroupCheck;
}
void pscalTtyCallerGiveUpTerminal(void) { gGaveUp = true; }
bool pscalTtyCallerGaveUpTerminal(void) { return gGaveUp; }
int pscalTtyCurrentPid(void) { return gSid; }
int pscalTtyCurrentPgid(void) { return gPgid; }
int pscalTtyCurrentSid(void) { return gSid; }
bool pscalTtyIsSessionLeader(void) { return gLeader; }
void pscalTtySetForegroundPgid(int sid, int fg) {
    gLastSetFgSid = sid;
    gLastSetFg = fg;
}
int pscalTtyGetForegroundPgid(int sid) {
    (void)sid;
    return gRecordedFg;
}
int pscalTtySendGroupSignal(int pgid, int sig) {
    if (gSentCount < 16) {
        gSent[gSentCount].pgid = pgid;
        gSent[gSentCount].sig = sig;
        gSentCount++;
    }
    return gSendResult;
}
/* What a stop and a SIGCONT come to: by the time it is over, the caller may have been moved. */
int pscalTtyAfterJobSignal(int sig) {
    (void)sig;
    gAfterSignals++;
    if (gGroupAfterSignal > 0) {
        gPgid = gGroupAfterSignal;
    }
    return gAfterSignal;
}
static void fakeReset(void) {
    gSid = -1;
    gPgid = -1;
    gLeader = false;
    gRecordedFg = -1;
    gSentCount = 0;
    gSendResult = 0;
    gLastSetFgSid = gLastSetFg = 0;
    gKernel = gRefused = gOrphaned = gGaveUp = false;
    gGroupCheck = 0;
    gAfterSignal = _EINTR;
    gAfterSignals = 0;
    gGroupAfterSignal = -1;
}

/* --- the two the layer takes from elsewhere --- */
bool pathTruncateEnabled(void) { return false; }
bool pathTruncateExpand(const char *in, char *out, size_t size) {
    snprintf(out, size, "%s", in);
    return true;
}
int pscalHostOpenRaw(const char *path, int flags, mode_t mode) {
    (void)path;
    (void)flags;
    (void)mode;
    return -1;
}

/* --- helpers --- */
typedef struct {
    struct pscal_fd *m, *s;
    int num;
} Pair;

static Pair pairOpen(void) {
    Pair p = {NULL, NULL, -1};
    assert(pscalPtyOpenMaster(O_RDWR | O_NOCTTY, &p.m, &p.num) == 0);
    assert(pscalPtyUnlock(p.m) == 0);
    assert(pscalPtyOpenSlave(p.num, O_RDWR | O_NOCTTY, &p.s) == 0);
    return p;
}
static void pairClose(Pair *p) {
    if (p->s) pscal_fd_close(p->s);
    if (p->m) pscal_fd_close(p->m);
    p->s = p->m = NULL;
}
static void setL(struct pscal_fd *s, dword_t on, dword_t off) {
    struct termios_ t;
    assert(s->ops->ioctl(s, TCGETS_, &t) == 0);
    t.lflags = (t.lflags | on) & ~off;
    assert(s->ops->ioctl(s, TCSETS_, &t) == 0);
}
static void setI(struct pscal_fd *s, dword_t on, dword_t off) {
    struct termios_ t;
    assert(s->ops->ioctl(s, TCGETS_, &t) == 0);
    t.iflags = (t.iflags | on) & ~off;
    assert(s->ops->ioctl(s, TCSETS_, &t) == 0);
}
static void setO(struct pscal_fd *s, dword_t on, dword_t off) {
    struct termios_ t;
    assert(s->ops->ioctl(s, TCGETS_, &t) == 0);
    t.oflags = (t.oflags | on) & ~off;
    assert(s->ops->ioctl(s, TCSETS_, &t) == 0);
}
static void setCc(struct pscal_fd *s, int idx, byte_t v) {
    struct termios_ t;
    assert(s->ops->ioctl(s, TCGETS_, &t) == 0);
    t.cc[idx] = v;
    assert(s->ops->ioctl(s, TCSETS_, &t) == 0);
}
static void nb(struct pscal_fd *f, bool on) {
    f->flags = on ? (f->flags | O_NONBLOCK) : (f->flags & ~(unsigned)O_NONBLOCK);
}
static void type(Pair *p, const char *keys) {
    size_t n = strlen(keys);
    assert(tty_input(p->s->tty, keys, n, false) == (ssize_t)n);
}
/* What is waiting on the master, drained. */
static size_t masterDrain(Pair *p, char *out, size_t size) {
    nb(p->m, true);
    size_t len = 0;
    for (;;) {
        ssize_t r = p->m->ops->read(p->m, out + len, size - 1 - len);
        if (r <= 0) break;
        len += (size_t)r;
    }
    nb(p->m, false);
    out[len] = 0;
    return len;
}
static void expectRead(struct pscal_fd *f, const char *want) {
    char got[256];
    size_t n = strlen(want);
    nb(f, true);
    ssize_t r = f->ops->read(f, got, sizeof(got));
    nb(f, false);
    if (r != (ssize_t)n || memcmp(got, want, n) != 0) {
        fprintf(stderr, "read wanted [%s] got %zd [%.*s]\n", want, r, r > 0 ? (int)r : 0, got);
        assert(!"read");
    }
}
static void expectAgain(struct pscal_fd *f) {
    char c;
    nb(f, true);
    assert(f->ops->read(f, &c, 1) == _EAGAIN);
    nb(f, false);
}
static void msleep(int ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

/* --- cases --- */

static void test_abi(void) {
    assert(sizeof(struct termios_) == 36);
    assert(sizeof(struct winsize_) == 8);
    assert(pscalCompatErrno(5) == 0);
    assert(pscalCompatErrno(_EAGAIN) == EAGAIN);
    assert(pscalCompatErrno(_EBADF) == EBADF);
    assert(pscalCompatErrno(_ETIMEDOUT) == ETIMEDOUT);
    assert(pscalCompatErrno(_ENOENT) == ENOENT);
    assert(pscalCompatErrno(_EMFILE) == EMFILE);
    assert(pscalCompatErrno(_ESRCH) == ESRCH);
    assert(pscalCompatErrno(_EEXIST) == EEXIST);
    assert(pscalCompatErrno(_EFAULT) == EFAULT);
    assert(pscalCompatErrno(_EBUSY) == EBUSY);
    assert(pscalCompatErrno(-999) == EIO);
    assert(POLL_READ == 1 && POLL_PRI == 2 && POLL_WRITE == 4 && POLL_ERR == 8 && POLL_HUP == 16 &&
           POLL_NVAL == 32);
    assert(pscal_tty_fd_ops.read && pscal_tty_fd_ops.write && pscal_tty_fd_ops.poll &&
           pscal_tty_fd_ops.ioctl && pscal_tty_fd_ops.ioctl_size && pscal_tty_fd_ops.close);
    assert(pscal_tty_fd_ops.ioctl_size(TCGETS_) == 36);
    assert(pscal_tty_fd_ops.ioctl_size(TIOCGWINSZ_) == 8);
    assert(pscal_tty_fd_ops.ioctl_size((int)TIOCGPTN_) == 4);
    assert(pscal_tty_fd_ops.ioctl_size(TCFLSH_) == 0);
    assert(pscal_tty_fd_ops.ioctl_size(0x1234) == -1);
}

static void test_defaults(void) {
    Pair p = pairOpen();
    struct termios_ t;
    assert(p.s->ops->ioctl(p.s, TCGETS_, &t) == 0);
    assert(t.iflags == (ICRNL_ | IXON_));
    assert(t.oflags == (OPOST_ | ONLCR_));
    assert(t.cflags == (B38400_ | CS8_ | CREAD_) && t.line == 0);
    assert(t.lflags == 0x8a3b && t.iflags == 0x500 && t.oflags == 0x5);
    assert(t.lflags == (ISIG_ | ICANON_ | ECHO_ | ECHOE_ | ECHOK_ | ECHOCTL_ | ECHOKE_ | IEXTEN_));
    assert(t.cc[VINTR_] == 3 && t.cc[VQUIT_] == 0x1c && t.cc[VERASE_] == 0x7f && t.cc[VKILL_] == 0x15);
    assert(t.cc[VEOF_] == 4 && t.cc[VTIME_] == 0 && t.cc[VMIN_] == 1 && t.cc[VSWTC_] == 0);
    assert(t.cc[VSTART_] == 0x11 && t.cc[VSTOP_] == 0x13 && t.cc[VSUSP_] == 0x1a && t.cc[VEOL_] == 0);
    assert(t.cc[VREPRINT_] == 0x12 && t.cc[VDISCARD_] == 0x0f && t.cc[VWERASE_] == 0x17);
    assert(t.cc[VLNEXT_] == 0x16 && t.cc[VEOL2_] == 0 && t.cc[17] == 0 && t.cc[18] == 0);
    /* The master's own record is flat, though a request on it reaches the slave. */
    assert(p.m->tty->termios.iflags == 0 && p.m->tty->termios.oflags == 0 && p.m->tty->termios.lflags == 0);
    assert(p.m->ops->ioctl(p.m, TCGETS_, &t) == 0 && t.lflags != 0);
    assert(p.s->tty->driver == &pty_slave && p.m->tty->driver == &pty_master);
    assert(p.s->tty->type == TTY_PSEUDO_SLAVE_MAJOR && p.m->tty->type == TTY_PSEUDO_MASTER_MAJOR);
    assert(p.s->tty->num == p.num && p.m->tty->pty.other == p.s->tty);
    assert(pscalPtyIsMaster(p.m) && !pscalPtyIsMaster(p.s) && pscalPtyIsSlave(p.s) && !pscalPtyIsSlave(p.m));
    assert(!pscalPtyIsMaster(NULL) && !pscalPtyIsSlave(NULL));
    pairClose(&p);
}

static void test_line_editing_and_echo(void) {
    Pair p = pairOpen();
    char echo[512];
    /* Return, echo, translation. */
    type(&p, "ab");
    expectAgain(p.s);
    type(&p, "c\r");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "abc\r\n") == 0);
    expectRead(p.s, "abc\n");
    /* Erase: rub out once, and twice for a byte shown as control. */
    type(&p, "ax\x7f");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "ax\b \b") == 0);
    type(&p, "\x01");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "^A") == 0);
    type(&p, "\x7f");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "\b \b\b \b") == 0);
    type(&p, "\n");
    masterDrain(&p, echo, sizeof(echo));
    expectRead(p.s, "a\n");
    /* Erase on nothing, and on a finished line. */
    type(&p, "\x7f");
    masterDrain(&p, echo, sizeof(echo));
    assert(echo[0] == 0);
    type(&p, "z\n\x7f");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "z\r\n") == 0);
    expectRead(p.s, "z\n");
    /* Kill. */
    type(&p, "one\ntw\x15");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "one\r\ntw\b \b\b \b") == 0);
    type(&p, "k\n");
    masterDrain(&p, echo, sizeof(echo));
    expectRead(p.s, "one\n");
    expectRead(p.s, "k\n");
    /* ECHOE and ECHOK off: the removal happens, and the keys show as themselves. */
    setL(p.s, 0, ECHOE_ | ECHOK_);
    type(&p, "ab\x7f\x15");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "ab^?^U") == 0);
    setL(p.s, ECHOE_ | ECHOK_, 0);
    /* ECHOCTL off: as it is. Tab, and VSTART, never show as control. */
    setL(p.s, 0, ECHOCTL_);
    type(&p, "\x01");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "\x01") == 0);
    setL(p.s, ECHOCTL_, 0);
    type(&p, "\x15");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "\b \b\b \b") == 0);
    type(&p, "\t\x11");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "\t\x11") == 0);
    type(&p, "\x15");
    masterDrain(&p, echo, sizeof(echo));
    /* VEOL */
    setCc(p.s, VEOL_, '%');
    type(&p, "pc%");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "pc%") == 0);
    expectRead(p.s, "pc%");
    setCc(p.s, VEOL_, 0);
    /* A 0 is data, though VEOL is 0. */
    assert(tty_input(p.s->tty, "\0\n", 2, false) == 2);
    masterDrain(&p, echo, sizeof(echo));
    char got[4];
    assert(p.s->ops->read(p.s, got, sizeof(got)) == 2 && got[0] == 0 && got[1] == '\n');
    /* No echo at all. */
    setL(p.s, 0, ECHO_);
    type(&p, "q\n");
    assert(masterDrain(&p, echo, sizeof(echo)) == 0);
    expectRead(p.s, "q\n");
    setL(p.s, ECHO_, 0);
    /* Input translations. */
    setI(p.s, INLCR_, ICRNL_);
    type(&p, "a\n\n");
    masterDrain(&p, echo, sizeof(echo));
    setI(p.s, 0, INLCR_);
    type(&p, "\n");
    masterDrain(&p, echo, sizeof(echo));
    expectRead(p.s, "a\r\r\n");
    setI(p.s, IGNCR_, 0);
    type(&p, "b\rc\n");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "bc\r\n") == 0);
    expectRead(p.s, "bc\n");
    pairClose(&p);
}

/* The tables came from running this on a Linux machine; tty_diff.c holds that to account in
 * full. These are the ones worth having with no vproc in the way. */
static void test_extended_keys(void) {
    Pair p = pairOpen();
    char echo[512];
    /* VWERASE: what's after the last word, and the word. */
    type(&p, "foo-bar  \x17");
    masterDrain(&p, echo, sizeof(echo));
    type(&p, "\n");
    masterDrain(&p, echo, sizeof(echo));
    expectRead(p.s, "foo-\n");
    /* VREPRINT. */
    type(&p, "abc\x12");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "abc^R\r\nabc") == 0);
    /* VLNEXT, on a key that would have erased. */
    type(&p, "\x16\x7f");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "^\b^?") == 0);
    type(&p, "\x7f\n");
    masterDrain(&p, echo, sizeof(echo));
    expectRead(p.s, "abc\n");
    /* VEOL2. */
    setCc(p.s, VEOL2_, '%');
    type(&p, "pc%");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "pc%") == 0);
    expectRead(p.s, "pc%");
    /* All of that goes with IEXTEN, even VEOL2. */
    setL(p.s, 0, IEXTEN_);
    type(&p, "x%\x17\x12\x16\n");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "x%^W^R^V\r\n") == 0);
    expectRead(p.s, "x%\x17\x12\x16\n");
    setL(p.s, IEXTEN_, 0);
    /* VREPRINT needs echo, or it's data. */
    setL(p.s, 0, ECHO_);
    type(&p, "\x12\n");
    expectRead(p.s, "\x12\n");
    setL(p.s, ECHO_, 0);
    masterDrain(&p, echo, sizeof(echo));
    pairClose(&p);
}

static void test_echo_flags(void) {
    Pair p = pairOpen();
    char echo[512];
    /* ECHONL, with nothing else. */
    setL(p.s, ECHONL_, ECHO_);
    type(&p, "ab\n");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "\r\n") == 0);
    expectRead(p.s, "ab\n");
    setL(p.s, ECHO_, ECHONL_);
    /* Echo is output: NL shows as CR NL only with OPOST and ONLCR. */
    setO(p.s, 0, OPOST_);
    type(&p, "ab\n");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "ab\n") == 0);
    expectRead(p.s, "ab\n");
    setO(p.s, OPOST_ | OLCUC_ | TAB3_, 0);
    type(&p, "ab\t\n");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "AB      \r\n") == 0);
    expectRead(p.s, "ab\t\n");
    setO(p.s, 0, OLCUC_ | TAB3_);
    /* ECHOPRT: what goes shows between a backslash and a slash. */
    setL(p.s, ECHOPRT_, 0);
    type(&p, "abc\x7f\x7f");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "abc\\cb") == 0);
    type(&p, "d");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "/d") == 0);
    type(&p, "\x15");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "\\da/") == 0);
    setL(p.s, 0, ECHOPRT_);
    /* A kill with ECHOK but not ECHOKE: the key and a new line. */
    setL(p.s, 0, ECHOKE_);
    type(&p, "ab\x15");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "ab^U\r\n") == 0);
    setL(p.s, ECHOKE_, 0);
    /* A tab's width comes from where the line began. */
    assert(p.s->ops->write(p.s, "xyz", 3) == 3);
    masterDrain(&p, echo, sizeof(echo));
    type(&p, "\t\x7f");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "\t\b\b\b\b\b") == 0);
    /* IUTF8: a character goes whole, and takes one column. */
    setI(p.s, IUTF8_, 0);
    type(&p, "\xc3\xa9\t\x7f");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "\xc3\xa9\t\b\b\b\b") == 0);
    type(&p, "\x7f");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "\b \b") == 0);
    setI(p.s, 0, IUTF8_);
    type(&p, "\n");
    masterDrain(&p, echo, sizeof(echo));
    expectRead(p.s, "\n");
    pairClose(&p);
}

static void test_translations(void) {
    Pair p = pairOpen();
    char echo[64];
    setL(p.s, 0, ECHO_);
    /* ISTRIP first, so that what was high can then mean something. */
    setI(p.s, ISTRIP_, 0);
    type(&p, "a\xff" "b\x8d");
    expectRead(p.s, "b\n");
    setI(p.s, 0, ISTRIP_);
    /* IUCLC, with IEXTEN and by Latin-1. */
    setI(p.s, IUCLC_, 0);
    type(&p, "aB\xc9\xdf\n");
    expectRead(p.s, "ab\xe9\xdf\n");
    setL(p.s, 0, IEXTEN_);
    type(&p, "B\n");
    expectRead(p.s, "B\n");
    setL(p.s, IEXTEN_, 0);
    setI(p.s, 0, IUCLC_);
    /* INLCR makes a CR that IGNCR doesn't discard. */
    setI(p.s, INLCR_ | IGNCR_, 0);
    type(&p, "a\nb\r");
    setI(p.s, 0, INLCR_ | IGNCR_);
    type(&p, "\n");
    expectRead(p.s, "a\rb\n");
    /* PARMRK doubles a real 0xff. */
    setI(p.s, PARMRK_, 0);
    type(&p, "\xff\n");
    expectRead(p.s, "\xff\xff\n");
    setI(p.s, 0, PARMRK_);
    /* Raw too. */
    setL(p.s, 0, ICANON_ | ISIG_);
    setI(p.s, IUCLC_ | ISTRIP_, 0);
    type(&p, "A\xc1");
    expectRead(p.s, "aa");
    setI(p.s, 0, IUCLC_ | ISTRIP_);
    setI(p.s, PARMRK_, 0);
    type(&p, "\xff");
    expectRead(p.s, "\xff\xff");
    masterDrain(&p, echo, sizeof(echo));
    pairClose(&p);
}

static void test_output_flags(void) {
    Pair p = pairOpen();
    /* ONOCR, which goes by the column, and CR NL isn't asked. */
    setO(p.s, ONOCR_, 0);
    assert(p.s->ops->write(p.s, "\rab\r\r\n", 6) == 6);
    expectRead(p.m, "ab\r\r\n");
    setO(p.s, 0, ONOCR_);
    /* ONLRET: a NL leaves column 0, and a CR is still a CR. */
    setO(p.s, ONLRET_ | TAB3_, ONLCR_);
    assert(p.s->ops->write(p.s, "ab\n\t|\r", 6) == 6);
    expectRead(p.m, "ab\n        |\r");
    /* Without it a NL leaves the column alone. */
    setO(p.s, 0, ONLRET_);
    assert(p.s->ops->write(p.s, "ab\n\t|\r", 6) == 6);
    expectRead(p.m, "ab\n      |\r");
    /* What doesn't take a column: controls, and with IUTF8 the rest of a character. */
    assert(p.s->ops->write(p.s, "\x01\x7f\t|\r", 5) == 5);
    expectRead(p.m, "\x01\x7f        |\r");
    assert(p.s->ops->write(p.s, "\xc3\xa9\t|\r", 5) == 5);
    expectRead(p.m, "\xc3\xa9      |\r");
    setI(p.s, IUTF8_, 0);
    assert(p.s->ops->write(p.s, "\xc3\xa9\t|\r", 5) == 5);
    expectRead(p.m, "\xc3\xa9       |\r");
    setI(p.s, 0, IUTF8_);
    /* OLCUC, by Latin-1, 0xff and all. */
    setO(p.s, OLCUC_, TAB3_);
    assert(p.s->ops->write(p.s, "az{\xe9\xf7\xff\xdf", 7) == 7);
    expectRead(p.m, "AZ{\xc9\xf7\xdf\xbf");
    /* OPOST clear: not a thing changes, and the column doesn't follow. */
    assert(p.s->ops->write(p.s, "\r", 1) == 1);
    expectRead(p.m, "\r");
    setO(p.s, TAB3_, OPOST_ | OLCUC_);
    assert(p.s->ops->write(p.s, "ab\t\n", 4) == 4);
    expectRead(p.m, "ab\t\n");
    setO(p.s, OPOST_, 0);
    assert(p.s->ops->write(p.s, "\t|", 2) == 2);
    expectRead(p.m, "        |");
    pairClose(&p);
}

static void test_end_of_file(void) {
    Pair p = pairOpen();
    char c;
    setL(p.s, 0, ECHO_ | ISIG_);
    /* Alone: one read of 0, and the count says it was the key. */
    type(&p, "\x04");
    assert(p.s->ops->read(p.s, &c, 1) == 0);
    assert(pscalTtyTakeEofKey(p.s));
    assert(!pscalTtyTakeEofKey(p.s));
    assert(!pscalTtyTakeEofKey(NULL));
    expectAgain(p.s);
    /* After text: hands it over, and then waits. */
    type(&p, "xy\x04");
    expectRead(p.s, "xy");
    expectAgain(p.s);
    assert(!pscalTtyTakeEofKey(p.s));
    /* After text, a byte at a time: the mark goes with the last of it. */
    type(&p, "xy\x04");
    assert(p.s->ops->read(p.s, &c, 1) == 1 && c == 'x');
    assert(p.s->ops->read(p.s, &c, 1) == 1 && c == 'y');
    expectAgain(p.s);
    /* After a finished line: the mark stays for the next read. */
    type(&p, "one\n\x04two\n");
    expectRead(p.s, "one\n");
    assert(p.s->ops->read(p.s, &c, 1) == 0);
    assert(pscalTtyTakeEofKey(p.s));
    expectRead(p.s, "two\n");
    /* A short read splits a line, and the rest needs no wait. */
    type(&p, "hello\n");
    char got[8];
    assert(p.s->ops->read(p.s, got, 2) == 2 && memcmp(got, "he", 2) == 0);
    expectRead(p.s, "llo\n");
    /* A mark in a non-canonical read is a byte 0, wherever it falls. */
    type(&p, "ab\x04");
    setL(p.s, 0, ICANON_);
    assert(p.s->ops->read(p.s, got, 2) == 2 && memcmp(got, "ab", 2) == 0);
    assert(p.s->ops->read(p.s, got, 2) == 1 && got[0] == 0);
    expectAgain(p.s);
    setL(p.s, ICANON_, 0);
    type(&p, "ab\x04");
    setL(p.s, 0, ICANON_);
    assert(p.s->ops->read(p.s, got, sizeof(got)) == 3 && got[2] == 0);
    setL(p.s, ICANON_, 0);
    type(&p, "\x04");
    setL(p.s, 0, ICANON_);
    char m;
    assert(p.s->ops->read(p.s, &m, 1) == 1 && m == 0);
    assert(!pscalTtyTakeEofKey(p.s));
    /* A flush takes marks with it. */
    setL(p.s, ICANON_, 0);
    type(&p, "one\n\x04");
    assert(p.s->ops->ioctl(p.s, TCFLSH_, (void *)(intptr_t)TCIFLUSH_) == 0);
    expectAgain(p.s);
    pairClose(&p);
}

static void test_long_line(void) {
    Pair p = pairOpen();
    setL(p.s, 0, ECHO_);
    static char keys[TTY_BUF_SIZE + 1000];
    memset(keys, 'x', sizeof(keys));
    assert(tty_input(p.s->tty, keys, sizeof(keys), true) == (ssize_t)sizeof(keys));
    type(&p, "\n");
    static char got[TTY_BUF_SIZE + 8];
    assert(p.s->ops->read(p.s, got, sizeof(got)) == TTY_BUF_SIZE);
    assert(got[TTY_BUF_SIZE - 1] == '\n');
    expectAgain(p.s);
    /* A full queue with a finished line: non-blocking stops. */
    type(&p, "a\n");
    memset(keys, 'y', sizeof(keys));
    ssize_t first = tty_input(p.s->tty, keys, sizeof(keys), false);
    /* All consumed: the room there is, then drops past the line limit? No: stops when full. */
    assert(first == TTY_BUF_SIZE - 2);
    assert(tty_input(p.s->tty, keys, 10, false) == _EAGAIN);
    /* Reading the line makes room, and what is left is an over-long line that drops. */
    expectRead(p.s, "a\n");
    assert(tty_input(p.s->tty, keys, sizeof(keys), false) == (ssize_t)sizeof(keys));
    type(&p, "\n");
    assert(p.s->ops->read(p.s, got, sizeof(got)) == TTY_BUF_SIZE);
    /* Raw text filling the queue is a finished line after a change to line mode: a reader
     * can make room, so a key waits for that. */
    setL(p.s, 0, ICANON_);
    assert(tty_input(p.s->tty, keys, TTY_BUF_SIZE, false) == TTY_BUF_SIZE);
    setL(p.s, ICANON_, 0);
    assert(tty_input(p.s->tty, "z", 1, false) == _EAGAIN);
    assert(p.s->ops->read(p.s, got, sizeof(got)) == TTY_BUF_SIZE);
    assert(got[TTY_BUF_SIZE - 1] == 'y');
    type(&p, "z\n");
    expectRead(p.s, "z\n");
    /* A byte dropped from a line too long still shows. */
    setL(p.s, ECHO_, 0);
    assert(tty_input(p.s->tty, keys, TTY_BUF_SIZE - 1, false) == TTY_BUF_SIZE - 1);
    static char echoed[TTY_BUF_SIZE + 8];
    assert(masterDrain(&p, echoed, sizeof(echoed)) == TTY_BUF_SIZE - 1);
    type(&p, "w");
    assert(masterDrain(&p, echoed, sizeof(echoed)) == 1 && echoed[0] == 'w');
    pairClose(&p);
}

static void test_session_reader_keys(void) {
    Pair p = pairOpen();
    char echo[128];
    /* Without a session reader these stay data. */
    setL(p.s, 0, ISIG_);
    type(&p, "ab\x03" "c\n");
    masterDrain(&p, echo, sizeof(echo));
    expectRead(p.s, "ab\x03" "c\n");
    /* With one, they end the line at once, and take what was typed with them. */
    pscalTtySetSessionReader(p.s);
    pscalTtySetSessionReader(NULL);
    type(&p, "gone");
    expectAgain(p.s);
    type(&p, "\x03");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "gone^C\r\n") == 0);
    expectRead(p.s, "\x03");
    type(&p, "more\x1a");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "more^Z") == 0);
    expectRead(p.s, "\x1a");
    /* Lines typed ahead go too. */
    type(&p, "one\ntwo\n\x03");
    masterDrain(&p, echo, sizeof(echo));
    expectRead(p.s, "\x03");
    expectAgain(p.s);
    /* NOFLSH keeps them. */
    setL(p.s, NOFLSH_, 0);
    type(&p, "one\n\x03");
    masterDrain(&p, echo, sizeof(echo));
    expectRead(p.s, "one\n");
    expectRead(p.s, "\x03");
    setL(p.s, 0, NOFLSH_);
    /* Not in raw mode: there they stay data. */
    setL(p.s, 0, ICANON_ | ECHO_);
    type(&p, "\x03");
    expectRead(p.s, "\x03");
    pairClose(&p);
}

static void test_signal_keys(void) {
    fakeReset();
    Pair p = pairOpen();
    char echo[128];
    /* No foreground group: swallowed, and still echoed. */
    type(&p, "a\x03");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "a^C") == 0);
    assert(gSentCount == 0);
    p.s->tty->fg_group = 77;
    /* The group gets each signal once, in ascending order, whatever the keys' order. */
    type(&p, "\x1a\x03\x1c\x03");
    masterDrain(&p, echo, sizeof(echo));
    assert(strcmp(echo, "^Z^C^\\^C") == 0);
    assert(gSentCount == 3);
    assert(gSent[0].sig == SIGINT && gSent[1].sig == SIGQUIT && gSent[2].sig == SIGTSTP);
    assert(gSent[0].pgid == 77);
    /* The queue goes with the key, unless NOFLSH. */
    gSentCount = 0;
    type(&p, "one\n\x03");
    masterDrain(&p, echo, sizeof(echo));
    expectAgain(p.s);
    setL(p.s, NOFLSH_, 0);
    type(&p, "one\n\x03");
    masterDrain(&p, echo, sizeof(echo));
    expectRead(p.s, "one\n");
    setL(p.s, 0, NOFLSH_);
    /* Non-canonical: signals yes, echo no. */
    gSentCount = 0;
    setL(p.s, 0, ICANON_);
    type(&p, "ab\x03");
    assert(masterDrain(&p, echo, sizeof(echo)) == 0);
    assert(gSentCount == 1);
    expectAgain(p.s);
    /* A master's input never signals: writing to the slave's queue from the slave side. */
    gSentCount = 0;
    assert(tty_input(p.m->tty, "\x03", 1, false) == 1);
    assert(gSentCount == 0);
    expectRead(p.m, "\x03");
    pairClose(&p);
    fakeReset();
}

static void test_raw_modes(void) {
    Pair p = pairOpen();
    setL(p.s, 0, ICANON_ | ECHO_ | ISIG_);
    char echo[16];
    char got[64];
    /* No translation, no echo, whatever the flags. */
    setL(p.s, ECHO_, 0);
    type(&p, "a\r\x7f\x04");
    assert(masterDrain(&p, echo, sizeof(echo)) == 0);
    expectRead(p.s, "a\r\x7f\x04");
    setL(p.s, 0, ECHO_);
    /* VMIN of 0 never waits, and its 0 is not the key. */
    setCc(p.s, VMIN_, 0);
    assert(p.s->ops->read(p.s, got, 1) == 0);
    assert(!pscalTtyTakeEofKey(p.s));
    setCc(p.s, VMIN_, 1);
    /* Short of VMIN a poll doesn't call it readable, and a non-blocking read takes what's
     * there. Only with nothing there does it say so. */
    setCc(p.s, VMIN_, 3);
    type(&p, "ab");
    assert(p.s->ops->poll(p.s) == POLL_WRITE);
    expectRead(p.s, "ab");
    expectAgain(p.s);
    /* A read that asks for fewer than VMIN needs no more than that. */
    type(&p, "a");
    assert(p.s->ops->read(p.s, got, 1) == 1 && got[0] == 'a');
    /* With VTIME the wait ends with what there is, and a poll goes by the first byte. */
    type(&p, "ab");
    setCc(p.s, VTIME_, 1);
    assert(p.s->ops->poll(p.s) == (POLL_WRITE | POLL_READ));
    assert(p.s->ops->read(p.s, got, sizeof(got)) == 2);
    type(&p, "abc");
    assert(p.s->ops->read(p.s, got, sizeof(got)) == 3);
    /* VMIN 0 with VTIME: a read of nothing waits for the timer, or for a byte. */
    setCc(p.s, VMIN_, 0);
    assert(p.s->ops->read(p.s, got, sizeof(got)) == 0);
    assert(!pscalTtyTakeEofKey(p.s));
    expectAgain(p.s);
    type(&p, "q");
    assert(p.s->ops->read(p.s, got, sizeof(got)) == 1 && got[0] == 'q');
    setCc(p.s, VTIME_, 0);
    setCc(p.s, VMIN_, 1);
    /* Text from line mode is plain bytes, terminators included. */
    setL(p.s, ICANON_, 0);
    type(&p, "one\ntw");
    setL(p.s, 0, ICANON_);
    expectRead(p.s, "one\ntw");
    /* A raw byte does not end a line afterwards, but what is there when the mode changes
     * is all one line, for a reader to find. */
    type(&p, "x\ny");
    setL(p.s, ICANON_, 0);
    assert(p.s->ops->poll(p.s) == (POLL_WRITE | POLL_READ));
    type(&p, "z");
    expectRead(p.s, "x\ny");
    expectAgain(p.s);
    assert(p.s->ops->poll(p.s) == POLL_WRITE);
    assert(p.s->ops->ioctl(p.s, TCFLSH_, (void *)(intptr_t)TCIOFLUSH_) == 0);
    /* Lines don't survive going there and back. */
    type(&p, "a\nb\n");
    setL(p.s, 0, ICANON_);
    setL(p.s, ICANON_, 0);
    expectRead(p.s, "a\nb\n");
    pairClose(&p);
}

static void test_write(void) {
    Pair p = pairOpen();
    char got[64];
    assert(p.s->ops->write(p.s, "a\nb", 3) == 3);
    expectRead(p.m, "a\r\nb");
    setO(p.s, OCRNL_, 0);
    assert(p.s->ops->write(p.s, "a\r", 2) == 2);
    expectRead(p.m, "a\n");
    setO(p.s, ONLRET_, 0);
    assert(p.s->ops->write(p.s, "a\rb", 3) == 3);
    expectRead(p.m, "a\nb");
    setO(p.s, 0, OPOST_);
    assert(p.s->ops->write(p.s, "\r\n", 2) == 2);
    expectRead(p.m, "\r\n");
    /* A write of the master is typing. */
    assert(p.m->ops->write(p.m, "k\r", 2) == 2);
    expectRead(p.s, "k\n");
    expectRead(p.m, "k\n");
    /* More than a chunk, with every byte doubling. */
    setO(p.s, OPOST_ | ONLCR_, OCRNL_ | ONLRET_);
    static char lots[2000];
    memset(lots, '\n', sizeof(lots));
    assert(p.s->ops->write(p.s, lots, sizeof(lots)) == sizeof(lots));
    static char expanded[6100];
    assert(p.m->ops->read(p.m, expanded, sizeof(expanded)) == 4000);
    nb(p.s, true);
    /* The queue is empty now, and two more than fit report what was asked. */
    char big[5000];
    memset(big, 'z', sizeof(big));
    assert(p.s->ops->write(p.s, big, sizeof(big)) == sizeof(big));
    assert(p.s->ops->write(p.s, big, 10) == _EAGAIN);
    nb(p.s, false);
    /* Packet mode. */
    assert(p.m->ops->read(p.m, expanded, sizeof(expanded)) == 4096);
    dword_t on = 1, seen = 0;
    assert(p.m->ops->ioctl(p.m, TIOCPKT_, &on) == 0);
    assert(p.m->ops->ioctl(p.m, (int)TIOCGPKT_, &seen) == 0 && seen == 1);
    assert(p.s->ops->ioctl(p.s, TIOCPKT_, &on) == _ENOTTY);
    assert(p.s->ops->ioctl(p.s, (int)TIOCGPKT_, &seen) == _ENOTTY);
    assert(p.s->ops->write(p.s, "pk", 2) == 2);
    assert(p.m->ops->read(p.m, got, sizeof(got)) == 3 && got[0] == 0 && got[1] == 'p' && got[2] == 'k');
    expectAgain(p.m);
    assert(p.s->ops->write(p.s, "q", 1) == 1);
    assert(p.m->ops->read(p.m, got, 1) == 1 && got[0] == 0);
    assert(p.m->ops->read(p.m, got, sizeof(got)) == 2 && got[0] == 0 && got[1] == 'q');
    /* Events come first, as one byte, and are reported until read. */
    assert(p.m->ops->poll(p.m) == POLL_WRITE);
    assert(p.s->ops->write(p.s, "r", 1) == 1);
    assert(p.s->ops->ioctl(p.s, TCFLSH_, (void *)(intptr_t)TCIFLUSH_) == 0);
    assert(p.s->ops->ioctl(p.s, TCFLSH_, (void *)(intptr_t)TCOFLUSH_) == 0);
    assert(p.m->ops->poll(p.m) == (POLL_WRITE | POLL_READ | POLL_PRI));
    assert(p.m->ops->read(p.m, got, sizeof(got)) == 1 && got[0] == 3);
    assert(p.m->ops->poll(p.m) == (POLL_WRITE | POLL_READ));
    assert(p.m->ops->read(p.m, got, sizeof(got)) == 2 && got[1] == 'r');
    /* A flush of a master's own is nothing to tell it. */
    assert(p.m->ops->ioctl(p.m, TCFLSH_, (void *)(intptr_t)TCIOFLUSH_) == 0);
    expectAgain(p.m);
    /* Flow control coming and going, of which the last word counts. */
    struct termios_ modes;
    assert(p.s->ops->ioctl(p.s, TCGETS_, &modes) == 0);
    modes.iflags &= ~IXON_;
    assert(p.s->ops->ioctl(p.s, TCSETS_, &modes) == 0);
    assert(p.m->ops->read(p.m, got, sizeof(got)) == 1 && got[0] == TIOCPKT_NOSTOP_);
    assert(p.s->ops->ioctl(p.s, TCSETS_, &modes) == 0);
    expectAgain(p.m);
    modes.iflags |= IXON_;
    assert(p.s->ops->ioctl(p.s, TCSETS_, &modes) == 0);
    modes.iflags &= ~IXON_;
    assert(p.s->ops->ioctl(p.s, TCSETS_, &modes) == 0);
    modes.iflags |= IXON_;
    assert(p.s->ops->ioctl(p.s, TCSETSF_, &modes) == 0);
    assert(p.m->ops->read(p.m, got, sizeof(got)) == 1 && got[0] == (TIOCPKT_DOSTOP_ | TIOCPKT_FLUSHREAD_));
    /* A status is forgotten on the way to packet mode, not on the way out. */
    assert(p.s->ops->ioctl(p.s, TCFLSH_, (void *)(intptr_t)TCIFLUSH_) == 0);
    assert(p.m->ops->ioctl(p.m, TIOCPKT_, &on) == 0);
    assert(p.m->ops->read(p.m, got, sizeof(got)) == 1 && got[0] == 1);
    assert(p.s->ops->ioctl(p.s, TCFLSH_, (void *)(intptr_t)TCIFLUSH_) == 0);
    on = 0;
    assert(p.m->ops->ioctl(p.m, TIOCPKT_, &on) == 0);
    on = 1;
    assert(p.m->ops->ioctl(p.m, TIOCPKT_, &on) == 0);
    expectAgain(p.m);
    on = 0;
    assert(p.m->ops->ioctl(p.m, TIOCPKT_, &on) == 0);
    assert(p.m->ops->ioctl(p.m, (int)TIOCGPKT_, &seen) == 0 && seen == 0);
    /* Zero is zero, before anything else. */
    assert(p.s->ops->read(p.s, got, 0) == 0);
    pairClose(&p);
}

static void test_ioctl(void) {
    fakeReset();
    Pair p = pairOpen();
    dword_t v = 0;
    assert(p.m->ops->ioctl(p.m, (int)TIOCGPTN_, &v) == 0 && (int)v == p.num);
    assert(p.s->ops->ioctl(p.s, (int)TIOCGPTN_, &v) == _ENOTTY);
    assert(p.s->ops->ioctl(p.s, TIOCSPTLCK_, &v) == _ENOTTY);
    assert(p.s->ops->ioctl(p.s, 0x7777, &v) == _ENOTTY);
    assert(p.s->ops->ioctl(p.s, TIOCGPTPEER_, &v) == _ENOTTY);
    /* FIONREAD counts what a read of that side could return. */
    setL(p.s, 0, ECHO_);
    type(&p, "abc\x04" "d");
    assert(p.s->ops->ioctl(p.s, FIONREAD_, &v) == 0 && v == 3);
    assert(p.m->ops->ioctl(p.m, FIONREAD_, &v) == 0 && v == 0);
    assert(p.s->ops->ioctl(p.s, TCFLSH_, (void *)(intptr_t)7) == _EINVAL);
    assert(p.s->ops->ioctl(p.s, TCFLSH_, (void *)(intptr_t)TCOFLUSH_) == 0);
    assert(p.s->ops->ioctl(p.s, FIONREAD_, &v) == 0 && v == 3);
    /* TCSETSF clears; TCSETSW doesn't. Both reach the slave from the master. */
    struct termios_ t;
    assert(p.m->ops->ioctl(p.m, TCGETS_, &t) == 0);
    assert(p.m->ops->ioctl(p.m, TCSETSW_, &t) == 0);
    assert(p.s->ops->ioctl(p.s, FIONREAD_, &v) == 0 && v == 3);
    assert(p.m->ops->ioctl(p.m, TCSETSF_, &t) == 0);
    assert(p.s->ops->ioctl(p.s, FIONREAD_, &v) == 0 && v == 0);
    /* The op copies exactly its struct. */
    struct {
        struct termios_ t;
        unsigned char after[8];
    } guarded;
    memset(&guarded, 0x5a, sizeof(guarded));
    assert(p.s->ops->ioctl(p.s, TCGETS_, &guarded) == 0);
    for (size_t i = 0; i < 8; i++) assert(guarded.after[i] == 0x5a);
    /* Winsize, from either side, sent to nobody before a session. */
    struct winsize_ ws = {24, 80, 0, 0}, out;
    assert(p.m->ops->ioctl(p.m, TIOCSWINSZ_, &ws) == 0);
    assert(gSentCount == 0);
    assert(p.s->ops->ioctl(p.s, TIOCGWINSZ_, &out) == 0 && out.row == 24 && out.col == 80);
    assert(p.m->ops->ioctl(p.m, TIOCGWINSZ_, &out) == 0 && out.row == 24);
    /* A pointer that isn't one. */
    assert(p.s->ops->ioctl(p.s, TCGETS_, NULL) == _EFAULT);
    assert(p.s->ops->ioctl(p.s, TCSETS_, NULL) == _EFAULT);
    assert(p.s->ops->ioctl(p.s, TIOCSWINSZ_, NULL) == _EFAULT);
    assert(p.s->ops->ioctl(p.s, FIONREAD_, NULL) == _EFAULT);
    assert(p.m->ops->ioctl(p.m, TIOCGPGRP_, NULL) == _EFAULT);
    /* A pty has no line to set up. */
    t.cflags = 0x7fffffff;
    assert(p.s->ops->ioctl(p.s, TCSETS_, &t) == 0);
    assert(p.s->ops->ioctl(p.s, TCGETS_, &t) == 0 && t.cflags == 0x5ffffeff);
    t.cflags = 0;
    assert(p.s->ops->ioctl(p.s, TCSETS_, &t) == 0);
    assert(p.s->ops->ioctl(p.s, TCGETS_, &t) == 0 && t.cflags == (CS8_ | CREAD_));
    /* The rest of what Linux answers. */
    v = 7;
    assert(p.s->ops->ioctl(p.s, TIOCOUTQ_, &v) == 0 && v == 0);
    assert(p.s->ops->ioctl(p.s, TCSBRK_, (void *)1) == 0);
    assert(p.m->ops->ioctl(p.m, TCSBRKP_, NULL) == 0);
    assert(p.s->ops->ioctl(p.s, TIOCSTI_, "k") == _EIO);
    assert(p.s->ops->ioctl(p.s, (int)TIOCGEXCL_, &v) == 0 && v == 0);
    assert(p.s->ops->ioctl(p.s, TIOCEXCL_, NULL) == 0);
    assert(p.s->ops->ioctl(p.s, (int)TIOCGEXCL_, &v) == 0 && v == 1);
    assert(p.m->ops->ioctl(p.m, (int)TIOCGEXCL_, &v) == 0 && v == 0);
    struct pscal_fd *busy = NULL;
    assert(pscalPtyOpenSlave(p.num, O_RDWR | O_NOCTTY, &busy) == _EBUSY && !busy);
    assert(p.s->ops->ioctl(p.s, TIOCNXCL_, NULL) == 0);
    assert(pscalPtyOpenSlave(p.num, O_RDWR | O_NOCTTY, &busy) == 0);
    pscal_fd_close(busy);
    pairClose(&p);
    fakeReset();
}

static void test_winsize_targets(void) {
    fakeReset();
    Pair p = pairOpen();
    struct winsize_ ws = {1, 2, 0, 0};
    /* The foreground group, when there's a change. */
    p.s->tty->session = 10;
    p.s->tty->fg_group = 20;
    tty_set_winsize(p.s->tty, ws);
    assert(gSentCount == 1 && gSent[0].pgid == 20 && gSent[0].sig == SIGWINCH);
    gSentCount = 0;
    tty_set_winsize(p.s->tty, ws);
    assert(gSentCount == 0);
    /* Every field is a change. */
    ws.xpixel = 5;
    tty_set_winsize(p.s->tty, ws);
    assert(gSentCount == 1);
    /* No foreground group: nobody, whatever vproc has recorded, and no adopting it. */
    gSentCount = 0;
    p.s->tty->fg_group = 0;
    gRecordedFg = 30;
    ws.row = 2;
    tty_set_winsize(p.s->tty, ws);
    assert(gSentCount == 0);
    assert(p.s->tty->fg_group == 0);
    /* No session either: no signal, and the size is kept. */
    p.s->tty->session = 0;
    ws.row = 3;
    tty_set_winsize(p.s->tty, ws);
    assert(gSentCount == 0);
    assert(p.s->tty->winsize.row == 3);
    pairClose(&p);
    fakeReset();
}

static void test_sessions(void) {
    fakeReset();
    struct pscal_fd *fd = NULL;
    /* Nothing to open yet. */
    assert(pscalTtyOpenControlling(O_RDWR, NULL) == _EINVAL);
    assert(pscalTtyOpenControlling(O_RDWR, &fd) == _ENXIO && !fd);
    gSid = 100;
    gPgid = 101;
    assert(pscalTtyOpenControlling(O_RDWR, &fd) == _ENXIO);
    assert(pscalTtySessionPtyNum(100) == -1);
    assert(pscalTtySessionPtyNum(0) == -1);

    /* Claim on open: only for a leader, only without O_NOCTTY. */
    struct pscal_fd *m = NULL, *s = NULL;
    int num = -1;
    assert(pscalPtyOpenMaster(O_RDWR | O_NOCTTY, &m, &num) == 0);
    assert(pscalPtyUnlock(m) == 0);
    assert(pscalPtyOpenSlave(num, O_RDWR, &s) == 0);
    assert(s->tty->session == 0);
    pscal_fd_close(s);
    /* That was the last open: reset to a pair that isn't hung up. */
    pscal_fd_close(m);
    assert(pscalPtyOpenMaster(O_RDWR | O_NOCTTY, &m, &num) == 0);
    assert(pscalPtyUnlock(m) == 0);
    gLeader = true;
    assert(pscalPtyOpenSlave(num, O_RDWR | O_NOCTTY, &s) == 0);
    assert(s->tty->session == 0);
    pscal_fd_close(s);
    pscal_fd_close(m);

    assert(pscalPtyOpenMaster(O_RDWR | O_NOCTTY, &m, &num) == 0);
    assert(pscalPtyUnlock(m) == 0);
    assert(pscalPtyOpenSlave(num, O_RDWR, &s) == 0);
    struct tty *slave = s->tty;
    assert(slave->session == 100 && slave->fg_group == 101);
    assert(gLastSetFgSid == 100 && gLastSetFg == 101);
    assert(pscalTtyIsControlling(slave));
    assert(!pscalTtyIsControlling(NULL));
    assert(pscalTtySessionPtyNum(100) == num);

    /* A second terminal isn't taken by the same session. */
    struct pscal_fd *m2 = NULL, *s2 = NULL;
    int num2 = -1;
    assert(pscalPtyOpenMaster(O_RDWR | O_NOCTTY, &m2, &num2) == 0);
    assert(pscalPtyUnlock(m2) == 0);
    assert(pscalPtyOpenSlave(num2, O_RDWR, &s2) == 0);
    assert(s2->tty->session == 0);

    /* TIOCSCTTY's table. */
    assert(s->ops->ioctl(s, TIOCSCTTY_, NULL) == 0);
    assert(s2->ops->ioctl(s2, TIOCSCTTY_, NULL) == _EPERM);
    gLeader = false;
    assert(s2->ops->ioctl(s2, TIOCSCTTY_, (void *)1) == _EPERM);
    gLeader = true;
    gSid = -1;
    assert(s2->ops->ioctl(s2, TIOCSCTTY_, (void *)1) == _EPERM);
    gSid = 200;
    gPgid = 0;
    assert(s->ops->ioctl(s, TIOCSCTTY_, NULL) == _EPERM);
    assert(s2->ops->ioctl(s2, TIOCSCTTY_, NULL) == 0);
    assert(s2->tty->session == 200 && s2->tty->fg_group == 200);
    /* Force takes one from another session, which then has none. */
    gSid = 300;
    gPgid = 301;
    assert(s->ops->ioctl(s, TIOCSCTTY_, (void *)1) == 0);
    assert(slave->session == 300 && slave->fg_group == 301);
    assert(pscalTtyIsControlling(slave));
    gSid = 100;
    assert(!pscalTtyIsControlling(slave));
    assert(pscalTtySessionPtyNum(100) == -1);
    gSid = 300;

    /* The foreground group, through both sides. */
    pid_t_ got = 0;
    assert(s->ops->ioctl(s, TIOCGPGRP_, &got) == 0 && got == 301);
    assert(m->ops->ioctl(m, TIOCGPGRP_, &got) == 0 && got == 301);
    dword_t want = 555;
    assert(s->ops->ioctl(s, TIOCSPGRP_, &want) == 0);
    assert(slave->fg_group == 555 && gLastSetFgSid == 300 && gLastSetFg == 555);
    gSid = 999;
    assert(s->ops->ioctl(s, TIOCGPGRP_, &got) == _ENOTTY);
    assert(s->ops->ioctl(s, TIOCSPGRP_, &want) == _ENOTTY);
    /* A master answers with no checks. */
    assert(m->ops->ioctl(m, TIOCGPGRP_, &got) == 0 && got == 555);
    gSid = 300;
    /* A group has to be one, and of this session. */
    want = (dword_t)-5;
    assert(s->ops->ioctl(s, TIOCSPGRP_, &want) == _EINVAL);
    want = 0;
    assert(s->ops->ioctl(s, TIOCSPGRP_, &want) == _ESRCH);
    want = 777;
    gGroupCheck = _EPERM;
    assert(s->ops->ioctl(s, TIOCSPGRP_, &want) == _EPERM);
    gGroupCheck = _ESRCH;
    assert(s->ops->ioctl(s, TIOCSPGRP_, &want) == _ESRCH);
    gGroupCheck = 0;
    assert(slave->fg_group == 555);
    assert(s->ops->ioctl(s, TIOCSPGRP_, NULL) == _EFAULT);
    /* Through a master, if its slave is the caller's. */
    want = 555;
    assert(m->ops->ioctl(m, TIOCSPGRP_, &want) == 0);
    assert(m2->ops->ioctl(m2, TIOCSPGRP_, &want) == _ENOTTY);
    /* No foreground group is an answer. */
    slave->fg_group = 0;
    got = -1;
    assert(s->ops->ioctl(s, TIOCGPGRP_, &got) == 0 && got == 0);
    slave->fg_group = 555;
    /* The session, through either side. */
    got = -1;
    assert(s->ops->ioctl(s, TIOCGSID_, &got) == 0 && got == 300);
    got = -1;
    assert(m->ops->ioctl(m, TIOCGSID_, &got) == 0 && got == 300);
    assert(s2->ops->ioctl(s2, TIOCGSID_, &got) == _ENOTTY);

    /* Background reads. */
    gPgid = 301;
    gSentCount = 0;
    assert(pscalTtyCheckBackgroundRead(s) == _EINTR);
    assert(gSentCount == 1 && gSent[0].pgid == 301 && gSent[0].sig == SIGTTIN);
    gSendResult = _ESRCH;
    assert(pscalTtyCheckBackgroundRead(s) == _EIO);
    gSendResult = 0;
    char c;
    assert(s->ops->read(s, &c, 1) == _EINTR);
    /* Asking for nothing is still asking. */
    assert(s->ops->read(s, &c, 0) == _EINTR);
    /* Nothing would come of the signal: no signal, and the read can't go on. */
    gSentCount = 0;
    gRefused = true;
    assert(s->ops->read(s, &c, 1) == _EIO);
    gRefused = false;
    gOrphaned = true;
    assert(s->ops->read(s, &c, 1) == _EIO);
    gOrphaned = false;
    assert(gSentCount == 0);
    /* Stopped, continued in the foreground, and the read goes on. */
    gAfterSignal = 0;
    gAfterSignals = 0;
    gGroupAfterSignal = 555;
    gSentCount = 0;
    nb(s, true);
    assert(s->ops->read(s, &c, 1) == _EAGAIN);
    nb(s, false);
    assert(gSentCount == 1 && gAfterSignals == 1);
    gAfterSignal = _EINTR;
    gGroupAfterSignal = -1;
    gAfterSignals = 0;
    /* What vproc has recorded as the foreground counts as well. */
    gPgid = 301;
    gRecordedFg = 301;
    gSentCount = 0;
    assert(pscalTtyCheckBackgroundRead(s) == 0);
    gRecordedFg = -1;
    /* A session's reader is no process, and is never stopped. */
    gKernel = true;
    assert(pscalTtyCheckBackgroundRead(s) == 0);
    assert(gSentCount == 0);
    gKernel = false;
    gPgid = 555;
    gSentCount = 0;
    assert(pscalTtyCheckBackgroundRead(s) == 0);
    assert(gSentCount == 0);
    assert(pscalTtyCheckBackgroundRead(NULL) == 0);
    /* Writes aren't stopped, unless TOSTOP asks for it. */
    gPgid = 1;
    assert(s->ops->write(s, "w", 1) == 1);
    expectRead(m, "w");
    setL(s, TOSTOP_, 0);
    assert(s->ops->write(s, "w", 1) == _EINTR);
    assert(gSentCount == 1 && gSent[0].pgid == 1 && gSent[0].sig == SIGTTOU);
    assert(s->ops->write(s, "w", 0) == _EINTR);
    gOrphaned = true;
    gSentCount = 0;
    assert(s->ops->write(s, "w", 1) == _EIO);
    gRefused = true;
    assert(s->ops->write(s, "w", 1) == 1);
    expectRead(m, "w");
    gOrphaned = false;
    assert(s->ops->write(s, "w", 1) == 1);
    expectRead(m, "w");
    gRefused = false;
    assert(gSentCount == 0);
    gKernel = true;
    assert(s->ops->write(s, "w", 1) == 1);
    expectRead(m, "w");
    gKernel = false;
    /* A master's writer has nothing to do with it. */
    assert(m->ops->write(m, "", 0) == 0);
    setL(s, 0, TOSTOP_);
    gPgid = 555;

    /* Opening what controls this session gives a new open of it. */
    struct pscal_fd *ctl = NULL;
    assert(pscalTtyOpenControlling(O_RDWR | O_NONBLOCK, &ctl) == 0);
    assert(ctl && ctl != s && ctl->tty == slave && (ctl->flags & O_NONBLOCK));
    assert(!(s->flags & O_NONBLOCK));
    pscal_fd_close(ctl);

    /* Setting it again is the same one reference, from any session. */
    int refs_before = __atomic_load_n(&slave->refs, __ATOMIC_RELAXED);
    gSid = 400;
    pscalTtySetControlling(slave);
    assert(__atomic_load_n(&slave->refs, __ATOMIC_RELAXED) == refs_before);
    gSid = 300;
    assert(pscalTtyIsControlling(slave));

    /* TIOCNOTTY: not for a master or a stranger. Someone not leading the session lets go
     * for itself, and a leader for all of it. */
    assert(m->ops->ioctl(m, TIOCNOTTY_, NULL) == _ENOTTY);
    assert(s2->ops->ioctl(s2, TIOCNOTTY_, NULL) == _ENOTTY);
    gLeader = false;
    assert(s->ops->ioctl(s, TIOCNOTTY_, NULL) == 0);
    assert(gGaveUp);
    assert(!pscalTtyIsControlling(slave));
    assert(s->ops->ioctl(s, TIOCGPGRP_, &got) == _ENOTTY);
    assert(s->ops->ioctl(s, TIOCNOTTY_, NULL) == _ENOTTY);
    assert(pscalTtyOpenControlling(O_RDWR, &ctl) == _ENXIO);
    gGaveUp = false;
    assert(pscalTtyIsControlling(slave));
    /* Exclusive, it can't be opened as /dev/tty either. */
    assert(s->ops->ioctl(s, TIOCEXCL_, NULL) == 0);
    assert(pscalTtyOpenControlling(O_RDWR, &ctl) == _EBUSY);
    assert(s->ops->ioctl(s, TIOCNXCL_, NULL) == 0);
    gLeader = true;
    assert(s->ops->ioctl(s, TIOCNOTTY_, NULL) == 0);
    assert(!gGaveUp);
    assert(!pscalTtyIsControlling(slave));
    assert(slave->session == 0);
    assert(__atomic_load_n(&slave->refs, __ATOMIC_RELAXED) == refs_before - 1);
    assert(s->ops->ioctl(s, TIOCSCTTY_, NULL) == 0);
    assert(pscalTtyIsControlling(slave));

    /* Clear, and drop. */
    pscalTtyClearControlling(slave);
    assert(slave->session == 0 && slave->fg_group == 0);
    assert(!pscalTtyIsControlling(slave));
    assert(__atomic_load_n(&slave->refs, __ATOMIC_RELAXED) == refs_before - 1);
    pscalTtyDropSession(0);
    pscalTtyDropSession(-1);
    pscalTtyDropSession(12345);
    gSid = 200;
    assert(pscalTtyIsControlling(s2->tty));
    pscalTtyDropSession(200);
    assert(!pscalTtyIsControlling(s2->tty));
    assert(s2->tty->session == 0 && s2->tty->fg_group == 0);

    pscal_fd_close(s2);
    pscal_fd_close(m2);
    pscal_fd_close(s);
    pscal_fd_close(m);
    pscalTtyDropSession(300);
    assert(!pscalPtyExists(num));

    /* A master never controls anything, by an open with or without O_NOCTTY. */
    gSid = 500;
    gPgid = 500;
    gLeader = true;
    assert(pscalPtyOpenMaster(O_RDWR, &m, &num) == 0);
    assert(m->tty->session == 0);
    assert(pscalTtySessionPtyNum(500) == -1);
    assert(pscalPtyUnlock(m) == 0);
    /* Asked of a master, it's the slave that's claimed. */
    assert(pscalPtyOpenSlave(num, O_RDWR | O_NOCTTY, &s) == 0);
    assert(s->tty->session == 0);
    assert(m->ops->ioctl(m, TIOCSCTTY_, NULL) == 0);
    assert(s->tty->session == 500 && m->tty->session == 0);
    assert(pscalTtyIsControlling(s->tty));
    pscalTtyDropSession(500);
    pscal_fd_close(s);
    pscal_fd_close(m);
    fakeReset();
}

static void test_numbers(void) {
    fakeReset();
    /* Lowest free, and reuse. */
    Pair a = pairOpen(), b = pairOpen(), c = pairOpen();
    assert(b.num == a.num + 1 && c.num == b.num + 1);
    pairClose(&b);
    Pair d = pairOpen();
    assert(d.num == a.num + 1);
    pairClose(&d);
    pairClose(&c);
    pairClose(&a);

    assert(pscalPtyGetLimit() == 4096);
    assert(!pscalPtyExists(-1) && !pscalPtyExists(4096));
    assert(pscalPtyOpenMaster(O_RDWR, NULL, NULL) == _EINVAL);

    struct pscal_fd *m = NULL, *s = NULL;
    int num = -1;
    /* Locked until asked, and then only by a master. */
    assert(pscalPtyOpenMaster(O_RDWR | O_NOCTTY, &m, &num) == 0);
    assert(pscalPtyExists(num));
    assert(pscalPtyOpenSlave(num, O_RDWR, &s) == _EIO && !s);
    assert(pscalPtyOpenSlave(num, O_RDWR, NULL) == _EINVAL);
    assert(pscalPtyOpenSlave(-1, O_RDWR, &s) == _ENOENT);
    assert(pscalPtyOpenSlave(4096, O_RDWR, &s) == _ENOENT);
    assert(pscalPtyOpenSlave(num + 1, O_RDWR, &s) == _ENOENT);
    assert(pscalPtyUnlock(NULL) == _EINVAL);
    dword_t lock_it = 1;
    assert(pscalPtyUnlock(m) == 0);
    assert(m->ops->ioctl(m, TIOCSPTLCK_, &lock_it) == 0);
    assert(pscalPtyOpenSlave(num, O_RDWR, &s) == _EIO);
    assert(pscalPtyUnlock(m) == 0);
    assert(pscalPtyOpenSlave(num, O_RDWR | O_NOCTTY, &s) == 0);

    /* Attributes. */
    mode_t_ perms = 0;
    uid_t_ uid = 1;
    gid_t_ gid = 1;
    assert(pscalPtyGetSlaveInfo(num, &perms, &uid, &gid) == 0);
    assert(perms == 0620 && uid == (uid_t_)geteuid() && gid == (gid_t_)getegid());
    assert(pscalPtyGetSlaveInfo(num, NULL, NULL, NULL) == 0);
    assert(pscalPtyGetSlaveInfo(4096, &perms, NULL, NULL) == -1);
    assert(pscalPtyGetSlaveInfo(num + 1, &perms, NULL, NULL) == -1);
    const mode_t_ wide = 0644;
    assert(pscalPtySetSlaveInfo(num, &wide, NULL, NULL) == 0);
    assert(pscalPtyGetSlaveInfo(num, &perms, &uid, NULL) == 0 && perms == 0644 && uid == (uid_t_)geteuid());
    assert(pscalPtySetSlaveInfo(4096, &wide, NULL, NULL) == _ENOENT);
    assert(pscalPtySetSlaveInfo(num + 1, &wide, NULL, NULL) == _ENOENT);

    /* Another slave open, and a reopen, on the same terminal. */
    struct pscal_fd *s2 = NULL;
    assert(pscalPtyOpenSlave(num, O_RDONLY | O_NONBLOCK | O_NOCTTY, &s2) == 0);
    assert(s2->tty == s->tty && s2 != s);
    assert((s2->flags & O_NONBLOCK) && !(s->flags & O_NONBLOCK));
    pscal_fd_close(s2);
    pscal_fd_close(s);
    assert(pscalPtyOpenSlave(num, O_RDWR | O_NOCTTY, &s) == 0);
    pscal_fd_close(s);
    pscal_fd_close(m);
    assert(!pscalPtyExists(num));
    assert(pscalPtyGetSlaveInfo(num, &perms, NULL, NULL) == -1);

    /* Everything taken. */
    enum { ALL = 4096 };
    struct pscal_fd **all = (struct pscal_fd **)calloc(ALL, sizeof(*all));
    int taken = 0;
    while (taken < ALL) {
        int n = -1;
        assert(pscalPtyOpenMaster(O_RDWR | O_NOCTTY, &all[taken], &n) == 0);
        assert(n == taken);
        taken++;
    }
    struct pscal_fd *over = NULL;
    int n = -7;
    assert(pscalPtyOpenMaster(O_RDWR | O_NOCTTY, &over, &n) == _ENOSPC);
    assert(!over && n == -7);
    for (int i = 0; i < ALL; i++) pscal_fd_close(all[i]);
    free(all);
    assert(!pscalPtyExists(0));
    fakeReset();
}

static void test_hangup(void) {
    fakeReset();
    char c;
    /* A master before any slave open isn't hung up. */
    struct pscal_fd *m = NULL, *s = NULL;
    int num = -1;
    assert(pscalPtyOpenMaster(O_RDWR | O_NOCTTY, &m, &num) == 0);
    assert(pscalPtyUnlock(m) == 0);
    nb(m, true);
    assert(m->ops->read(m, &c, 1) == _EAGAIN);
    nb(m, false);
    assert(m->ops->poll(m) == POLL_WRITE);
    assert(m->ops->write(m, "x\n", 2) == 2);

    /* The last slave open closing hangs up the master; the first of two doesn't. */
    assert(pscalPtyOpenSlave(num, O_RDWR | O_NOCTTY, &s) == 0);
    struct pscal_fd *s2 = NULL;
    assert(pscalPtyOpenSlave(num, O_RDWR | O_NOCTTY, &s2) == 0);
    pscal_fd_close(s2);
    assert(!(m->ops->poll(m) & POLL_HUP));
    assert(s->ops->write(s, "out", 3) == 3);
    pscal_fd_close(s);
    /* With data queued, it still reads 0. */
    assert(m->ops->read(m, &c, 1) == 0);
    assert(m->ops->poll(m) == (POLL_READ | POLL_WRITE | POLL_ERR | POLL_HUP));
    assert(m->ops->write(m, "x", 1) == _EIO);
    dword_t v;
    assert(m->ops->ioctl(m, FIONREAD_, &v) == _EIO);
    assert(m->ops->ioctl(m, TIOCSPGRP_, &v) == _ENOTTY);
    /* It stays that way after a reopen. */
    assert(pscalPtyOpenSlave(num, O_RDWR | O_NOCTTY, &s) == 0);
    assert(m->ops->read(m, &c, 1) == 0);

    /* The master going: slave hung up, link gone, opens fail. */
    struct tty *slave = s->tty;
    pscal_fd_close(m);
    assert(slave->pty.other == NULL);
    assert(s->ops->read(s, &c, 1) == 0);
    assert(s->ops->write(s, "x", 1) == _EIO);
    assert(s->ops->poll(s) == (POLL_READ | POLL_WRITE | POLL_ERR | POLL_HUP));
    assert(s->ops->ioctl(s, TIOCGWINSZ_, &v) == _EIO);
    assert(s->ops->ioctl(s, TIOCSPGRP_, &v) == _ENOTTY);
    /* It still queues keys. */
    assert(tty_input(slave, "k\n", 2, false) == 2);
    assert(!pscalTtyTakeEofKey(s));
    /* The number stays taken while the slave is held. */
    assert(pscalPtyExists(num));
    struct pscal_fd *late = NULL;
    assert(pscalPtyOpenSlave(num, O_RDWR, &late) == _ENOENT && !late);
    pscal_fd_close(s);
    assert(!pscalPtyExists(num));

    /* tty_hangup directly: a slave's tells the master, a master's only itself. */
    Pair p = pairOpen();
    lock(&p.s->tty->lock);
    tty_hangup(p.s->tty);
    unlock(&p.s->tty->lock);
    assert(p.s->ops->read(p.s, &c, 1) == 0);
    assert(p.m->ops->read(p.m, &c, 1) == 0);
    assert(p.m->ops->write(p.m, "x", 1) == _EIO);
    assert(p.m->ops->poll(p.m) == (POLL_READ | POLL_WRITE | POLL_HUP));
    pairClose(&p);

    p = pairOpen();
    lock(&p.m->tty->lock);
    tty_hangup(p.m->tty);
    unlock(&p.m->tty->lock);
    assert(p.m->ops->read(p.m, &c, 1) == 0);
    nb(p.s, true);
    assert(p.s->ops->read(p.s, &c, 1) == _EAGAIN);
    nb(p.s, false);
    pairClose(&p);
    fakeReset();
}

/* --- threads --- */

typedef struct {
    struct pscal_fd *fd;
    char buf[64];
    size_t size;
    ssize_t result;
    volatile bool started, done;
} Op;

static void *readerMain(void *arg) {
    Op *op = (Op *)arg;
    op->started = true;
    op->result = op->fd->ops->read(op->fd, op->buf, op->size);
    op->done = true;
    return NULL;
}

static void *writerMain(void *arg) {
    Op *op = (Op *)arg;
    static char big[20000];
    memset(big, 'w', sizeof(big));
    op->started = true;
    op->result = op->fd->ops->write(op->fd, big, sizeof(big));
    op->done = true;
    return NULL;
}

static pthread_t startOp(void *(*fn)(void *), Op *op) {
    pthread_t t;
    assert(pthread_create(&t, NULL, fn, op) == 0);
    while (!op->started) msleep(1);
    msleep(30);
    return t;
}

static void test_waiting(void) {
    fakeReset();
    Pair p = pairOpen();
    setL(p.s, 0, ECHO_ | ISIG_);
    /* A waiting read follows a change to raw, with no new input. */
    type(&p, "part");
    Op op = {.fd = p.s, .size = sizeof(op.buf)};
    pthread_t t = startOp(readerMain, &op);
    assert(!op.done);
    setL(p.s, 0, ICANON_);
    pthread_join(t, NULL);
    assert(op.result == 4 && memcmp(op.buf, "part", 4) == 0);

    /* And to line mode: then an end-of-file key is a mark, not a byte. */
    memset(&op, 0, sizeof(op));
    op.fd = p.s;
    op.size = sizeof(op.buf);
    t = startOp(readerMain, &op);
    setL(p.s, ICANON_, 0);
    msleep(20);
    type(&p, "\x04");
    pthread_join(t, NULL);
    assert(op.result == 0 && pscalTtyTakeEofKey(p.s));

    /* A cancelled read leaves the terminal usable and its cleanup can close. */
    memset(&op, 0, sizeof(op));
    op.fd = p.s;
    op.size = sizeof(op.buf);
    t = startOp(readerMain, &op);
    assert(pthread_cancel(t) == 0);
    void *ret = NULL;
    pthread_join(t, &ret);
    assert(ret == PTHREAD_CANCELED);
    type(&p, "after\n");
    expectRead(p.s, "after\n");

    /* A blocked write, cancelled the same way, and the pin it held lets go. */
    memset(&op, 0, sizeof(op));
    op.fd = p.s;
    t = startOp(writerMain, &op);
    assert(!op.done);
    assert(pthread_cancel(t) == 0);
    pthread_join(t, &ret);
    assert(ret == PTHREAD_CANCELED);
    assert(p.m->tty->pins == 1);
    char sink[8192];
    assert(p.m->ops->read(p.m, sink, sizeof(sink)) > 0);

    /* A blocked write finishes as the reader makes room. */
    memset(&op, 0, sizeof(op));
    op.fd = p.s;
    t = startOp(writerMain, &op);
    size_t total = 0;
    while (total < 20000) {
        ssize_t r = p.m->ops->read(p.m, sink, sizeof(sink));
        assert(r > 0);
        total += (size_t)r;
    }
    pthread_join(t, NULL);
    assert(op.result == 20000 && total == 20000);

    /* A blocked read wakes on a master's going. Blocked writer too. */
    memset(&op, 0, sizeof(op));
    op.fd = p.s;
    op.size = sizeof(op.buf);
    t = startOp(readerMain, &op);
    Op wop = {.fd = p.s};
    pthread_t wt = startOp(writerMain, &wop);
    assert(!op.done && !wop.done);
    pscal_fd_close(p.m);
    p.m = NULL;
    pthread_join(t, NULL);
    assert(op.result == 0);
    pthread_join(wt, NULL);
    pairClose(&p);

    /* Queues full both ways, with no lock held by either writer. */
    p = pairOpen();
    setL(p.s, 0, ICANON_ | ECHO_ | ISIG_);
    Op w1 = {.fd = p.s}, w2 = {.fd = p.m};
    pthread_t t1 = startOp(writerMain, &w1);
    pthread_t t2 = startOp(writerMain, &w2);
    assert(!w1.done && !w2.done);
    /* A non-blocking key still answers at once. */
    assert(tty_input(p.s->tty, "x", 1, false) == _EAGAIN);
    assert(p.s->ops->poll(p.s) & POLL_READ);
    assert(p.m->ops->poll(p.m) & POLL_READ);
    size_t got1 = 0, got2 = 0;
    while (got1 < 20000 || got2 < 20000) {
        if (got1 < 20000) {
            ssize_t r = p.m->ops->read(p.m, sink, sizeof(sink));
            assert(r > 0);
            got1 += (size_t)r;
        }
        if (got2 < 20000) {
            ssize_t r = p.s->ops->read(p.s, sink, sizeof(sink));
            assert(r > 0);
            got2 += (size_t)r;
        }
    }
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    assert(w1.result == 20000 && w2.result == 20000);
    pairClose(&p);

    /* The key writer doesn't wait on a master that isn't read, echo included. */
    p = pairOpen();
    static char keys[9000];
    memset(keys, 'e', sizeof(keys));
    assert(p.s->ops->write(p.s, keys, TTY_BUF_SIZE) == TTY_BUF_SIZE);
    assert(tty_input(p.s->tty, "typed", 5, false) == 5);
    pairClose(&p);
    fakeReset();
}

static void test_poll(void) {
    fakeReset();
    Pair p = pairOpen();
    setL(p.s, 0, ECHO_);
    int wake = pscalPollWakeFd();
    assert(wake >= 0);
    assert(fcntl(wake, F_GETFL) & O_NONBLOCK);
    assert(fcntl(wake, F_GETFD) & FD_CLOEXEC);
    pscalPollDrain();
    struct pollfd pfd = {.fd = wake, .events = POLLIN, .revents = 0};
    assert(poll(&pfd, 1, 0) == 0);

    /* An unfinished line is nothing to wake for, or to report. */
    type(&p, "ab");
    assert(poll(&pfd, 1, 0) == 0);
    assert(p.s->ops->poll(p.s) == POLL_WRITE);
    type(&p, "\n");
    assert(poll(&pfd, 1, 0) == 1);
    pscalPollDrain();
    assert(p.s->ops->poll(p.s) == (POLL_WRITE | POLL_READ));
    expectRead(p.s, "ab\n");
    assert(p.s->ops->poll(p.s) == POLL_WRITE);
    type(&p, "\x04");
    assert(poll(&pfd, 1, 0) == 1);
    pscalPollDrain();
    assert(p.s->ops->poll(p.s) == (POLL_WRITE | POLL_READ));
    char c;
    assert(p.s->ops->read(p.s, &c, 1) == 0);
    assert(pscalTtyTakeEofKey(p.s));

    /* Raw: any byte. A change to raw that reveals text wakes too. */
    type(&p, "un");
    setL(p.s, 0, ICANON_);
    assert(poll(&pfd, 1, 0) == 1);
    pscalPollDrain();
    assert(p.s->ops->poll(p.s) == (POLL_WRITE | POLL_READ));
    expectRead(p.s, "un");
    assert(p.s->ops->poll(p.s) == POLL_WRITE);
    type(&p, "x");
    assert(poll(&pfd, 1, 0) == 1);
    pscalPollDrain();
    expectRead(p.s, "x");

    /* Output reaches the master's poller. */
    assert(p.s->ops->write(p.s, "o", 1) == 1);
    assert(poll(&pfd, 1, 0) == 1);
    pscalPollDrain();
    assert(p.m->ops->poll(p.m) == (POLL_WRITE | POLL_READ));

    /* Hang-up wakes. */
    pscal_fd_close(p.m);
    p.m = NULL;
    assert(poll(&pfd, 1, 0) == 1);
    pscalPollDrain();

    /* errno is left alone. */
    errno = E2BIG;
    pscal_fd_poll_wakeup(NULL, POLLIN);
    pscalPollDrain();
    assert(errno == E2BIG);
    pairClose(&p);
    fakeReset();
}

static int gClosed = 0;
static int fakeClose(struct pscal_fd *fd) {
    (void)fd;
    gClosed++;
    return 42;
}
static const struct pscal_fd_ops kFakeOps = {.close = fakeClose};

static void test_fd(void) {
    assert(pscal_fd_close(NULL) == _EBADF);
    assert(pscal_fd_retain(NULL) == NULL);
    struct pscal_fd *fd = pscal_fd_create(&kFakeOps);
    assert(fd && fd->ops == &kFakeOps && !fd->tty && !fd->userdata && fd->flags == 0);
    assert(pscal_fd_retain(fd) == fd);
    assert(pscal_fd_close(fd) == 0 && gClosed == 0);
    assert(pscal_fd_close(fd) == 42 && gClosed == 1);
    static const struct pscal_fd_ops kNoOps = {0};
    fd = pscal_fd_create(&kNoOps);
    assert(pscal_fd_close(fd) == 0);
    /* Not terminals: everything asked about them is a quiet no. */
    fd = pscal_fd_create(&kNoOps);
    pscalTtySetSessionReader(fd);
    assert(!pscalTtyTakeEofKey(fd));
    assert(pscalTtyCheckBackgroundRead(fd) == 0);
    assert(!pscalPtyIsMaster(fd) && !pscalPtyIsSlave(fd));
    assert(pscalPtyUnlock(fd) == _EINVAL);
    pscal_fd_close(fd);
}

#define RUN(t)                     \
    do {                           \
        fprintf(stderr, #t "\n");  \
        t();                       \
    } while (0)

int main(void) {
    alarm(30);
    RUN(test_abi);
    RUN(test_fd);
    RUN(test_defaults);
    RUN(test_line_editing_and_echo);
    RUN(test_extended_keys);
    RUN(test_echo_flags);
    RUN(test_translations);
    RUN(test_output_flags);
    RUN(test_end_of_file);
    RUN(test_long_line);
    RUN(test_session_reader_keys);
    RUN(test_signal_keys);
    RUN(test_raw_modes);
    RUN(test_write);
    RUN(test_ioctl);
    RUN(test_winsize_targets);
    RUN(test_sessions);
    RUN(test_numbers);
    RUN(test_hangup);
    RUN(test_waiting);
    RUN(test_poll);
    fprintf(stderr, "tty contract: all passed\n");
    return 0;
}
