#include "ios/tty/pscal_tty.h"
#include "ios/tty/pscal_tty_host.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

extern struct tty_driver pty_master;
extern struct tty_driver pty_slave;

struct tty_driver *tty_drivers[256] = {0};

/* lock this before locking a tty */
lock_t ttys_lock = LOCK_INITIALIZER;

static int pscalConsoleInit(struct tty *UNUSED(tty)) {
    return 0;
}

static int pscalConsoleWrite(struct tty *UNUSED(tty), const void *UNUSED(buf), size_t len, bool UNUSED(blocking)) {
    return (int)len;
}

static void pscalConsoleCleanup(struct tty *UNUSED(tty)) {
}

static const struct tty_driver_ops pscal_console_ops = {
    .init = pscalConsoleInit,
    .write = pscalConsoleWrite,
    .cleanup = pscalConsoleCleanup,
};

DEFINE_TTY_DRIVER(pscal_console_driver, &pscal_console_ops, TTY_CONSOLE_MAJOR, 64);

typedef struct {
    pid_t_ sid;
    struct tty *tty;
} TtySessionEntry;

static pthread_mutex_t gTtySessionMu = PTHREAD_MUTEX_INITIALIZER;
static TtySessionEntry *gTtySessions = NULL;
static size_t gTtySessionCount = 0;
static size_t gTtySessionCap = 0;

static bool tty_session_has_controlling(pid_t_ sid);

static struct tty *ttySessionPeek(pid_t_ sid) {
    if (sid <= 0) {
        return NULL;
    }
    struct tty *result = NULL;
    pthread_mutex_lock(&gTtySessionMu);
    for (size_t i = 0; i < gTtySessionCount; ++i) {
        if (gTtySessions[i].sid == sid) {
            result = gTtySessions[i].tty;
            break;
        }
    }
    pthread_mutex_unlock(&gTtySessionMu);
    return result;
}

static struct tty *ttySessionRetain(pid_t_ sid) {
    struct tty *tty = ttySessionPeek(sid);
    if (!tty) {
        return NULL;
    }
    lock(&tty->lock);
    tty->refcount++;
    unlock(&tty->lock);
    return tty;
}

static void ttySessionHoldRefLocked(struct tty *tty) {
    if (!tty) {
        return;
    }
    tty->refcount++;
}

static void ttySessionReleaseRef(struct tty *tty) {
    if (!tty) {
        return;
    }
    lock(&ttys_lock);
    tty_release(tty);
    unlock(&ttys_lock);
}

static struct tty *ttySessionSet(pid_t_ sid, struct tty *tty) {
    if (sid <= 0 || !tty) {
        return NULL;
    }
    struct tty *old = NULL;
    pthread_mutex_lock(&gTtySessionMu);
    for (size_t i = 0; i < gTtySessionCount; ++i) {
        if (gTtySessions[i].sid == sid) {
            old = gTtySessions[i].tty;
            gTtySessions[i].tty = tty;
            pthread_mutex_unlock(&gTtySessionMu);
            return old;
        }
    }
    if (gTtySessionCount >= gTtySessionCap) {
        size_t new_cap = gTtySessionCap ? gTtySessionCap * 2 : 8;
        TtySessionEntry *resized = (TtySessionEntry *)realloc(gTtySessions, new_cap * sizeof(TtySessionEntry));
        if (!resized) {
            pthread_mutex_unlock(&gTtySessionMu);
            return NULL;
        }
        gTtySessions = resized;
        gTtySessionCap = new_cap;
    }
    gTtySessions[gTtySessionCount++] = (TtySessionEntry){ .sid = sid, .tty = tty };
    pthread_mutex_unlock(&gTtySessionMu);
    return NULL;
}

static struct tty *ttySessionTake(pid_t_ sid) {
    if (sid <= 0) {
        return NULL;
    }
    struct tty *old = NULL;
    pthread_mutex_lock(&gTtySessionMu);
    for (size_t i = 0; i < gTtySessionCount; ++i) {
        if (gTtySessions[i].sid == sid) {
            old = gTtySessions[i].tty;
            gTtySessions[i] = gTtySessions[gTtySessionCount - 1];
            gTtySessionCount--;
            break;
        }
    }
    pthread_mutex_unlock(&gTtySessionMu);
    return old;
}

