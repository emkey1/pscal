#include <stddef.h>
#include <errno.h>

typedef struct PathTruncateSmallclueApplet {
    const char *name;
    int (*entry)(int argc, char **argv);
    const char *description;
} PathTruncateSmallclueApplet;

/*
 * Unit tests in this directory link a small subset of runtime sources directly.
 * Provide a local stub so path_truncate.c can probe applet metadata without
 * pulling the full smallclue object set into each test binary.
 */
const PathTruncateSmallclueApplet *smallclueGetApplets(size_t *count) {
    if (count) {
        *count = 0;
    }
    return NULL;
}

int pscalRuntimeEnsureMountSourceAccess(const char *path) {
    (void)path;
    errno = ENOSYS;
    return -1;
}
