/* Forced into libgit2's own sources on iOS, which take none of the vproc shims: what git makes
 * obeys the mask of the task running it. libc's would use the host's, the session shell's. */
#pragma once
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

int vprocHostOpenMasked(const char *path, int flags, ...);
int vprocHostMkdirMasked(const char *path, mode_t mode);
FILE *vprocHostFopenMasked(const char *path, const char *mode);
mode_t vprocUmaskShim(mode_t mask);

#define open vprocHostOpenMasked
#define mkdir(path, mode) vprocHostMkdirMasked((path), (mode))
#define fopen(path, mode) vprocHostFopenMasked((path), (mode))
#define umask(mask) vprocUmaskShim((mask))