static struct tty *ttySessionRemoveIfMatch(pid_t_ sid, struct tty *tty) {
    if (sid <= 0 || !tty) {
        return NULL;
    }
    struct tty *old = NULL;
    pthread_mutex_lock(&gTtySessionMu);
    for (size_t i = 0; i < gTtySessionCount; ++i) {
        if (gTtySessions[i].sid == sid && gTtySessions[i].tty == tty) {
            old = gTtySessions[i].tty;
            gTtySessions[i] = gTtySessions[gTtySessionCount - 1];
            gTtySessionCount--;
            break;
        }
    }
    pthread_mutex_unlock(&gTtySessionMu);
    return old;
}

bool pscalTtyIsControlling(struct tty *tty) {
    if (!tty) {
        return false;
    }
    int sid = pscalTtyCurrentSid();
    if (sid <= 0) {
        return false;
    }
    return ttySessionPeek((pid_t_)sid) == tty;
}

void pscalTtySetControlling(struct tty *tty) {
    if (!tty || tty->session == 0) {
        return;
    }
    if (pscalTtyIsControlling(tty)) {
        return;
    }
    ttySessionHoldRefLocked(tty);
    struct tty *old = ttySessionSet(tty->session, tty);
    if (old && old != tty) {
        ttySessionReleaseRef(old);
    }
}

void pscalTtyClearControlling(struct tty *tty) {
    if (!tty || tty->session == 0) {
        return;
    }
    struct tty *old = ttySessionRemoveIfMatch(tty->session, tty);
    if (old) {
        lock(&old->lock);
        if (old->session == tty->session) {
            old->session = 0;
            old->fg_group = 0;
        }
        unlock(&old->lock);
        ttySessionReleaseRef(old);
    }
}

void pscalTtyDropSession(pid_t_ sid) {
    if (sid <= 0) {
        return;
    }
    struct tty *tty = ttySessionTake(sid);
    if (!tty) {
        return;
    }
    lock(&tty->lock);
    if (tty->session == sid) {
        tty->session = 0;
        tty->fg_group = 0;
    }
    unlock(&tty->lock);
    ttySessionReleaseRef(tty);
}
struct tty *tty_alloc(struct tty_driver *driver, int type, int num) {
    struct tty *tty = calloc(1, sizeof(struct tty));
    if (tty == NULL) {
        return NULL;
    }

    tty->refcount = 0;
    tty->driver = driver;
    tty->type = type;
    tty->num = num;
    tty->hung_up = false;
    tty->ever_opened = false;
    tty->session = 0;
    tty->fg_group = 0;
    list_init(&tty->fds);

    tty->termios.iflags = ICRNL_ | IXON_;
    tty->termios.oflags = OPOST_ | ONLCR_;
    tty->termios.cflags = 0;
    tty->termios.lflags = ISIG_ | ICANON_ | ECHO_ | ECHOE_ | ECHOK_ | ECHOCTL_ | ECHOKE_ | IEXTEN_;
    memcpy(tty->termios.cc, "\003\034\177\025\004\0\1\0\021\023\032\0\022\017\027\026\0\0\0", 19);
    memset(&tty->winsize, 0, sizeof(tty->winsize));

    lock_init(&tty->lock);
    lock_init(&tty->fds_lock);
    cond_init(&tty->produced);
    cond_init(&tty->consumed);
    memset(tty->buf_flag, false, sizeof(tty->buf_flag));
    tty->bufsize = 0;
    tty->packet_flags = 0;

    return tty;
}

