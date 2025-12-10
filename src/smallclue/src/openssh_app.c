#include "openssh_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include "../../third-party/openssh-10.2p1/pscal_runtime_hooks.h"

int pscal_openssh_ssh_main(int argc, char **argv);
int pscal_openssh_scp_main(int argc, char **argv);
int pscal_openssh_sftp_main(int argc, char **argv);
int pscal_openssh_ssh_keygen_main(int argc, char **argv);

volatile sig_atomic_t g_smallclue_openssh_exit_requested = 0;

typedef struct {
    int pipe_read;
    int pipe_write_dup;
    int stdout_dup;
    int stderr_dup;
    int log_fd;
    pthread_t thread;
    bool active;
} SmallclueLogTee;

static void smallclueFreeArgv(char **argv, int count) {
    if (!argv) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        free(argv[i]);
    }
    free(argv);
}

static int smallclueEnsureDirectory(const char *path, mode_t mode) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (errno != ENOENT) {
        return -1;
    }
    if (mkdir(path, mode) == 0) {
        return 0;
    }
    return (errno == EEXIST) ? 0 : -1;
}

static char *smallclueKnownHostsPath(void) {
    const char *home = getenv("HOME");
    if (!home || !*home) {
        home = ".";
    }
    char ssh_dir[PATH_MAX];
    int written = snprintf(ssh_dir, sizeof(ssh_dir), "%s/.ssh", home);
    if (written < 0 || written >= (int)sizeof(ssh_dir)) {
        return NULL;
    }
    if (smallclueEnsureDirectory(ssh_dir, 0700) != 0) {
        return NULL;
    }
    char file_path[PATH_MAX];
    written = snprintf(file_path, sizeof(file_path), "%s/known_hosts", ssh_dir);
    if (written < 0 || written >= (int)sizeof(file_path)) {
        return NULL;
    }
    return strdup(file_path);
}

static char *smallclueRuntimeLogPath(void) {
    const char *home = getenv("HOME");
    if (!home || !*home) {
        home = ".";
    }
    char var_dir[PATH_MAX];
    int written = snprintf(var_dir, sizeof(var_dir), "%s/Documents/var", home);
    if (written < 0 || written >= (int)sizeof(var_dir)) {
        return NULL;
    }
    if (smallclueEnsureDirectory(var_dir, 0755) != 0) {
        return NULL;
    }
    char log_dir[PATH_MAX];
    written = snprintf(log_dir, sizeof(log_dir), "%s/log", var_dir);
    if (written < 0 || written >= (int)sizeof(log_dir)) {
        return NULL;
    }
    if (smallclueEnsureDirectory(log_dir, 0755) != 0) {
        return NULL;
    }
    char file_path[PATH_MAX];
    written = snprintf(file_path, sizeof(file_path), "%s/pscal_runtime.log", log_dir);
    if (written < 0 || written >= (int)sizeof(file_path)) {
        return NULL;
    }
    return strdup(file_path);
}

static void *smallclueLogTeePump(void *arg) {
    SmallclueLogTee *tee = (SmallclueLogTee *)arg;
    if (!tee) return NULL;
    char buffer[4096];
    while (1) {
        ssize_t n = read(tee->pipe_read, buffer, sizeof(buffer));
        if (n <= 0) {
            break;
        }
        if (tee->stdout_dup >= 0) {
            (void)write(tee->stdout_dup, buffer, (size_t)n);
        }
        if (tee->stderr_dup >= 0) {
            (void)write(tee->stderr_dup, buffer, (size_t)n);
        }
        if (tee->log_fd >= 0) {
            (void)write(tee->log_fd, buffer, (size_t)n);
        }
    }
    return NULL;
}

static void smallclueLogTeeStop(SmallclueLogTee *tee) {
    if (!tee || !tee->active) return;
    /* Restore original stdout/stderr before closing backups. */
    if (tee->stdout_dup >= 0) {
        dup2(tee->stdout_dup, STDOUT_FILENO);
    }
    if (tee->stderr_dup >= 0) {
        dup2(tee->stderr_dup, STDERR_FILENO);
    }
    close(tee->pipe_read);
    tee->pipe_read = -1;
    pthread_join(tee->thread, NULL);
    if (tee->pipe_write_dup >= 0) close(tee->pipe_write_dup);
    if (tee->stdout_dup >= 0) close(tee->stdout_dup);
    if (tee->stderr_dup >= 0) close(tee->stderr_dup);
    if (tee->log_fd >= 0) close(tee->log_fd);
    tee->active = false;
}

