#include "includes.h"
#include "pscal_runtime_hooks.h"

#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef PSCAL_TARGET_IOS
extern void pscalRuntimeDebugLog(const char *);
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
#define PSCAL_THREAD_LOCAL _Thread_local
#else
#define PSCAL_THREAD_LOCAL __thread
#endif

static PSCAL_THREAD_LOCAL pscal_openssh_exit_context *g_pscal_openssh_ctx = NULL;
#ifdef PSCAL_TARGET_IOS
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
pscal_openssh_reset_progress_state(void)
{
#ifdef PSCAL_TARGET_IOS
	interrupted = 0;
	showprogress = 1;
#endif
}

void
pscal_openssh_register_cleanup(pscal_openssh_cleanup_fn cleanup)
{
	if (g_pscal_openssh_ctx != NULL) {
		g_pscal_openssh_ctx->cleanup = cleanup;
	}
}

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
		longjmp(g_pscal_openssh_ctx->env, 1);
	}
#ifdef PSCAL_TARGET_IOS
	pscalRuntimeDebugLog("cleanup_exit without PSCAL context, aborting process");
#endif
	_exit(code);
}
