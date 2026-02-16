#pragma once

#include "ios/tty/ish_compat.h"
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pscal_fd;

struct tty;

struct pscal_fd_ops {
    ssize_t (*read)(struct pscal_fd *fd, void *buf, size_t bufsize);
    ssize_t (*write)(struct pscal_fd *fd, const void *buf, size_t bufsize);
    int (*poll)(struct pscal_fd *fd);
    ssize_t (*ioctl_size)(int cmd);
    int (*ioctl)(struct pscal_fd *fd, int cmd, void *arg);
    int (*close)(struct pscal_fd *fd);
};

struct pscal_fd {
    atomic_uint refcount;
    unsigned flags;
    const struct pscal_fd_ops *ops;
    struct list poll_fds;
    struct list tty_other_fds;
    struct tty *tty;
    lock_t lock;
    void *userdata;
};

struct pscal_fd *pscal_fd_create(const struct pscal_fd_ops *ops);
struct pscal_fd *pscal_fd_retain(struct pscal_fd *fd);
int pscal_fd_close(struct pscal_fd *fd);

int pscalPollWakeFd(void);
void pscalPollDrain(void);
void pscal_fd_poll_wakeup(struct pscal_fd *fd, int events);

#ifdef __cplusplus
}
#endif