static bool smallclueLogTeeStart(SmallclueLogTee *tee) {
    (void)tee;
    return false;
}

static int smallclueInvokeOpensshEntry(const char *label, int (*entry)(int, char **),
                                       int argc, char **argv) {
    g_smallclue_openssh_exit_requested = 0;
    if (!entry) {
        fprintf(stderr, "%s: command unavailable\n", label ? label : "ssh");
        return 127;
    }
    pscal_openssh_exit_context exitContext;
    pscal_openssh_reset_progress_state();
    pscal_openssh_push_exit_context(&exitContext);
    SmallclueLogTee tee;
    bool tee_active = false; /* Disable log tee in standalone build */
    int status;
    if (sigsetjmp(exitContext.env, 0) == 0) {
        status = entry(argc, argv);
    } else {
        status = exitContext.exit_code;
    }
    if (tee_active) {
        fflush(stdout);
        fflush(stderr);
        smallclueLogTeeStop(&tee);
    }
    pscal_openssh_pop_exit_context(&exitContext);
    return status;
}

static int smallclueRunOpensshEntry(const char *label, int (*entry)(int, char **),
                                    int argc, char **argv) {
    if (!entry) {
        fprintf(stderr, "%s: command unavailable\n", label ? label : "ssh");
        return 127;
    }
    struct sigaction old_pipe;
    struct sigaction ignore_action;
    memset(&ignore_action, 0, sizeof(ignore_action));
    ignore_action.sa_handler = SIG_IGN;
    sigemptyset(&ignore_action.sa_mask);
    sigaction(SIGPIPE, &ignore_action, &old_pipe);
    int status = smallclueInvokeOpensshEntry(label, entry, argc, argv);
    sigaction(SIGPIPE, &old_pipe, NULL);
    return status;
}

int smallclueRunSsh(int argc, char **argv) {
    char *known_hosts_path = smallclueKnownHostsPath();
    if (!known_hosts_path) {
        return smallclueRunOpensshEntry("ssh", pscal_openssh_ssh_main, argc, argv);
    }
    size_t opt_len = strlen("UserKnownHostsFile=") + strlen(known_hosts_path) + 1;
    char *known_hosts_opt = (char *)malloc(opt_len);
    if (!known_hosts_opt) {
        free(known_hosts_path);
        return smallclueRunOpensshEntry("ssh", pscal_openssh_ssh_main, argc, argv);
    }
    snprintf(known_hosts_opt, opt_len, "UserKnownHostsFile=%s", known_hosts_path);
    const char *strict_opt = "StrictHostKeyChecking=accept-new";
    int extra = 4;
    int new_argc = argc + extra;
    char **augmented = (char **)calloc((size_t)new_argc + 1, sizeof(char *));
    if (!augmented) {
        free(known_hosts_opt);
        free(known_hosts_path);
        return smallclueRunOpensshEntry("ssh", pscal_openssh_ssh_main, argc, argv);
    }
    int count = 0;
    augmented[count++] = strdup((argc > 0 && argv && argv[0]) ? argv[0] : "ssh");
    augmented[count++] = strdup("-o");
    augmented[count++] = known_hosts_opt;
    known_hosts_opt = NULL;
    augmented[count++] = strdup("-o");
    augmented[count++] = strdup(strict_opt);
    for (int i = 1; i < argc; ++i) {
        augmented[count++] = argv[i] ? strdup(argv[i]) : strdup("");
    }
    bool alloc_failed = false;
    for (int i = 0; i < count; ++i) {
        if (!augmented[i]) {
            alloc_failed = true;
            break;
        }
    }
    int status;
    if (!alloc_failed) {
        fprintf(stderr,
                "ssh: automatically accepting new host keys; cache=%s\n",
                known_hosts_path);
        status = smallclueRunOpensshEntry("ssh", pscal_openssh_ssh_main, count, augmented);
    } else {
        status = smallclueRunOpensshEntry("ssh", pscal_openssh_ssh_main, argc, argv);
    }
    smallclueFreeArgv(augmented, count);
    free(known_hosts_opt);
    free(known_hosts_path);
    return status;
}

int smallclueRunScp(int argc, char **argv) {
    return smallclueRunOpensshEntry("scp", pscal_openssh_scp_main, argc, argv);
}

int smallclueRunSftp(int argc, char **argv) {
    return smallclueRunOpensshEntry("sftp", pscal_openssh_sftp_main, argc, argv);
}

int smallclueRunSshKeygen(int argc, char **argv) {
    return smallclueRunOpensshEntry("ssh-keygen", pscal_openssh_ssh_keygen_main, argc, argv);
}