struct tty *tty_get(struct tty_driver *driver, int type, int num) {
    lock(&ttys_lock);
    struct tty *tty = driver->ttys[num];
    /* pty_reserve_next stores 1 to avoid races on the same tty */
    if (tty == NULL || tty == (void *)1) {
        tty = tty_alloc(driver, type, num);
        if (tty == NULL) {
            unlock(&ttys_lock);
            return ERR_PTR(_ENOMEM);
        }

        if (driver->ops && driver->ops->init) {
            int err = driver->ops->init(tty);
            if (err < 0) {
                unlock(&ttys_lock);
                free(tty);
                return ERR_PTR(err);
            }
        }
        driver->ttys[num] = tty;
    }
    lock(&tty->lock);
    tty->refcount++;
    tty->ever_opened = true;
    unlock(&tty->lock);
    unlock(&ttys_lock);
    return tty;
}

static struct tty *get_slave_side_tty(struct tty *tty) {
    if (tty->type == TTY_PSEUDO_MASTER_MAJOR) {
        return tty->pty.other;
    }
    return tty;
}

static void tty_poll_wakeup(struct tty *tty, int events) {
    unlock(&tty->lock);
    struct pscal_fd *fd;
    lock(&tty->fds_lock);
    list_for_each_entry(&tty->fds, fd, tty_other_fds) {
        pscal_fd_poll_wakeup(fd, events);
    }
    unlock(&tty->fds_lock);
    lock(&tty->lock);
}

void tty_release(struct tty *tty) {
    lock(&tty->lock);
    if (--tty->refcount == 0) {
        if (tty->session != 0) {
            (void)ttySessionRemoveIfMatch(tty->session, tty);
        }
        tty->session = 0;
        tty->fg_group = 0;
        struct tty_driver *driver = tty->driver;
        if (driver && driver->ops && driver->ops->cleanup) {
            driver->ops->cleanup(tty);
        }
        if (driver && driver->ttys) {
            driver->ttys[tty->num] = NULL;
        }
        unlock(&tty->lock);
        cond_destroy(&tty->produced);
        free(tty);
        return;
    }
    unlock(&tty->lock);
}

/* must call with tty lock */
static void tty_set_controlling(struct tty *tty) {
    int sid = pscalTtyCurrentSid();
    int pgid = pscalTtyCurrentPgid();
    if (sid <= 0) {
        return;
    }
    tty->session = (pid_t_)sid;
    tty->fg_group = (pgid > 0) ? (pid_t_)pgid : (pid_t_)sid;
    pscalTtySetControlling(tty);
    pscalTtySetForegroundPgid(sid, (int)tty->fg_group);
}

int tty_open(struct tty *tty, struct pscal_fd *fd) {
    fd->tty = tty;

    lock(&tty->fds_lock);
    list_add(&tty->fds, &fd->tty_other_fds);
    unlock(&tty->fds_lock);

    if (!(fd->flags & O_NOCTTY)) {
        /* Make this our controlling terminal if:
         * - the terminal doesn't already have a session
         * - we're a session leader
         */
        lock(&tty->lock);
        if (tty->session == 0 &&
            pscalTtyIsSessionLeader() &&
            !tty_session_has_controlling((pid_t_)pscalTtyCurrentSid())) {
            tty_set_controlling(tty);
        }
        unlock(&tty->lock);
    }

    return 0;
}

static int tty_close(struct pscal_fd *fd) {
    if (fd->tty != NULL) {
        struct tty *tty = fd->tty;
        lock(&tty->fds_lock);
        list_remove_safe(&fd->tty_other_fds);
        unlock(&tty->fds_lock);
        lock(&ttys_lock);
        if (tty->driver && tty->driver->ops && tty->driver->ops->close) {
            tty->driver->ops->close(tty);
        }
        tty_release(tty);
        unlock(&ttys_lock);
    }
    return 0;
}

static void tty_input_wakeup(struct tty *tty) {
    notify(&tty->produced);
    tty_poll_wakeup(tty, POLL_READ);
}

static int tty_push_char(struct tty *tty, char ch, bool flag, int blocking) {
    while (tty->bufsize >= sizeof(tty->buf)) {
        if (!blocking) {
            return _EAGAIN;
        }
        if (wait_for(&tty->consumed, &tty->lock, NULL)) {
            return _EINTR;
        }
    }
    tty->buf[tty->bufsize] = ch;
    tty->buf_flag[tty->bufsize++] = flag;
    return 0;
}

