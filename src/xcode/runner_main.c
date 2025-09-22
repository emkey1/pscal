#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_error(const char *message) {
    fprintf(stderr, "pscal-runner: %s\n", message);
}

static int join_path(char *dest, size_t dest_size, const char *prefix, const char *suffix) {
    int written = snprintf(dest, dest_size, "%s%s", prefix, suffix);
    if (written < 0 || (size_t)written >= dest_size) {
        print_error("computed executable path is too long");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    char resolved_self[PATH_MAX];
    if (realpath(argv[0], resolved_self) == NULL) {
        fprintf(
            stderr,
            "pscal-runner: unable to resolve runner path '%s': %s\n",
            argv[0],
            strerror(errno)
        );
        return 1;
    }

    char runner_dir[PATH_MAX];
    if (snprintf(runner_dir, sizeof(runner_dir), "%s", resolved_self) >= (int)sizeof(runner_dir)) {
        print_error("runner path is too long");
        return 1;
    }

    char *slash = strrchr(runner_dir, '/');
    if (slash == NULL) {
        print_error("runner path is missing a directory component");
        return 1;
    }
    slash[1] = '\0';

    const char *override_exec = getenv("PSCAL_RUN_EXECUTABLE");
    const char *target_name = getenv("PSCAL_RUN_TARGET");
    if (target_name == NULL || target_name[0] == '\0') {
        target_name = "pascal";
    }

    char target_path[PATH_MAX];
    if (override_exec != NULL && override_exec[0] != '\0') {
        if (override_exec[0] == '/') {
            if ((size_t)snprintf(target_path, sizeof(target_path), "%s", override_exec) >= sizeof(target_path)) {
                print_error("override executable path is too long");
                return 1;
            }
        } else if (join_path(target_path, sizeof(target_path), runner_dir, override_exec) != 0) {
            return 1;
        }
    } else if (join_path(target_path, sizeof(target_path), runner_dir, target_name) != 0) {
        return 1;
    }

    if (access(target_path, X_OK) != 0) {
        fprintf(stderr, "pscal-runner: executable '%s' is not available: %s\n", target_path, strerror(errno));
        return 1;
    }

    char **child_argv = calloc((size_t)argc + 1, sizeof(char *));
    if (child_argv == NULL) {
        print_error("out of memory");
        return 1;
    }

    child_argv[0] = target_path;
    for (int index = 1; index < argc; ++index) {
        child_argv[index] = argv[index];
    }

    fprintf(stderr, "pscal-runner: executing %s\n", target_path);
    execv(target_path, child_argv);

    fprintf(stderr, "pscal-runner: execv('%s') failed: %s\n", target_path, strerror(errno));
    free(child_argv);
    return 1;
}
