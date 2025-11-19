#include "smallclue/openssh_app.h"

#include <stdio.h>

#include <setjmp.h>
#include <pthread.h>
#include <signal.h>
#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN (64 * 1024)
#endif
#include "../../third-party/openssh-10.2p1/pscal_runtime_hooks.h"

int pscal_openssh_ssh_main(int argc, char **argv);
int pscal_openssh_scp_main(int argc, char **argv);
int pscal_openssh_sftp_main(int argc, char **argv);

#ifndef SMALLCLUE_OPENSSH_STACK_SIZE
#define SMALLCLUE_OPENSSH_STACK_SIZE (16ull * 1024ull * 1024ull)
#endif

typedef struct {
    const char *label;
    int (*entry)(int, char **);
    int argc;
    char **argv;
    int status;
    volatile sig_atomic_t exitRequested;
} smallclueOpensshThreadContext;

volatile sig_atomic_t g_smallclue_openssh_exit_requested = 0;

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
    int status;
    if (setjmp(exitContext.env) == 0) {
        status = entry(argc, argv);
    } else {
        status = exitContext.exit_code;
    }
    pscal_openssh_pop_exit_context(&exitContext);
    return status;
}

static void *smallclueOpensshThreadMain(void *arg) {
    smallclueOpensshThreadContext *ctx = (smallclueOpensshThreadContext *)arg;
    ctx->status = smallclueInvokeOpensshEntry(ctx->label, ctx->entry, ctx->argc, ctx->argv);
    g_smallclue_openssh_exit_requested = 1;
    ctx->exitRequested = 1;
    return NULL;
}

static int smallclueRunOpensshEntry(const char *label, int (*entry)(int, char **),
                                    int argc, char **argv) {
    if (!entry) {
        fprintf(stderr, "%s: command unavailable\n", label ? label : "ssh");
        return 127;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    size_t desiredStack = SMALLCLUE_OPENSSH_STACK_SIZE;
    if (desiredStack < (size_t)PTHREAD_STACK_MIN) {
        desiredStack = (size_t)PTHREAD_STACK_MIN;
    }
    int err = pthread_attr_setstacksize(&attr, desiredStack);
    if (err != 0) {
        pthread_attr_destroy(&attr);
        return smallclueInvokeOpensshEntry(label, entry, argc, argv);
    }

    smallclueOpensshThreadContext ctx = {
        .label = label,
        .entry = entry,
        .argc = argc,
        .argv = argv,
        .status = 127
    };

    pthread_t thread;
    err = pthread_create(&thread, &attr, smallclueOpensshThreadMain, &ctx);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        return smallclueInvokeOpensshEntry(label, entry, argc, argv);
    }

    pthread_join(thread, NULL);
    ctx.exitRequested = 1;
    return ctx.status;
}

int smallclueRunSsh(int argc, char **argv) {
    return smallclueRunOpensshEntry("ssh", pscal_openssh_ssh_main, argc, argv);
}

int smallclueRunScp(int argc, char **argv) {
    return smallclueRunOpensshEntry("scp", pscal_openssh_scp_main, argc, argv);
}

int smallclueRunSftp(int argc, char **argv) {
    return smallclueRunOpensshEntry("sftp", pscal_openssh_sftp_main, argc, argv);
}