static void tty_echo(struct tty *tty, const char *data, size_t size) {
    if (tty->driver && tty->driver->ops && tty->driver->ops->write) {
        bool drop_lock = (tty->driver == &pty_master || tty->driver == &pty_slave);
        if (drop_lock) {
            /* Avoid master/slave lock inversion during echo. */
            unlock(&tty->lock);
        }
        tty->driver->ops->write(tty, data, size, false);
        if (drop_lock) {
            lock(&tty->lock);
        }
    }
}

static bool tty_send_input_signal(struct tty *tty, char ch, sigset_t_ *queue) {
    if (tty->driver == &pty_master) {
        return false;
    }
    if (!(tty->termios.lflags & ISIG_)) {
        return false;
    }
    unsigned char *cc = tty->termios.cc;
    int sig;
    if (ch == '\0') {
        return false;
    } else if (ch == cc[VINTR_]) {
        sig = SIGINT_;
    } else if (ch == cc[VQUIT_]) {
        sig = SIGQUIT_;
    } else if (ch == cc[VSUSP_]) {
        sig = SIGTSTP_;
    } else {
        return false;
    }

    if (tty->fg_group != 0) {
        if (!(tty->termios.lflags & NOFLSH_)) {
            tty->bufsize = 0;
        }
        sigset_add(queue, sig);
    }
    return true;
}

ssize_t tty_input(struct tty *tty, const char *input, size_t size, bool blocking) {
    int err = 0;
    size_t done_size = 0;
    sigset_t_ queue = 0; /* avoid locking tty + task state at once */

    lock(&tty->lock);
    dword_t lflags = tty->termios.lflags;
    dword_t iflags = tty->termios.iflags;
    unsigned char *cc = tty->termios.cc;
    if (tty->driver == &pty_master) {
        lflags &= ~ICANON_;
    }

#define SHOULD_ECHOCTL(ch) \
    (lflags & ECHOCTL_ && \
     ((0 <= (ch) && (ch) < ' ') || (ch) == '\x7f') && \
     !((ch) == '\t' || (ch) == '\n' || (ch) == cc[VSTART_] || (ch) == cc[VSTOP_]))

    if (lflags & ICANON_) {
        for (size_t i = 0; i < size; i++) {
            done_size++;
            char ch = input[i];
            bool echo = lflags & ECHO_;

            if (iflags & INLCR_ && ch == '\n') {
                ch = '\r';
            } else if (iflags & ICRNL_ && ch == '\r') {
                ch = '\n';
            }
            if (iflags & IGNCR_ && ch == '\r') {
                continue;
            }

            if (ch == '\0') {
                /* '\0' is used to disable cc entries */
                goto no_special;
            } else if (ch == cc[VERASE_] || ch == cc[VKILL_]) {
                /*
                 * ECHOKE enables erasing the line instead of echoing the kill
                 * char and outputting a newline (not fully implemented here).
                 */
                echo = lflags & ECHOK_;
                int count = (int)tty->bufsize;
                if (ch == cc[VERASE_] && tty->bufsize > 0) {
                    echo = lflags & ECHOE_;
                    count = 1;
                }
                if (!(lflags & ECHO_)) {
                    echo = false;
                }
                for (int j = 0; j < count; j++) {
                    if (tty->buf_flag[tty->bufsize - 1]) {
                        break;
                    }
                    tty->bufsize--;
                    if (echo) {
                        tty_echo(tty, "\b \b", 3);
                        if (SHOULD_ECHOCTL(tty->buf[tty->bufsize])) {
                            tty_echo(tty, "\b \b", 3);
                        }
                    }
                }
                echo = false;
            } else if (ch == cc[VEOF_]) {
                ch = '\0';
                goto canon_wake;
            } else if (ch == '\n' || ch == cc[VEOL_]) {
                if (echo) {
                    tty_echo(tty, "\r\n", 2);
                }
canon_wake:
                err = tty_push_char(tty, ch, /*flag*/true, blocking);
                if (err < 0) {
                    done_size--;
                    break;
                }
                echo = false;
                tty_input_wakeup(tty);
            } else {
                if (!tty_send_input_signal(tty, ch, &queue)) {
no_special:
                    err = tty_push_char(tty, ch, /*flag*/false, blocking);
                    if (err < 0) {
                        done_size--;
                        break;
                    }
                }
            }

            if (echo) {
                if (SHOULD_ECHOCTL(ch)) {
                    tty_echo(tty, "^", 1);
                    ch ^= '\100';
                }
                tty_echo(tty, &ch, 1);
            }
        }
    } else {
        for (size_t i = 0; i < size; i++) {
            done_size++;
            if (tty_send_input_signal(tty, input[i], &queue)) {
                continue;
            }
            while (tty->bufsize >= sizeof(tty->buf)) {
                err = _EAGAIN;
                if (!blocking) {
                    break;
                }
                err = wait_for(&tty->consumed, &tty->lock, NULL);
                if (err < 0) {
                    break;
                }
            }
            if (err < 0) {
                done_size--;
                break;
            }
            assert(tty->bufsize < sizeof(tty->buf));
            tty->buf[tty->bufsize++] = input[i];
            if (tty->bufsize == 1) {
                tty_input_wakeup(tty);
            }
        }
        if (tty->bufsize > 0) {
            tty_input_wakeup(tty);
        }
    }

    pid_t_ fg_group = tty->fg_group;
    assert(tty->bufsize <= sizeof(tty->buf));
    unlock(&tty->lock);

    if (fg_group != 0) {
        for (int sig = 1; sig < NUM_SIGS; sig++) {
            if (sigset_has(queue, sig)) {
                pscalTtySendGroupSignal((int)fg_group, sig);
            }
        }
    }

    if (done_size > 0) {
        return (ssize_t)done_size;
    }
    return err;
}

