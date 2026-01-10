#include "ios/tty/pscal_fd.h"
#include "ios/vproc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

struct pscal_fd *pscal_fd_create(const struct pscal_fd_ops *ops) {
    struct pscal_fd *fd = (struct pscal_fd *)calloc(1, sizeof(struct pscal_fd));
    if (!fd) {
        return NULL;
    }
    atomic_init(&fd->refcount, 1);
    fd->flags = 0;
    fd->ops = ops;
    list_init(&fd->poll_fds);
    list_init(&fd->tty_other_fds);
    fd->tty = NULL;
    lock_init(&fd->lock);
    return fd;
}

struct pscal_fd *pscal_fd_retain(struct pscal_fd *fd) {
    if (!fd) {
        return NULL;
    }
    atomic_fetch_add(&fd->refcount, 1);
    return fd;
}

int pscal_fd_close(struct pscal_fd *fd) {
    if (!fd) {
        return _EBADF;
    }
    if (atomic_fetch_sub(&fd->refcount, 1) != 1) {
        return 0;
    }
    int rc = 0;
    if (fd->ops && fd->ops->close) {
        rc = fd->ops->close(fd);
    }
    free(fd);
    return rc;
}

static int gPollWakePipe[2] = {-1, -1};
static pthread_once_t gPollWakeOnce = PTHREAD_ONCE_INIT;

static void pscalPollInit(void) {
    if (vprocHostPipe(gPollWakePipe) != 0) {
        gPollWakePipe[0] = -1;
        gPollWakePipe[1] = -1;
        return;
    }
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(gPollWakePipe[i], F_GETFL, 0);
        if (flags >= 0) {
            fcntl(gPollWakePipe[i], F_SETFL, flags | O_NONBLOCK);
        }
        int fd_flags = fcntl(gPollWakePipe[i], F_GETFD, 0);
        if (fd_flags >= 0) {
            fcntl(gPollWakePipe[i], F_SETFD, fd_flags | FD_CLOEXEC);
        }
    }
}

int pscalPollWakeFd(void) {
    pthread_once(&gPollWakeOnce, pscalPollInit);
    return gPollWakePipe[0];
}

void pscalPollDrain(void) {
    int fd = pscalPollWakeFd();
    if (fd < 0) {
        return;
    }
    char buf[64];
    while (vprocHostRead(fd, buf, sizeof(buf)) > 0) {
        /* drain */
    }
}

void pscal_fd_poll_wakeup(struct pscal_fd *fd, int UNUSED(events)) {
    (void)fd;
    pthread_once(&gPollWakeOnce, pscalPollInit);
    if (gPollWakePipe[1] < 0) {
        return;
    }
    (void)vprocHostWrite(gPollWakePipe[1], "", 1);
}
