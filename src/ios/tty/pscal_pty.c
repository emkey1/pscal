#include "ios/tty/pscal_pty.h"
#include "ios/tty/pscal_fd.h"
#include "ios/vproc.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "common/path_truncate.h"

extern struct tty_driver pty_master;
extern struct tty_driver pty_slave;

#define MAX_PTYS (1 << 12)

extern int pscalHostOpenRaw(const char *path, int flags, mode_t mode);

static int pscalHostMkdirRaw(const char *path, mode_t mode) {
    vprocInterposeBypassEnter();
    int res = mkdir(path, mode);
    vprocInterposeBypassExit();
    return res;
}

static int pscalHostUnlinkRaw(const char *path) {
    vprocInterposeBypassEnter();
    int res = unlink(path);
    vprocInterposeBypassExit();
    return res;
}

static int pscalHostChmodRaw(const char *path, mode_t mode) {
    vprocInterposeBypassEnter();
    int res = chmod(path, mode);
    vprocInterposeBypassExit();
    return res;
}

static int pscalHostChownRaw(const char *path, uid_t uid, gid_t gid) {
    vprocInterposeBypassEnter();
    int res = chown(path, uid, gid);
    vprocInterposeBypassExit();
    return res;
}

static void pscalPtyEnsureDevptsRoot(void) {
    if (!pathTruncateEnabled()) {
        return;
    }
    char pts_dir[PATH_MAX];
    if (!pathTruncateExpand("/dev/pts", pts_dir, sizeof(pts_dir))) {
        return;
    }
    if (pscalHostMkdirRaw(pts_dir, 0755) != 0 && errno != EEXIST) {
        return;
    }
    char ptmx_path[PATH_MAX];
    if (!pathTruncateExpand("/dev/pts/ptmx", ptmx_path, sizeof(ptmx_path))) {
        return;
    }
    int fd = pscalHostOpenRaw(ptmx_path, O_CREAT | O_EXCL | O_RDONLY, 0666);
    if (fd >= 0) {
        vprocHostClose(fd);
    }
    if (fd >= 0 || errno == EEXIST) {
        (void)pscalHostChmodRaw(ptmx_path, 0666);
    }
}

static void pscalPtySyncDevptsEntry(int pty_num, mode_t_ perms, uid_t_ uid, gid_t_ gid) {
    if (!pathTruncateEnabled()) {
        return;
    }
    char rel_path[64];
    snprintf(rel_path, sizeof(rel_path), "/dev/pts/%d", pty_num);
    char node_path[PATH_MAX];
    if (!pathTruncateExpand(rel_path, node_path, sizeof(node_path))) {
        return;
    }
    (void)pscalHostChmodRaw(node_path, (mode_t)(perms & 0777));
    (void)pscalHostChownRaw(node_path, (uid_t)uid, (gid_t)gid);
}

static void pscalPtyEnsureDevptsEntry(int pty_num) {
    if (!pathTruncateEnabled()) {
        return;
    }
    pscalPtyEnsureDevptsRoot();
    char pts_dir[PATH_MAX];
    if (!pathTruncateExpand("/dev/pts", pts_dir, sizeof(pts_dir))) {
        return;
    }
    if (pscalHostMkdirRaw(pts_dir, 0755) != 0 && errno != EEXIST) {
        return;
    }
    char rel_path[64];
    snprintf(rel_path, sizeof(rel_path), "/dev/pts/%d", pty_num);
    char node_path[PATH_MAX];
    if (!pathTruncateExpand(rel_path, node_path, sizeof(node_path))) {
        return;
    }
    int fd = pscalHostOpenRaw(node_path, O_CREAT | O_EXCL | O_RDONLY, 0620);
    if (fd >= 0) {
        vprocHostClose(fd);
    }
    if (fd >= 0 || errno == EEXIST) {
        (void)pscalHostChmodRaw(node_path, 0620);
    }
}

static void pscalPtyRemoveDevptsEntry(int pty_num) {
    if (!pathTruncateEnabled()) {
        return;
    }
    char rel_path[64];
    snprintf(rel_path, sizeof(rel_path), "/dev/pts/%d", pty_num);
    char node_path[PATH_MAX];
    if (!pathTruncateExpand(rel_path, node_path, sizeof(node_path))) {
        return;
    }
    pscalHostUnlinkRaw(node_path);
}

static void pty_slave_init_inode(struct tty *tty) {
    tty->pty.uid = (uid_t_)geteuid();
    tty->pty.gid = (gid_t_)getegid();
    tty->pty.perms = 0620;
}

