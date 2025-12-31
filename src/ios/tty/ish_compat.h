#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal iSH compatibility layer for tty/pty port. */

#ifdef POLL_PRI
#undef POLL_PRI
#endif
#ifdef POLL_ERR
#undef POLL_ERR
#endif
#ifdef POLL_HUP
#undef POLL_HUP
#endif
#ifdef POLL_NVAL
#undef POLL_NVAL
#endif

#define UNUSED(x) UNUSED_##x __attribute__((unused))

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define zero_init(type) ((type[1]){}[0])

/* Error pointer helpers (iSH-style). */
#define ERR_PTR(err) ((void *)(intptr_t)(err))
#define PTR_ERR(ptr) ((intptr_t)(ptr))
#define IS_ERR(ptr) ((uintptr_t)(ptr) > (uintptr_t)-0xfff)

/* iSH-style types. */
typedef int64_t sqword_t;
typedef uint64_t qword_t;
typedef uint32_t dword_t;
typedef int32_t sdword_t;
typedef uint16_t word_t;
typedef uint8_t byte_t;

typedef sdword_t pid_t_;
typedef dword_t uid_t_;
typedef dword_t gid_t_;
typedef word_t mode_t_;
typedef sqword_t off_t_;
typedef qword_t sigset_t_;

/* Negative errno codes (subset). */
#define _EPERM          -1
#define _ENOENT         -2
#define _ESRCH          -3
#define _EINTR          -4
#define _EIO            -5
#define _ENXIO          -6
#define _EBADF          -9
#define _EAGAIN         -11
#define _ENOMEM         -12
#define _EACCES         -13
#define _EFAULT         -14
#define _EBUSY          -16
#define _EEXIST         -17
#define _ENODEV         -19
#define _ENOTDIR        -20
#define _EISDIR         -21
#define _EINVAL         -22
#define _EMFILE         -24
#define _ENOTTY         -25
#define _EPIPE          -32
#define _ENOSPC         -28
#define _ENOTEMPTY      -39
#define _ENOTSUP        -95
#define _ETIMEDOUT      -110

/* Poll/event bits (iSH-style). */
#define POLL_READ 1
#define POLL_PRI 2
#define POLL_WRITE 4
#define POLL_ERR 8
#define POLL_HUP 16
#define POLL_NVAL 32

/* Signal aliases used by iSH tty. */
#define SIGINT_ SIGINT
#define SIGQUIT_ SIGQUIT
#define SIGTSTP_ SIGTSTP
#define SIGTTIN_ SIGTTIN
#define SIGTTOU_ SIGTTOU
#define SIGWINCH_ SIGWINCH

#define NUM_SIGS 64

static inline sigset_t_ sig_mask(int sig) {
    assert(sig >= 1 && sig < NUM_SIGS);
    return 1ULL << (sig - 1);
}

static inline bool sigset_has(sigset_t_ set, int sig) {
    return (set & sig_mask(sig)) != 0;
}

static inline void sigset_add(sigset_t_ *set, int sig) {
    *set |= sig_mask(sig);
}

/* Simple doubly-linked list. */
struct list {
    struct list *next;
    struct list *prev;
};

static inline void list_init(struct list *list) {
    list->next = list;
    list->prev = list;
}

static inline bool list_null(struct list *list) {
    return list->next == NULL && list->prev == NULL;
}

static inline bool list_empty(struct list *list) {
    return list->next == list || list_null(list);
}

static inline void list_add_between(struct list *prev, struct list *next, struct list *item) {
    prev->next = item;
    item->prev = prev;
    item->next = next;
    next->prev = item;
}

static inline void list_add(struct list *list, struct list *item) {
    list_add_between(list, list->next, item);
}

static inline void list_add_tail(struct list *list, struct list *item) {
    list_add_between(list->prev, list, item);
}

static inline void list_remove(struct list *item) {
    item->prev->next = item->next;
    item->next->prev = item->prev;
    item->next = NULL;
    item->prev = NULL;
}

static inline void list_remove_safe(struct list *item) {
    if (!list_null(item)) {
        list_remove(item);
    }
}

#define list_entry(item, type, member) \
    container_of(item, type, member)

#define list_for_each_entry(list, item, member) \
    for (item = list_entry((list)->next, typeof(*item), member); \
         &item->member != (list); \
         item = list_entry(item->member.next, typeof(*item), member))

/* Lightweight mutex/cond wrappers. */
typedef struct {
    pthread_mutex_t m;
} lock_t;

#define LOCK_INITIALIZER {PTHREAD_MUTEX_INITIALIZER}

static inline void lock_init(lock_t *lock) {
    pthread_mutex_init(&lock->m, NULL);
}

static inline void lock(lock_t *lock) {
    pthread_mutex_lock(&lock->m);
}

static inline bool lock_try(lock_t *lock) {
    return pthread_mutex_trylock(&lock->m) == 0;
}

static inline void unlock(lock_t *lock) {
    pthread_mutex_unlock(&lock->m);
}

typedef struct {
    pthread_cond_t cond;
} cond_t;

void cond_init(cond_t *cond);
void cond_destroy(cond_t *cond);
int wait_for(cond_t *cond, lock_t *lock, struct timespec *timeout);
void notify(cond_t *cond);

int pscalCompatErrno(int err);

#ifdef __cplusplus
}
#endif
