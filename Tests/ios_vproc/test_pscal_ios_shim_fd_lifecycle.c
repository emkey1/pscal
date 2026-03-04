#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
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

int main(void) {
    fprintf(stderr, "TEST pscal_ios_close_untracks_socket_resource\n");
    assertCloseUntracksSocketResource();

    fprintf(stderr, "TEST pscal_ios_closefrom_only_closes_owned_fds\n");
    assertClosefromOnlyClosesOwnedFds();

    fprintf(stderr, "PASS ios openssh shim fd lifecycle tests\n");
    return 0;
}
