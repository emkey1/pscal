#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ios/vproc.h"

#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
void pscalRuntimeDebugLog(const char *message) {
    (void)message;
}
#endif

int pscal_openssh_ssh_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 0;
}

int pscal_openssh_scp_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 0;
}

int pscal_openssh_sftp_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 0;
}

int pscalVprocTestChildMain(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 0;
}

int pscal_ios_close(int fd);
void pscal_ios_closefrom(int lowfd);
ssize_t pscal_ios_read(int fd, void *buf, size_t nbyte);
ssize_t pscal_ios_write(int fd, const void *buf, size_t nbyte);
int pscal_ios_open(const char *path, int oflag, ...);
int pscal_ios_openat(int fd, const char *path, int oflag, ...);
int pscal_ios_fstat(int fd, struct stat *buf);

static int createTrackedSocket(void) {
    int fd = vprocSocketShim(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        return fd;
    }
    int saved_errno = errno;
    fd = vprocSocketShim(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
        return fd;
    }
    errno = saved_errno;
    return -1;
}

static void assertCloseUntracksSocketResource(void) {
    int host_pipe[2] = {-1, -1};
    assert(pipe(host_pipe) == 0);

    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);

    int tracked_socket = createTrackedSocket();
    assert(tracked_socket >= 0);

    assert(pscal_ios_close(tracked_socket) == 0);
    errno = 0;
    assert(vprocCloseShim(tracked_socket) == -1);
    assert(errno == EBADF);

    vprocDeactivate();

    assert(dup2(host_pipe[0], tracked_socket) == tracked_socket);
    close(host_pipe[0]);
    host_pipe[0] = tracked_socket;
    assert(fcntl(host_pipe[0], F_GETFD) != -1);

    vprocDestroy(vp);

    /* Old behavior could close this reused host fd during vprocDestroy(). */
    assert(fcntl(host_pipe[0], F_GETFD) != -1);

    close(host_pipe[0]);
    close(host_pipe[1]);
}

static void assertClosefromOnlyClosesOwnedFds(void) {
    int host_pipe[2] = {-1, -1};
    assert(pipe(host_pipe) == 0);

    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);

    int s1 = createTrackedSocket();
    int s2 = createTrackedSocket();
    assert(s1 >= 0);
    assert(s2 >= 0);

    pscal_ios_closefrom(3);

    errno = 0;
    assert(fcntl(s1, F_GETFD) == -1);
    assert(errno == EBADF);
    errno = 0;
    assert(fcntl(s2, F_GETFD) == -1);
    assert(errno == EBADF);

    /* Created outside vproc ownership; must survive closefrom sweep. */
    assert(fcntl(host_pipe[0], F_GETFD) != -1);
    assert(fcntl(host_pipe[1], F_GETFD) != -1);

    vprocDeactivate();
    vprocDestroy(vp);

    close(host_pipe[0]);
    close(host_pipe[1]);
}

static void assertReadFallsBackToHostForUntrackedFileFd(void) {
    char templ[] = "/tmp/pscal-ios-shim-key-read-XXXXXX";
    int host_fd = mkstemp(templ);
    assert(host_fd >= 0);
    const char *payload = "ssh-key-material\n";
    size_t payload_len = strlen(payload);
    assert(write(host_fd, payload, payload_len) == (ssize_t)payload_len);
    assert(lseek(host_fd, 0, SEEK_SET) == 0);

    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);

    char buf[64] = {0};
    ssize_t n = pscal_ios_read(host_fd, buf, sizeof(buf));
    assert(n == (ssize_t)payload_len);
    assert(memcmp(buf, payload, payload_len) == 0);

    assert(pscal_ios_close(host_fd) == 0);
    errno = 0;
    assert(fcntl(host_fd, F_GETFD) == -1);
    assert(errno == EBADF);
    vprocDeactivate();
    vprocDestroy(vp);
    unlink(templ);
}

static void assertWriteFallsBackToHostForUntrackedFileFd(void) {
    char templ[] = "/tmp/pscal-ios-shim-key-write-XXXXXX";
    int host_fd = mkstemp(templ);
    assert(host_fd >= 0);

    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);

    const char *payload = "ssh-download-block\n";
    size_t payload_len = strlen(payload);
    ssize_t n = pscal_ios_write(host_fd, payload, payload_len);
    assert(n == (ssize_t)payload_len);
    assert(vprocHostLseek(host_fd, 0, SEEK_SET) == 0);

    char buf[64] = {0};
    assert(vprocHostRead(host_fd, buf, payload_len) == (ssize_t)payload_len);
    assert(memcmp(buf, payload, payload_len) == 0);

    assert(pscal_ios_close(host_fd) == 0);
    errno = 0;
    assert(fcntl(host_fd, F_GETFD) == -1);
    assert(errno == EBADF);

    vprocDeactivate();
    vprocDestroy(vp);
    unlink(templ);
}

static void assertRelativeOpenUsesVprocCwd(void) {
    char cwd[PATH_MAX];
    char tmpdir[PATH_MAX];
    char path_one[PATH_MAX];
    char path_two[PATH_MAX];

    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);

    assert(vprocShellGetcwdShim(cwd, sizeof(cwd)) != NULL);
    snprintf(tmpdir, sizeof(tmpdir), "%s/.tmp-ios-shim-open-cwd-%ld", cwd, (long)getpid());
    snprintf(path_one, sizeof(path_one), "%s/from_open.txt", tmpdir);
    snprintf(path_two, sizeof(path_two), "%s/from_openat.txt", tmpdir);

    (void)vprocUnlinkShim(path_one);
    (void)vprocUnlinkShim(path_two);
    (void)vprocRmdirShim(tmpdir);
    assert(vprocMkdirShim(tmpdir, 0700) == 0);

    assert(vprocShellChdirShim(tmpdir) == 0);

    int fd = pscal_ios_open("from_open.txt", O_CREAT | O_TRUNC | O_WRONLY, 0600);
    assert(fd >= 0);
    struct stat open_st;
    assert(pscal_ios_fstat(fd, &open_st) == 0);
    assert(S_ISREG(open_st.st_mode));
    assert(pscal_ios_write(fd, "ok", 2) == 2);
    assert(pscal_ios_close(fd) == 0);

