#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(PSCAL_TARGET_IOS)
#include <dirent.h>
#include <glob.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

int pscalPathVirtualized_chdir(const char *path);
char *pscalPathVirtualized_getcwd(char *buffer, size_t size);
int pscalPathVirtualized_open(const char *path, int oflag, ...);
FILE *pscalPathVirtualized_fopen(const char *path, const char *mode);
FILE *pscalPathVirtualized_freopen(const char *path, const char *mode, FILE *stream);
int pscalPathVirtualized_stat(const char *path, struct stat *buf);
int pscalPathVirtualized_lstat(const char *path, struct stat *buf);
int pscalPathVirtualized_access(const char *path, int mode);
int pscalPathVirtualized_mkdir(const char *path, mode_t mode);
int pscalPathVirtualized_rmdir(const char *path);
int pscalPathVirtualized_unlink(const char *path);
int pscalPathVirtualized_remove(const char *path);
int pscalPathVirtualized_rename(const char *oldpath, const char *newpath);
DIR *pscalPathVirtualized_opendir(const char *name);
int pscalPathVirtualized_glob(const char *pattern,
                              int flags,
                              int (*errfunc)(const char *, int),
                              glob_t *pglob);
int pscalPathVirtualized_symlink(const char *target, const char *linkpath);
ssize_t pscalPathVirtualized_readlink(const char *path, char *buf, size_t size);
char *pscalPathVirtualized_realpath(const char *path, char *resolved_path);

#if !defined(PATH_VIRTUALIZATION_NO_MACROS)
#define chdir(path) pscalPathVirtualized_chdir(path)
#define getcwd(buf, size) pscalPathVirtualized_getcwd(buf, size)
#ifndef open
#define open(...) pscalPathVirtualized_open(__VA_ARGS__)
#endif
#define fopen(path, mode) pscalPathVirtualized_fopen(path, mode)
#define freopen(path, mode, stream) pscalPathVirtualized_freopen(path, mode, stream)
#ifndef stat
#define stat(path, buf) pscalPathVirtualized_stat(path, buf)
#endif
#ifndef lstat
#define lstat(path, buf) pscalPathVirtualized_lstat(path, buf)
#endif
#define access(path, mode) pscalPathVirtualized_access(path, mode)
#define mkdir(path, mode) pscalPathVirtualized_mkdir(path, mode)
#define rmdir(path) pscalPathVirtualized_rmdir(path)
#define unlink(path) pscalPathVirtualized_unlink(path)
#define remove(path) pscalPathVirtualized_remove(path)
#define rename(oldpath, newpath) pscalPathVirtualized_rename(oldpath, newpath)
#define opendir(name) pscalPathVirtualized_opendir(name)
#define symlink(target, linkpath) pscalPathVirtualized_symlink(target, linkpath)
#define readlink(path, buf, size) pscalPathVirtualized_readlink(path, buf, size)
#define realpath(path, resolved_path) pscalPathVirtualized_realpath(path, resolved_path)
#endif /* PATH_VIRTUALIZATION_NO_MACROS */

#endif /* PSCAL_TARGET_IOS */

#ifdef __cplusplus
}
#endif