/* expects bufsize <= tty->bufsize */
static void tty_read_into_buf(struct tty *tty, void *buf, size_t bufsize) {
    assert(bufsize <= tty->bufsize);
    memcpy(buf, tty->buf, bufsize);
    tty->bufsize -= bufsize;
    memmove(tty->buf, tty->buf + bufsize, tty->bufsize);
    memmove(tty->buf_flag, tty->buf_flag + bufsize, tty->bufsize);
    notify(&tty->consumed);
}

static size_t tty_canon_size(struct tty *tty) {
    bool *flag_ptr = memchr(tty->buf_flag, true, tty->bufsize);
    if (flag_ptr == NULL) {
        return (size_t)-1;
    }
    return (size_t)(flag_ptr - tty->buf_flag) + 1;
}

static bool pty_is_half_closed_master(struct tty *tty) {
    if (tty->driver != &pty_master) {
        return false;
    }

    struct tty *slave = tty->pty.other;
    if (!slave) {
        return false;
    }
    bool locked = lock_try(&slave->lock);
    bool half_closed = slave->ever_opened && (slave->refcount == 1 || slave->hung_up);
    if (locked) {
        unlock(&slave->lock);
    }
    return half_closed;
}

static bool tty_is_current(struct tty *tty) {
    return pscalTtyIsControlling(tty);
}

static int tty_signal_if_background(struct tty *tty, pid_t_ current_pgid, int sig) {
    if (!tty_is_current(tty)) {
        return 0;
    }
    if (tty->fg_group == 0 || current_pgid == tty->fg_group) {
        return 0;
    }

    if (pscalTtySendGroupSignal((int)current_pgid, sig) != 0) {
        return _EIO;
    }
    return _EINTR;
}

