#ifndef PSCAL_IOS_SHIM_H
#define PSCAL_IOS_SHIM_H

#ifdef PSCAL_TARGET_IOS
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>

int pscal_ios_open(const char *path, int oflag, ...);
int pscal_ios_openat(int fd, const char *path, int oflag, ...);
ssize_t pscal_ios_write(int fd, const void *buf, size_t nbyte);
int pscal_ios_close(int fd);
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

#ifndef PSCAL_IOS_SHIM_IMPLEMENTATION
# define open(...) pscal_ios_open(__VA_ARGS__)
# define openat(...) pscal_ios_openat(__VA_ARGS__)
# define write(...) pscal_ios_write(__VA_ARGS__)
# define close(...) pscal_ios_close(__VA_ARGS__)
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
#endif

#endif /* PSCAL_TARGET_IOS */

#endif /* PSCAL_IOS_SHIM_H */