#if defined(AT_FDCWD)
    fd = pscal_ios_openat(AT_FDCWD, "from_openat.txt", O_CREAT | O_TRUNC | O_WRONLY, 0600);
    assert(fd >= 0);
    assert(pscal_ios_fstat(fd, &open_st) == 0);
    assert(S_ISREG(open_st.st_mode));
    assert(pscal_ios_write(fd, "ok", 2) == 2);
    assert(pscal_ios_close(fd) == 0);
#endif

    struct stat sb;
    assert(vprocStatShim(path_one, &sb) == 0);
#if defined(AT_FDCWD)
    assert(vprocStatShim(path_two, &sb) == 0);
#endif

    assert(vprocShellChdirShim(cwd) == 0);
    assert(vprocUnlinkShim(path_one) == 0);
#if defined(AT_FDCWD)
    assert(vprocUnlinkShim(path_two) == 0);
#endif
    assert(vprocRmdirShim(tmpdir) == 0);

    vprocDeactivate();
    vprocDestroy(vp);
}

static void assertPrivateKeyFstatModePreserved(void) {
    char root_template[] = "/tmp/pscal-ios-shim-key-mode-XXXXXX";
    char *root = mkdtemp(root_template);
    assert(root != NULL);

    char home_dir[PATH_MAX];
    char ssh_dir[PATH_MAX];
    char key_path[PATH_MAX];
    snprintf(home_dir, sizeof(home_dir), "%s/home", root);
    snprintf(ssh_dir, sizeof(ssh_dir), "%s/.ssh", home_dir);
    snprintf(key_path, sizeof(key_path), "%s/id_ed25519", ssh_dir);

    assert(mkdir(home_dir, 0700) == 0);
    assert(mkdir(ssh_dir, 0700) == 0);

    int host_fd = open(key_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    assert(host_fd >= 0);
    const char *payload = "key\n";
    size_t payload_len = strlen(payload);
    assert(write(host_fd, payload, payload_len) == (ssize_t)payload_len);
    assert(close(host_fd) == 0);

    struct stat st;
    assert(stat(key_path, &st) == 0);
    assert(S_ISREG(st.st_mode));
    assert((st.st_mode & 0777) == 0600);

    const char *path_truncate_env = getenv("PATH_TRUNCATE");
    const char *container_root_env = getenv("PSCALI_CONTAINER_ROOT");
    char *saved_path_truncate = path_truncate_env ? strdup(path_truncate_env) : NULL;
    char *saved_container_root = container_root_env ? strdup(container_root_env) : NULL;

    setenv("PATH_TRUNCATE", root, 1);
    unsetenv("PSCALI_CONTAINER_ROOT");

    VProc *vp = vprocCreate(NULL);
    assert(vp != NULL);
    vprocActivate(vp);

    int key_fd = pscal_ios_open("/home/.ssh/id_ed25519", O_RDONLY);
    assert(key_fd >= 0);
    assert(pscal_ios_fstat(key_fd, &st) == 0);
    assert(S_ISREG(st.st_mode));
    assert((st.st_mode & 0777) == 0600);
    assert(pscal_ios_close(key_fd) == 0);

    vprocDeactivate();
    vprocDestroy(vp);

    if (saved_path_truncate) {
        setenv("PATH_TRUNCATE", saved_path_truncate, 1);
    } else {
        unsetenv("PATH_TRUNCATE");
    }
    if (saved_container_root) {
        setenv("PSCALI_CONTAINER_ROOT", saved_container_root, 1);
    } else {
        unsetenv("PSCALI_CONTAINER_ROOT");
    }
    free(saved_path_truncate);
    free(saved_container_root);

    assert(unlink(key_path) == 0);
    assert(rmdir(ssh_dir) == 0);
    assert(rmdir(home_dir) == 0);
    assert(rmdir(root) == 0);
}

int main(void) {
    fprintf(stderr, "TEST pscal_ios_close_untracks_socket_resource\n");
    assertCloseUntracksSocketResource();

    fprintf(stderr, "TEST pscal_ios_closefrom_only_closes_owned_fds\n");
    assertClosefromOnlyClosesOwnedFds();

    fprintf(stderr, "TEST pscal_ios_read_falls_back_to_host_for_untracked_file_fd\n");
    assertReadFallsBackToHostForUntrackedFileFd();

    fprintf(stderr, "TEST pscal_ios_write_falls_back_to_host_for_untracked_file_fd\n");
    assertWriteFallsBackToHostForUntrackedFileFd();

    fprintf(stderr, "TEST pscal_ios_relative_open_uses_vproc_cwd\n");
    assertRelativeOpenUsesVprocCwd();

    fprintf(stderr, "TEST pscal_ios_private_key_fstat_mode_preserved\n");
    assertPrivateKeyFstatModePreserved();

    fprintf(stderr, "PASS ios openssh shim fd lifecycle tests\n");
    return 0;
}