static ssize_t tty_read(struct pscal_fd *fd, void *buf, size_t bufsize) {
    if (bufsize == 0) {
        return 0;
    }

    int err = 0;
    struct tty *tty = fd->tty;
    lock(&tty->lock);
    if (tty->hung_up || pty_is_half_closed_master(tty)) {
        unlock(&tty->lock);
        return 0;
    }

    if (tty->driver != &pty_master) {
        pid_t_ current_pgid = (pid_t_)pscalTtyCurrentPgid();
        err = tty_signal_if_background(tty, current_pgid, SIGTTIN_);
        if (err < 0) {
            unlock(&tty->lock);
            return err;
        }
    }

    int bufsize_extra = 0;
    if (tty->driver == &pty_master && tty->pty.packet_mode) {
        char *cbuf = buf;
        *cbuf++ = tty->packet_flags;
        bufsize--;
        bufsize_extra++;
        buf = cbuf;
        if (tty->packet_flags != 0) {
            bufsize = 0;
            goto out;
        }
        if (bufsize == 0) {
            goto out;
        }
    }

    bool canonical = (tty->termios.lflags & ICANON_) && tty->driver != &pty_master;
    if (canonical) {
        size_t canon_size;
        while ((canon_size = tty_canon_size(tty)) == (size_t)-1) {
            err = _EIO;
            if (pty_is_half_closed_master(tty)) {
                goto error;
            }
            err = _EAGAIN;
            if (fd->flags & O_NONBLOCK) {
                goto error;
            }
            err = wait_for(&tty->produced, &tty->lock, NULL);
            if (err < 0) {
                goto error;
            }
        }
        if (tty->buf[canon_size - 1] == '\0') {
            canon_size--;
        }

        if (bufsize > canon_size) {
            bufsize = canon_size;
        }
    } else {
        dword_t min = tty->termios.cc[VMIN_];
        dword_t time = tty->termios.cc[VTIME_];
        if (tty->driver == &pty_master) {
            min = 1;
            time = 0;
        }

        struct timespec timeout;
        timeout.tv_sec = (time / 10);
        timeout.tv_nsec = (long)(time % 10) * 100000000L;
        struct timespec *timeout_ptr = &timeout;
        if (time == 0) {
            timeout_ptr = NULL;
        }

        while (tty->bufsize < min) {
            err = _EIO;
            if (pty_is_half_closed_master(tty)) {
                goto error;
            }
            err = _EAGAIN;
            if (fd->flags & O_NONBLOCK) {
                goto error;
            }
            err = wait_for(&tty->produced, &tty->lock,
                           tty->bufsize == 0 ? NULL : timeout_ptr);
            if (err == _ETIMEDOUT) {
                break;
            }
            if (err == _EINTR) {
                goto error;
            }
        }
    }

    if (bufsize > tty->bufsize) {
        bufsize = tty->bufsize;
    }
    tty_read_into_buf(tty, buf, bufsize);
    if (tty->bufsize > 0 && tty->buf[0] == '\0' && tty->buf_flag[0]) {
        char dummy;
        tty_read_into_buf(tty, &dummy, 1);
    }

out:
    unlock(&tty->lock);
    return (ssize_t)bufsize + bufsize_extra;
error:
    unlock(&tty->lock);
    return err;
}

static ssize_t tty_write(struct pscal_fd *fd, const void *buf, size_t bufsize) {
    struct tty *tty = fd->tty;
    lock(&tty->lock);
    if (tty->hung_up || pty_is_half_closed_master(tty)) {
        unlock(&tty->lock);
        return _EIO;
    }

    bool blocking = !(fd->flags & O_NONBLOCK);
    dword_t oflags = tty->termios.oflags;
    unlock(&tty->lock);

    char *postbuf = NULL;
    size_t postbufsize = bufsize;
    if (oflags & OPOST_) {
        postbuf = malloc(bufsize * 2 + 1);
        if (!postbuf) {
            return _ENOMEM;
        }
        postbufsize = 0;
        const char *cbuf = buf;
        for (size_t i = 0; i < bufsize; i++) {
            char ch = cbuf[i];
            if (ch == '\r' && oflags & ONLRET_) {
                continue;
            } else if (ch == '\r' && oflags & OCRNL_) {
                ch = '\n';
            } else if (ch == '\n' && oflags & ONLCR_) {
                postbuf[postbufsize++] = '\r';
            }
            postbuf[postbufsize++] = ch;
        }
        buf = postbuf;
    }

    ssize_t res = (tty->driver && tty->driver->ops && tty->driver->ops->write)
        ? tty->driver->ops->write(tty, buf, postbufsize, blocking)
        : (ssize_t)postbufsize;
    if (postbuf) {
        free(postbuf);
    }
    if (res < 0) {
        return res;
    }
    return (ssize_t)bufsize;
}