static int pty_master_init(struct tty *tty) {
    tty->termios.iflags = 0;
    tty->termios.oflags = 0;
    tty->termios.lflags = 0;

    struct tty *slave = tty_alloc(&pty_slave, TTY_PSEUDO_SLAVE_MAJOR, tty->num);
    if (!slave) {
        return _ENOMEM;
    }
    slave->refcount = 1;
    pty_slave.ttys[tty->num] = slave;
    tty->pty.other = slave;
    slave->pty.other = tty;
    slave->pty.locked = true;
    pty_slave_init_inode(slave);
    pscalPtyEnsureDevptsEntry(tty->num);
    pscalPtySyncDevptsEntry(tty->num, slave->pty.perms, slave->pty.uid, slave->pty.gid);
    return 0;
}

static void pty_hangup(struct tty *tty) {
    if (!tty) {
        return;
    }
    lock(&tty->lock);
    tty_hangup(tty);
    unlock(&tty->lock);
}

static struct tty *pty_hangup_other(struct tty *tty) {
    struct tty *other = tty->pty.other;
    if (!other) {
        return NULL;
    }
    pty_hangup(other);
    return other;
}

static void pty_slave_cleanup(struct tty *tty) {
    pty_hangup_other(tty);
}

static void pty_master_cleanup(struct tty *tty) {
    struct tty *slave = pty_hangup_other(tty);
    if (slave) {
        slave->pty.other = NULL;
        tty_release(slave);
    }
    pscalPtyRemoveDevptsEntry(tty->num);
}

static int pty_slave_open(struct tty *tty) {
    if (tty->pty.other == NULL) {
        return _EIO;
    }
    if (tty->pty.locked) {
        return _EIO;
    }
    return 0;
}

static int pty_slave_close(struct tty *tty) {
    if (tty->refcount - 1 == (tty->session ? 2 : 1)) {
        pty_hangup_other(tty);
    }
    return 0;
}

static int pty_master_ioctl(struct tty *tty, int cmd, void *arg) {
    struct tty *slave = tty->pty.other;
    switch (cmd) {
        case TIOCSPTLCK_:
            if (slave) {
                slave->pty.locked = !!*(dword_t *)arg;
            }
            break;
        case TIOCGPTN_:
            if (slave) {
                *(dword_t *)arg = slave->num;
            }
            break;
        case TIOCPKT_:
            tty->pty.packet_mode = !!*(dword_t *)arg;
            break;
        case TIOCGPKT_:
            *(dword_t *)arg = tty->pty.packet_mode;
            break;
        default:
            return _ENOTTY;
    }
    return 0;
}

static int pty_write(struct tty *tty, const void *buf, size_t len, bool blocking) {
    return (int)tty_input(tty->pty.other, buf, len, blocking);
}

static int pty_return_eio(struct tty *UNUSED(tty)) {
    return _EIO;
}

static const struct tty_driver_ops pty_master_ops = {
    .init = pty_master_init,
    .open = pty_return_eio,
    .write = pty_write,
    .ioctl = pty_master_ioctl,
    .cleanup = pty_master_cleanup,
};

DEFINE_TTY_DRIVER(pty_master, &pty_master_ops, TTY_PSEUDO_MASTER_MAJOR, MAX_PTYS);

static const struct tty_driver_ops pty_slave_ops = {
    .init = pty_return_eio,
    .open = pty_slave_open,
    .close = pty_slave_close,
    .write = pty_write,
    .cleanup = pty_slave_cleanup,
};

DEFINE_TTY_DRIVER(pty_slave, &pty_slave_ops, TTY_PSEUDO_SLAVE_MAJOR, MAX_PTYS);

static pthread_once_t g_pty_init_once = PTHREAD_ONCE_INIT;

static void pscalPtyInit(void) {
    tty_drivers[TTY_PSEUDO_MASTER_MAJOR] = &pty_master;
    tty_drivers[TTY_PSEUDO_SLAVE_MAJOR] = &pty_slave;
}

static int pty_reserve_next(void) {
    int pty_num;
    lock(&ttys_lock);
    for (pty_num = 0; pty_num < MAX_PTYS; pty_num++) {
        if (pty_slave.ttys[pty_num] == NULL) {
            break;
        }
    }
    if (pty_num < MAX_PTYS) {
        pty_slave.ttys[pty_num] = (void *)1;
    }
    unlock(&ttys_lock);
    return pty_num;
}

static struct pscal_fd *pscalPtyOpenTty(struct tty *tty, int flags) {
    struct pscal_fd *fd = pscal_fd_create(&pscal_tty_fd_ops);
    if (!fd) {
        return NULL;
    }
    fd->flags = (unsigned)flags;
    if (tty_open(tty, fd) != 0) {
        pscal_fd_close(fd);
        return NULL;
    }
    return fd;
}

