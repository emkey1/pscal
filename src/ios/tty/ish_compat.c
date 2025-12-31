#include "ios/tty/ish_compat.h"

void cond_init(cond_t *cond) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_cond_init(&cond->cond, &attr);
}

void cond_destroy(cond_t *cond) {
    pthread_cond_destroy(&cond->cond);
}

static int cond_wait_internal(cond_t *cond, lock_t *lock, struct timespec *timeout) {
    if (!timeout) {
        return pthread_cond_wait(&cond->cond, &lock->m);
    }
    struct timespec abs_timeout;
    if (clock_gettime(CLOCK_REALTIME, &abs_timeout) != 0) {
        return pthread_cond_wait(&cond->cond, &lock->m);
    }
    abs_timeout.tv_sec += timeout->tv_sec;
    abs_timeout.tv_nsec += timeout->tv_nsec;
    if (abs_timeout.tv_nsec >= 1000000000L) {
        abs_timeout.tv_sec += 1;
        abs_timeout.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(&cond->cond, &lock->m, &abs_timeout);
}

int wait_for(cond_t *cond, lock_t *lock, struct timespec *timeout) {
    int rc = cond_wait_internal(cond, lock, timeout);
    if (rc == ETIMEDOUT) {
        return _ETIMEDOUT;
    }
    return 0;
}

void notify(cond_t *cond) {
    pthread_cond_broadcast(&cond->cond);
}

int pscalCompatErrno(int err) {
    if (err >= 0) {
        return 0;
    }
    switch (err) {
        case _EAGAIN:
            return EAGAIN;
        case _EINTR:
            return EINTR;
        case _EIO:
            return EIO;
        case _ENOTTY:
            return ENOTTY;
        case _EINVAL:
            return EINVAL;
        case _EPERM:
            return EPERM;
        case _ENOMEM:
            return ENOMEM;
        case _ENXIO:
            return ENXIO;
        case _ENOSPC:
            return ENOSPC;
        case _EPIPE:
            return EPIPE;
        case _ENOTSUP:
            return ENOTSUP;
        case _ETIMEDOUT:
            return ETIMEDOUT;
        case _EACCES:
            return EACCES;
        case _EBADF:
            return EBADF;
        default:
            return EIO;
    }
}