static int tty_poll(struct pscal_fd *fd) {
    struct tty *tty = fd->tty;
    lock(&tty->lock);
    int types = 0;
    types |= POLL_WRITE;
    if (tty->hung_up) {
        types |= POLL_READ | POLL_WRITE | POLL_ERR | POLL_HUP;
    } else if (pty_is_half_closed_master(tty)) {
        types |= POLL_READ | POLL_HUP;
    } else if (tty->termios.lflags & ICANON_) {
        if (tty_canon_size(tty) != (size_t)-1) {
            types |= POLL_READ;
        }
    } else {
        if (tty->bufsize > 0) {
            types |= POLL_READ;
        }
    }
    if (tty->driver == &pty_master && tty->packet_flags != 0) {
        types |= POLL_PRI;
    }
    unlock(&tty->lock);
    return types;
}

static ssize_t tty_ioctl_size(int cmd) {
    switch (cmd) {
        case TCGETS_:
        case TCSETS_:
        case TCSETSF_:
        case TCSETSW_:
            return sizeof(struct termios_);
        case TIOCGWINSZ_:
        case TIOCSWINSZ_:
            return sizeof(struct winsize_);
        case TIOCGPGRP_:
        case TIOCSPGRP_:
        case TIOCSPTLCK_:
        case TIOCGPTN_:
        case TIOCGPTPEER_:
        case TIOCPKT_:
        case TIOCGPKT_:
        case FIONREAD_:
            return sizeof(dword_t);
        case TCFLSH_:
        case TIOCSCTTY_:
            return 0;
    }
    return -1;
}

static bool tty_session_has_controlling(pid_t_ sid) {
    return ttySessionPeek(sid) != NULL;
}

static int tiocsctty(struct tty *tty, int force) {
    int err = 0;
    unlock(&tty->lock);

    int sid = pscalTtyCurrentSid();
    bool leader = pscalTtyIsSessionLeader();
    bool has_ctrl = tty_session_has_controlling((pid_t_)sid);
    bool already_controlling = pscalTtyIsControlling(tty);

    lock(&tty->lock);
    if (sid <= 0 || !leader) {
        return _EPERM;
    }
    if (already_controlling) {
        return 0;
    }
    if (has_ctrl && tty->session != (pid_t_)sid) {
        return _EPERM;
    }
    if (tty->session && tty->session != (pid_t_)sid) {
        if (!force) {
            return _EPERM;
        }
    }

    if (force && has_ctrl) {
        struct tty *old = ttySessionTake((pid_t_)sid);
        if (old && old != tty) {
            lock(&old->lock);
            if (old->session == (pid_t_)sid) {
                old->session = 0;
                old->fg_group = 0;
            }
            unlock(&old->lock);
            ttySessionReleaseRef(old);
        }
    }

    tty_set_controlling(tty);
    return err;
}

static int tiocgpgrp(struct tty *tty, pid_t_ *fg_group) {
    int err = 0;
    struct tty *slave = get_slave_side_tty(tty);
    if (!slave) {
        return _ENOTTY;
    }
    if (slave != tty) {
        lock(&slave->lock);
    }

    if (tty == slave && (!tty_is_current(slave) || slave->fg_group == 0)) {
        err = _ENOTTY;
        goto out_no_ctrl;
    }
    *fg_group = slave->fg_group;

out_no_ctrl:
    if (slave != tty) {
        unlock(&slave->lock);
    }
    return err;
}