int pscalPtyOpenMaster(int flags, struct pscal_fd **out_master, int *out_pty_num) {
    if (!out_master || !out_pty_num) {
        return _EINVAL;
    }
    pthread_once(&g_pty_init_once, pscalPtyInit);

    int pty_num = pty_reserve_next();
    if (pty_num >= MAX_PTYS) {
        return _ENOSPC;
    }
    struct tty *master = tty_get(&pty_master, TTY_PSEUDO_MASTER_MAJOR, pty_num);
    if (IS_ERR(master)) {
        return (int)PTR_ERR(master);
    }

    struct pscal_fd *fd = pscalPtyOpenTty(master, flags);
    if (!fd) {
        lock(&ttys_lock);
        tty_release(master);
        unlock(&ttys_lock);
        return _ENOMEM;
    }

    *out_master = fd;
    *out_pty_num = pty_num;
    return 0;
}

int pscalPtyOpenSlave(int pty_num, int flags, struct pscal_fd **out_slave) {
    if (!out_slave) {
        return _EINVAL;
    }
    pthread_once(&g_pty_init_once, pscalPtyInit);
    if (pty_num < 0 || pty_num >= MAX_PTYS) {
        return _ENXIO;
    }

    lock(&ttys_lock);
    struct tty *tty = pty_slave.ttys[pty_num];
    if (tty == NULL || tty == (void *)1) {
        unlock(&ttys_lock);
        return _ENXIO;
    }
    lock(&tty->lock);
    tty->refcount++;
    unlock(&tty->lock);
    unlock(&ttys_lock);

    if (tty->driver && tty->driver->ops && tty->driver->ops->open) {
        int err = tty->driver->ops->open(tty);
        if (err < 0) {
            lock(&ttys_lock);
            tty_release(tty);
            unlock(&ttys_lock);
            return err;
        }
    }

    struct pscal_fd *fd = pscalPtyOpenTty(tty, flags);
    if (!fd) {
        lock(&ttys_lock);
        tty_release(tty);
        unlock(&ttys_lock);
        return _ENOMEM;
    }

    *out_slave = fd;
    return 0;
}

bool pscalPtyIsMaster(struct pscal_fd *fd) {
    if (!fd || !fd->tty) {
        return false;
    }
    return fd->tty->driver == &pty_master;
}

bool pscalPtyIsSlave(struct pscal_fd *fd) {
    if (!fd || !fd->tty) {
        return false;
    }
    return fd->tty->driver == &pty_slave;
}

int pscalPtyGetSlaveInfo(int pty_num, mode_t_ *perms, uid_t_ *uid, gid_t_ *gid) {
    if (pty_num < 0 || pty_num >= MAX_PTYS) {
        return -1;
    }
    lock(&ttys_lock);
    struct tty *tty = pty_slave.ttys[pty_num];
    if (!tty || tty == (void *)1) {
        unlock(&ttys_lock);
        return -1;
    }
    lock(&tty->lock);
    if (perms) {
        *perms = tty->pty.perms;
    }
    if (uid) {
        *uid = tty->pty.uid;
    }
    if (gid) {
        *gid = tty->pty.gid;
    }
    unlock(&tty->lock);
    unlock(&ttys_lock);
    return 0;
}

int pscalPtySetSlaveInfo(int pty_num, const mode_t_ *perms, const uid_t_ *uid, const gid_t_ *gid) {
    if (pty_num < 0 || pty_num >= MAX_PTYS) {
        return _ENOENT;
    }
    mode_t_ updated_perms = 0;
    uid_t_ updated_uid = 0;
    gid_t_ updated_gid = 0;
    lock(&ttys_lock);
    struct tty *tty = pty_slave.ttys[pty_num];
    if (!tty || tty == (void *)1) {
        unlock(&ttys_lock);
        return _ENOENT;
    }
    lock(&tty->lock);
    if (perms) {
        tty->pty.perms = *perms;
    }
    if (uid) {
        tty->pty.uid = *uid;
    }
    if (gid) {
        tty->pty.gid = *gid;
    }
    updated_perms = tty->pty.perms;
    updated_uid = tty->pty.uid;
    updated_gid = tty->pty.gid;
    unlock(&tty->lock);
    unlock(&ttys_lock);

    pscalPtyEnsureDevptsEntry(pty_num);
    pscalPtySyncDevptsEntry(pty_num, updated_perms, updated_uid, updated_gid);
    return 0;
}

int pscalPtyUnlock(struct pscal_fd *master) {
    if (!master || !master->ops || !master->ops->ioctl) {
        return _EINVAL;
    }
    dword_t unlock = 0;
    return master->ops->ioctl(master, TIOCSPTLCK_, &unlock);
}
