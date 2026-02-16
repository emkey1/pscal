#include "includes.h"
#include "pscal_runtime_hooks.h"

#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#ifdef PSCAL_TARGET_IOS
extern void pscalRuntimeDebugLog(const char *);
void pscal_clientloop_reset_hostkeys(void);
#endif

#ifndef PSCAL_THREAD_LOCAL
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
#define PSCAL_THREAD_LOCAL _Thread_local
#else
#define PSCAL_THREAD_LOCAL __thread
#endif
#endif

static PSCAL_THREAD_LOCAL pscal_openssh_exit_context *g_pscal_openssh_ctx = NULL;
static PSCAL_THREAD_LOCAL sigjmp_buf *g_pscal_openssh_global_exit_env = NULL;
static PSCAL_THREAD_LOCAL volatile sig_atomic_t *g_pscal_openssh_global_exit_code = NULL;
#ifdef PSCAL_TARGET_IOS
volatile sig_atomic_t interrupted = 0;
int showprogress = 1;
#else
volatile sig_atomic_t interrupted = 0;
int showprogress = 1;
#endif
void
pscal_openssh_push_exit_context(pscal_openssh_exit_context *ctx)
{
	if (!ctx)
		return;
	ctx->exit_code = 0;
	ctx->cleanup = NULL;
	ctx->prev = g_pscal_openssh_ctx;
	g_pscal_openssh_ctx = ctx;
}

void
pscal_openssh_pop_exit_context(pscal_openssh_exit_context *ctx)
{
	if (g_pscal_openssh_ctx == ctx) {
		g_pscal_openssh_ctx = ctx->prev;
	}
}

void
pscal_openssh_set_global_exit_handler(sigjmp_buf *env,
    volatile sig_atomic_t *code_out)
{
	g_pscal_openssh_global_exit_env = env;
	g_pscal_openssh_global_exit_code = code_out;
}

void
pscal_openssh_reset_progress_state(void)
{
#ifdef PSCAL_TARGET_IOS
	pscal_clientloop_reset_hostkeys();
	pscal_mux_reset_state();
	pscal_sshconnect_reset_state();
	pscal_sshconnect2_reset_state();
	pscal_sshtty_reset_state();
#endif
	interrupted = 0;
	showprogress = 1;
}

const char *
pscal_openssh_hostkey_path(const char *default_path)
{
	static char pathbuf[PATH_MAX];
#ifdef PSCAL_TARGET_IOS
	const char *root = getenv("PSCALI_CONTAINER_ROOT");
	if (!root || root[0] == '\0') {
		root = getenv("HOME");
	}
	if (root && root[0] != '\0' && default_path && default_path[0] != '\0') {
		const char *base = strrchr(default_path, '/');
		base = base ? base + 1 : default_path;
		snprintf(pathbuf, sizeof(pathbuf), "%s/etc/ssh/%s", root, base);
		return pathbuf;
	}
#endif
	return default_path;
}

const char *
pscal_openssh_hostkey_dir(void)
{
	const char *root = getenv("PSCALI_CONTAINER_ROOT");
	if (!root || root[0] == '\0') {
		root = getenv("HOME");
	}
	const char *workdir = getenv("PSCALI_WORKDIR");
	if (workdir && workdir[0] != '\0') {
		root = workdir;
	}
	if (!root || root[0] == '\0') {
		return NULL;
	}
	static char dirbuf[PATH_MAX];
	snprintf(dirbuf, sizeof(dirbuf), "%s/etc/ssh", root);
	return dirbuf;
}

void
pscal_openssh_register_cleanup(pscal_openssh_cleanup_fn cleanup)
{
	if (g_pscal_openssh_ctx != NULL) {
		g_pscal_openssh_ctx->cleanup = cleanup;
	}
}

#ifdef PSCAL_TARGET_IOS
void
cleanup_exit(int code)
{
	if (g_pscal_openssh_ctx != NULL) {
		pscal_openssh_cleanup_fn handler = g_pscal_openssh_ctx->cleanup;
		g_pscal_openssh_ctx->cleanup = NULL;
		if (handler != NULL) {
			handler(code);
		}
		g_pscal_openssh_ctx->exit_code = code;
		siglongjmp(g_pscal_openssh_ctx->env, 1);
	}
	if (g_pscal_openssh_global_exit_env != NULL) {
		if (g_pscal_openssh_global_exit_code != NULL) {
			*g_pscal_openssh_global_exit_code = code;
		}
		siglongjmp(*g_pscal_openssh_global_exit_env, 1);
	}
#ifdef PSCAL_TARGET_IOS
	pscalRuntimeDebugLog("cleanup_exit without PSCAL context, terminating thread");
	/* Avoid altering the alternate signal stack; just exit the thread. */
	pthread_exit((void*)(intptr_t)code);
#endif
	_exit(code);
}
#endif /* PSCAL_TARGET_IOS */