/* operate on slave side of a pseudoterminal even if master is specified */
static int tty_mode_ioctl(struct tty *in_tty, int cmd, void *arg) {
    int err = 0;
    struct tty *tty = in_tty;
    if (in_tty->driver == &pty_master) {
        tty = in_tty->pty.other;
        if (!tty) {
            return _ENOTTY;
        }
        lock(&tty->lock);
    }

    switch (cmd) {
        case TCGETS_:
            *(struct termios_ *)arg = tty->termios;
            break;
        case TCSETSF_:
            tty->bufsize = 0;
            notify(&tty->consumed);
            /* fallthrough */
        case TCSETSW_:
        case TCSETS_:
            tty->termios = *(struct termios_ *)arg;
            break;
        case TIOCGWINSZ_:
            *(struct winsize_ *)arg = tty->winsize;
            break;
        case TIOCSWINSZ_:
            tty_set_winsize(tty, *(struct winsize_ *)arg);
            break;
        default:
            err = _ENOTTY;
            break;
    }

    if (in_tty->driver == &pty_master) {
        unlock(&tty->lock);
    }
    return err;
}

static int tty_ioctl(struct pscal_fd *fd, int cmd, void *arg) {
    int err = 0;
    struct tty *tty = fd->tty;
    lock(&tty->lock);
    if (tty->hung_up) {
        unlock(&tty->lock);
        if (cmd == TIOCSPGRP_) {
            return _ENOTTY;
        }
        return _EIO;
    }

    switch (cmd) {
        case TCFLSH_:
            switch ((uintptr_t)arg) {
                case TCIFLUSH_:
                case TCIOFLUSH_:
                    tty->bufsize = 0;
                    notify(&tty->consumed);
                    break;
                case TCOFLUSH_:
                    break;
                default:
                    err = _EINVAL;
                    break;
            }
            break;
        case TIOCSCTTY_:
            err = tiocsctty(tty, (uintptr_t)arg);
            break;
        case TIOCGPGRP_:
            err = tiocgpgrp(tty, (pid_t_ *)arg);
            break;
        case TIOCSPGRP_:
            if (!tty_is_current(tty) || (pid_t_)pscalTtyCurrentSid() != tty->session) {
                err = _ENOTTY;
                break;
            }
            tty->fg_group = *(dword_t *)arg;
            pscalTtySetForegroundPgid((int)tty->session, (int)tty->fg_group);
            break;
        case FIONREAD_:
            *(dword_t *)arg = (dword_t)tty->bufsize;
            break;
        default:
            err = tty_mode_ioctl(tty, cmd, arg);
            if (err == _ENOTTY && tty->driver && tty->driver->ops && tty->driver->ops->ioctl) {
                err = tty->driver->ops->ioctl(tty, cmd, arg);
            }
            break;
    }

    unlock(&tty->lock);
    return err;
}

void tty_set_winsize(struct tty *tty, struct winsize_ winsize) {
    tty->winsize = winsize;
    if (tty->fg_group != 0) {
        pscalTtySendGroupSignal((int)tty->fg_group, SIGWINCH_);
    }
}

void tty_hangup(struct tty *tty) {
    tty->hung_up = true;
    tty_input_wakeup(tty);
}

int pscalTtyOpenControlling(int flags, struct pscal_fd **out_fd) {
    if (!out_fd) {
        return _EINVAL;
    }
    *out_fd = NULL;
    int sid = pscalTtyCurrentSid();
    if (sid <= 0) {
        return _ENXIO;
    }

    struct tty *found = ttySessionRetain((pid_t_)sid);

    if (!found) {
        return _ENXIO;
    }

    struct pscal_fd *fd = pscal_fd_create(&pscal_tty_fd_ops);
    if (!fd) {
        lock(&ttys_lock);
        tty_release(found);
        unlock(&ttys_lock);
        return _ENOMEM;
    }
    fd->flags = (unsigned)flags;
    int err = tty_open(found, fd);
    if (err < 0) {
        pscal_fd_close(fd);
        lock(&ttys_lock);
        tty_release(found);
        unlock(&ttys_lock);
        return err;
    }
    *out_fd = fd;
    return 0;
}

const struct pscal_fd_ops pscal_tty_fd_ops = {
    .read = tty_read,
    .write = tty_write,
    .poll = tty_poll,
    .ioctl_size = tty_ioctl_size,
    .ioctl = tty_ioctl,
    .close = tty_close,
};
