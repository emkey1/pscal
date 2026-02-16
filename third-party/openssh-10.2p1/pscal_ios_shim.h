#ifndef PSCAL_IOS_SHIM_H
#define PSCAL_IOS_SHIM_H

#ifdef PSCAL_TARGET_IOS
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <pthread.h>
#include <setjmp.h>
#include "ios/tty/pscal_fd.h"
#include "ios/vproc.h"

#ifndef vprocSetCompatErrno
static inline int pscal_ios_vproc_set_compat_errno(int err) {
    errno = pscalCompatErrno(err);
    return -1;
}
#define vprocSetCompatErrno pscal_ios_vproc_set_compat_errno
#endif

int pscal_ios_open(const char *path, int oflag, ...);
int pscal_ios_openat(int fd, const char *path, int oflag, ...);
ssize_t pscal_ios_read(int fd, void *buf, size_t nbyte);
ssize_t pscal_ios_write(int fd, const void *buf, size_t nbyte);
int pscal_ios_fprintf(FILE *stream, const char *format, ...);
int pscal_ios_printf(const char *format, ...);
int pscal_ios_fputs(const char *s, FILE *stream);
int pscal_ios_puts(const char *s);
int pscal_ios_close(int fd);
int pscal_ios_dup(int fd);
int pscal_ios_dup2(int fd, int target);
int pscal_ios_fcntl(int fd, int cmd, ...);
void pscal_ios_closefrom(int lowfd);
int pscal_ios_ioctl(int fd, unsigned long request, ...);
int pscal_ios_tcgetattr(int fd, struct termios *termios_p);
int pscal_ios_tcsetattr(int fd, int optional_actions,
    const struct termios *termios_p);
int pscal_ios_isatty(int fd);
int pscal_ios_stat(const char *path, struct stat *buf);
int pscal_ios_lstat(const char *path, struct stat *buf);
int pscal_ios_access(const char *path, int mode);
int pscal_ios_faccessat(int fd, const char *path, int mode, int flag);
FILE *pscal_ios_fopen(const char *path, const char *mode);
DIR *pscal_ios_opendir(const char *path);
int pscal_ios_mkdir(const char *path, mode_t mode);
int pscal_ios_rmdir(const char *path);
int pscal_ios_unlink(const char *path);
int pscal_ios_remove(const char *path);
int pscal_ios_rename(const char *oldpath, const char *newpath);
int pscal_ios_link(const char *target, const char *linkpath);
int pscal_ios_symlink(const char *target, const char *linkpath);
char *pscal_ios_realpath(const char *path, char *resolved_path);

extern __thread sigjmp_buf pscal_ios_fork_jmpbuf;
pid_t pscal_ios_fork_dispatch(int jump_rc);
int pscal_ios_execv(const char *path, char *const argv[]);
int pscal_ios_execvp(const char *file, char *const argv[]);
int pscal_ios_execl(const char *path, const char *arg, ...);
int pscal_ios_execle(const char *path, const char *arg, ...);
int pscal_ios_execlp(const char *file, const char *arg, ...);

#ifndef PSCAL_IOS_SHIM_IMPLEMENTATION
# define fork() pscal_ios_fork_dispatch(sigsetjmp(pscal_ios_fork_jmpbuf, 1))
# define execv(...) pscal_ios_execv(__VA_ARGS__)
# define execvp(...) pscal_ios_execvp(__VA_ARGS__)
# define execl(...) pscal_ios_execl(__VA_ARGS__)
# define execle(...) pscal_ios_execle(__VA_ARGS__)
# define execlp(...) pscal_ios_execlp(__VA_ARGS__)

# define read(...) pscal_ios_read(__VA_ARGS__)
# define open(...) pscal_ios_open(__VA_ARGS__)
# define openat(...) pscal_ios_openat(__VA_ARGS__)
# define write(...) pscal_ios_write(__VA_ARGS__)
# define fprintf(...) pscal_ios_fprintf(__VA_ARGS__)
# define printf(...) pscal_ios_printf(__VA_ARGS__)
# define fputs(...) pscal_ios_fputs(__VA_ARGS__)
# define puts(...) pscal_ios_puts(__VA_ARGS__)
# define close(...) pscal_ios_close(__VA_ARGS__)
# define pipe(...) vprocPipeShim(__VA_ARGS__)
# define socket(...) vprocSocketShim(__VA_ARGS__)
# define socketpair(...) vprocSocketpairShim(__VA_ARGS__)
# define dup(...) pscal_ios_dup(__VA_ARGS__)
# define dup2(...) pscal_ios_dup2(__VA_ARGS__)
# define fcntl(...) pscal_ios_fcntl(__VA_ARGS__)
# define closefrom(...) pscal_ios_closefrom(__VA_ARGS__)
# define ioctl(...) pscal_ios_ioctl(__VA_ARGS__)
# define tcgetattr(fd, termios_p) pscal_ios_tcgetattr((fd), (termios_p))
# define tcsetattr(fd, opt, termios_p) \
    pscal_ios_tcsetattr((fd), (opt), (termios_p))
# define isatty(fd) pscal_ios_isatty((fd))
# define stat(path, buf) pscal_ios_stat((path), (buf))
# define lstat(path, buf) pscal_ios_lstat((path), (buf))
# define access(path, mode) pscal_ios_access((path), (mode))
# define faccessat(fd, path, mode, flag) \
    pscal_ios_faccessat((fd), (path), (mode), (flag))
# define fopen(path, mode) pscal_ios_fopen((path), (mode))
# define opendir(path) pscal_ios_opendir((path))
# define mkdir(path, mode) pscal_ios_mkdir((path), (mode))
# define rmdir(path) pscal_ios_rmdir((path))
# define unlink(path) pscal_ios_unlink((path))
# define remove(path) pscal_ios_remove((path))
# define rename(oldpath, newpath) pscal_ios_rename((oldpath), (newpath))
# define link(target, linkpath) pscal_ios_link((target), (linkpath))
# define symlink(target, linkpath) pscal_ios_symlink((target), (linkpath))
# define pthread_create(thread, attr, start_routine, arg) \
    vprocPthreadCreateShim((thread), (attr), (start_routine), (arg))
#endif

#endif /* PSCAL_TARGET_IOS */

#endif /* PSCAL_IOS_SHIM_H */
