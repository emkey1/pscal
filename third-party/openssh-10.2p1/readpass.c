/* $OpenBSD: readpass.c,v 1.72 2025/06/11 13:24:05 dtucker Exp $ */
/*
 * Copyright (c) 2001 Markus Friedl.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "includes.h"

#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef PSCAL_TARGET_IOS
#include "ios/vproc.h"
#include "common/runtime_tty.h"
#include <pthread.h>
extern VProcSessionStdio *PSCALRuntimeGetCurrentRuntimeStdio(void) __attribute__((weak));
#endif

#include "xmalloc.h"
#include "misc.h"
#include "pathnames.h"
#include "log.h"
#include "ssh.h"
#include "uidswap.h"

#ifdef PSCAL_TARGET_IOS
static void
pscal_dump_session_state(const char *tag, int host_fd)
{
	VProcSessionStdio *session = vprocSessionStdioCurrent();
	if (!session) {
		debug3_f("PSCAL iOS %s session=null host_fd=%d", tag, host_fd);
		return;
	}
	VProcSessionInput *input = session->input;
	int stdin_fd = session->stdin_host_fd;
	int stdout_fd = session->stdout_host_fd;
	int stderr_fd = session->stderr_host_fd;
	bool needs_refresh = vprocSessionStdioNeedsRefresh(session);
	if (!input) {
		debug3_f("PSCAL iOS %s session_in=%d out=%d err=%d host_fd=%d refresh=%d input=null",
		    tag, stdin_fd, stdout_fd, stderr_fd, host_fd, (int)needs_refresh);
		return;
	}
	pthread_mutex_lock(&input->mu);
	debug3_f("PSCAL iOS %s session_in=%d out=%d err=%d host_fd=%d refresh=%d len=%zu cap=%zu eof=%d reader=%d reader_fd=%d stop=%d intr=%d",
	    tag,
	    stdin_fd,
	    stdout_fd,
	    stderr_fd,
	    host_fd,
	    (int)needs_refresh,
	    input->len,
	    input->cap,
	    (int)input->eof,
	    (int)input->reader_active,
	    input->reader_fd,
	    (int)input->stop_requested,
	    (int)input->interrupt_pending);
	pthread_mutex_unlock(&input->mu);
}
#endif

static char *
ssh_askpass(char *askpass, const char *msg, const char *env_hint)
{
	pid_t pid, ret;
	size_t len;
	char *pass;
	int p[2], status;
	char buf[1024];
	void (*osigchld)(int);

#ifdef PSCAL_TARGET_IOS
	(void)askpass;
	(void)env_hint;

	if (msg && *msg) {
		size_t msglen = strlen(msg);
		(void)write(STDERR_FILENO, msg, msglen);
		if (msg[msglen - 1] != ' ')
			(void)write(STDERR_FILENO, " ", 1);
	}
	if (readpassphrase("", buf, sizeof(buf), RPP_ECHO_OFF | RPP_STDIN) == NULL)
		return NULL;
	buf[strcspn(buf, "\r\n")] = '\0';
	pass = xstrdup(buf);
	explicit_bzero(buf, sizeof(buf));
	return pass;
#endif

	if (fflush(stdout) != 0)
		error_f("fflush: %s", strerror(errno));
	if (askpass == NULL)
		fatal("internal error: askpass undefined");
	if (pipe(p) == -1) {
		error_f("pipe: %s", strerror(errno));
		return NULL;
	}
	osigchld = ssh_signal(SIGCHLD, SIG_DFL);
	if ((pid = fork()) == -1) {
		error_f("fork: %s", strerror(errno));
		ssh_signal(SIGCHLD, osigchld);
		return NULL;
	}
	if (pid == 0) {
		close(p[0]);
		if (dup2(p[1], STDOUT_FILENO) == -1)
			fatal_f("dup2: %s", strerror(errno));
		if (env_hint != NULL)
			setenv("SSH_ASKPASS_PROMPT", env_hint, 1);
		execlp(askpass, askpass, msg, (char *)NULL);
		fatal_f("exec(%s): %s", askpass, strerror(errno));
	}
	close(p[1]);

	len = 0;
	do {
		ssize_t r = read(p[0], buf + len, sizeof(buf) - 1 - len);

		if (r == -1 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		len += r;
	} while (len < sizeof(buf) - 1);
	buf[len] = '\0';

	close(p[0]);
	while ((ret = waitpid(pid, &status, 0)) == -1)
		if (errno != EINTR)
			break;
	ssh_signal(SIGCHLD, osigchld);
	if (ret == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		explicit_bzero(buf, sizeof(buf));
		return NULL;
	}

	buf[strcspn(buf, "\r\n")] = '\0';
	pass = xstrdup(buf);
	explicit_bzero(buf, sizeof(buf));
	return pass;
}

/* private/internal read_passphrase flags */
#define RP_ASK_PERMISSION	0x8000 /* pass hint to askpass for confirm UI */

/*
 * Reads a passphrase from /dev/tty with echo turned off/on.  Returns the
 * passphrase (allocated with xmalloc).  Exits if EOF is encountered. If
 * RP_ALLOW_STDIN is set, the passphrase will be read from stdin if no
 * tty is or askpass program is available
 */
char *
read_passphrase(const char *prompt, int flags)
{
	char cr = '\r', *askpass = NULL, *ret, buf[1024];
	int rppflags, ttyfd, use_askpass = 0, allow_askpass = 0;
	const char *askpass_hint = NULL;
	const char *s;

#ifdef PSCAL_TARGET_IOS
	debug2_f("PSCAL iOS read_passphrase prompt=\"%s\" flags=0x%x",
	    prompt ? prompt : "", flags);
	VProc *vp = vprocCurrent();
	int host_fd = -1;
	int host_errno = 0;
	VProcSessionStdio *session = vprocSessionStdioCurrent();
	VProcSessionStdio *prompt_session = session;
	struct termios saved_termios;
	bool restore_termios = false;
	if (vp) {
		host_fd = vprocTranslateFd(vp, STDIN_FILENO);
		host_errno = errno;
	}
	bool use_session_queue = false;
	if (session && session->stdin_host_fd >= 0) {
		if (pscalRuntimeStdinIsInteractive()) {
			use_session_queue = true;
		} else if (host_fd >= 0) {
			if (session->stdin_host_fd == host_fd) {
				use_session_queue = true;
			} else {
				struct stat session_st;
				struct stat host_st;
				if (fstat(session->stdin_host_fd, &session_st) == 0 &&
				    fstat(host_fd, &host_st) == 0 &&
				    session_st.st_dev == host_st.st_dev &&
				    session_st.st_ino == host_st.st_ino) {
					use_session_queue = true;
				}
			}
		}
	}
	if (!use_session_queue && PSCALRuntimeGetCurrentRuntimeStdio) {
		VProcSessionStdio *runtime_stdio = PSCALRuntimeGetCurrentRuntimeStdio();
		if (runtime_stdio && runtime_stdio != session &&
		    !vprocSessionStdioIsDefault(runtime_stdio)) {
			bool runtime_has_input = runtime_stdio->pty_active ||
			    runtime_stdio->stdin_pscal_fd ||
			    runtime_stdio->stdin_host_fd < 0;
			if (runtime_has_input) {
				prompt_session = runtime_stdio;
				use_session_queue = true;
			}
		}
	}
	bool switched_session = false;
	if (prompt_session && prompt_session != session) {
		vprocSessionStdioActivate(prompt_session);
		switched_session = true;
	}
	debug3_f("PSCAL iOS read_passphrase stdin vp=%p host=%d host_errno=%d session_in=%d prompt_in=%d use_session=%d",
	    (void *)vp,
	    host_fd,
	    host_errno,
	    session ? session->stdin_host_fd : -1,
	    prompt_session ? prompt_session->stdin_host_fd : -1,
	    (int)use_session_queue);
	if (getenv("PSCALI_TOOL_DEBUG")) {
		pscal_dump_session_state("readpass-start", host_fd);
		fprintf(stderr,
		    "[readpass-ios] host=%d session_in=%d prompt_in=%d use_session=%d\n",
		    host_fd,
		    session ? session->stdin_host_fd : -1,
		    prompt_session ? prompt_session->stdin_host_fd : -1,
		    (int)use_session_queue);
	}
	if ((flags & RP_ECHO) == 0) {
		if (vprocSessionStdioFetchTermios(STDIN_FILENO, &saved_termios)) {
			struct termios raw = saved_termios;
			raw.c_lflag &= ~(ECHO | ECHONL);
			if (vprocSessionStdioApplyTermios(STDIN_FILENO, TCSAFLUSH, &raw)) {
				restore_termios = true;
			}
		}
	}
	if (prompt && *prompt) {
		size_t plen = strlen(prompt);
		(void)write(STDERR_FILENO, prompt, plen);
		if (prompt[plen - 1] != ' ')
			(void)write(STDERR_FILENO, " ", 1);
	}
	size_t len = 0;
	char ch = '\0';
	while (len + 1 < sizeof(buf)) {
		ssize_t rd = use_session_queue ?
		    vprocSessionReadInputShim(&ch, 1) :
		    vprocReadShim(STDIN_FILENO, &ch, 1);
		if (rd < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
			continue;
		}
		if (rd <= 0) {
			debug3_f("PSCAL iOS read_passphrase read rc=%zd errno=%d",
			    rd, errno);
			if (getenv("PSCALI_TOOL_DEBUG")) {
				pscal_dump_session_state("readpass-fail", host_fd);
			}
			if (flags & RP_ALLOW_EOF) {
				explicit_bzero(buf, sizeof(buf));
				ret = NULL;
				goto readpass_done;
			}
			buf[0] = '\0';
			ret = xstrdup(buf);
			explicit_bzero(buf, sizeof(buf));
			goto readpass_done;
		}
		if (ch == '\n' || ch == '\r')
			break;
		buf[len++] = ch;
	}
	buf[len] = '\0';
	debug2_f("PSCAL iOS read_passphrase len=%zu", len);
	if (getenv("PSCALI_TOOL_DEBUG")) {
		pscal_dump_session_state("readpass-done", host_fd);
	}
	ret = xstrdup(buf);
	explicit_bzero(buf, sizeof(buf));
readpass_done:
	if (restore_termios) {
		(void)vprocSessionStdioApplyTermios(STDIN_FILENO, TCSANOW, &saved_termios);
	}
	if (switched_session) {
		if (session) {
			vprocSessionStdioActivate(session);
		} else {
			vprocSessionStdioActivate(NULL);
		}
	}
	return ret;
#endif

	if (((s = getenv("DISPLAY")) != NULL && *s != '\0') ||
	    ((s = getenv("WAYLAND_DISPLAY")) != NULL && *s != '\0'))
		allow_askpass = 1;
	if ((s = getenv(SSH_ASKPASS_REQUIRE_ENV)) != NULL) {
		if (strcasecmp(s, "force") == 0) {
			use_askpass = 1;
			allow_askpass = 1;
		} else if (strcasecmp(s, "prefer") == 0)
			use_askpass = allow_askpass;
		else if (strcasecmp(s, "never") == 0)
			allow_askpass = 0;
	}

	rppflags = (flags & RP_ECHO) ? RPP_ECHO_ON : RPP_ECHO_OFF;
	if (use_askpass)
		debug_f("requested to askpass");
	else if (flags & RP_USE_ASKPASS)
		use_askpass = 1;
	else if (flags & RP_ALLOW_STDIN) {
		if (!isatty(STDIN_FILENO)) {
			debug_f("stdin is not a tty");
			use_askpass = 1;
		}
	} else {
		rppflags |= RPP_REQUIRE_TTY;
		ttyfd = open(_PATH_TTY, O_RDWR);
		if (ttyfd >= 0) {
			/*
			 * If we're on a tty, ensure that show the prompt at
			 * the beginning of the line. This will hopefully
			 * clobber any password characters the user has
			 * optimistically typed before echo is disabled.
			 */
			(void)write(ttyfd, &cr, 1);
			close(ttyfd);
		} else {
			debug_f("can't open %s: %s", _PATH_TTY,
			    strerror(errno));
			use_askpass = 1;
		}
	}

	if ((flags & RP_USE_ASKPASS) && !allow_askpass)
		return (flags & RP_ALLOW_EOF) ? NULL : xstrdup("");

	if (use_askpass && allow_askpass) {
		if (getenv(SSH_ASKPASS_ENV))
			askpass = getenv(SSH_ASKPASS_ENV);
		else
			askpass = _PATH_SSH_ASKPASS_DEFAULT;
		if ((flags & RP_ASK_PERMISSION) != 0)
			askpass_hint = "confirm";
		if ((ret = ssh_askpass(askpass, prompt, askpass_hint)) == NULL)
			if (!(flags & RP_ALLOW_EOF))
				return xstrdup("");
		return ret;
	}

	if (readpassphrase(prompt, buf, sizeof buf, rppflags) == NULL) {
		if (flags & RP_ALLOW_EOF)
			return NULL;
		return xstrdup("");
	}

	ret = xstrdup(buf);
	explicit_bzero(buf, sizeof(buf));
	return ret;
}

int
ask_permission(const char *fmt, ...)
{
	va_list args;
	char *p, prompt[1024];
	int allowed = 0;

	va_start(args, fmt);
	vsnprintf(prompt, sizeof(prompt), fmt, args);
	va_end(args);

	p = read_passphrase(prompt,
	    RP_USE_ASKPASS|RP_ALLOW_EOF|RP_ASK_PERMISSION);
	if (p != NULL) {
		/*
		 * Accept empty responses and responses consisting
		 * of the word "yes" as affirmative.
		 */
		if (*p == '\0' || *p == '\n' ||
		    strcasecmp(p, "yes") == 0)
			allowed = 1;
		free(p);
	}

	return (allowed);
}

static void
writemsg(const char *msg)
{
	(void)write(STDERR_FILENO, "\r", 1);
	(void)write(STDERR_FILENO, msg, strlen(msg));
	(void)write(STDERR_FILENO, "\r\n", 2);
}

struct notifier_ctx {
	pid_t pid;
	void (*osigchld)(int);
};

struct notifier_ctx *
notify_start(int force_askpass, const char *fmt, ...)
{
	va_list args;
	char *prompt = NULL;
	pid_t pid = -1;
	void (*osigchld)(int) = NULL;
	const char *askpass, *s;
	struct notifier_ctx *ret = NULL;

	va_start(args, fmt);
	xvasprintf(&prompt, fmt, args);
	va_end(args);

	if (fflush(NULL) != 0)
		error_f("fflush: %s", strerror(errno));
	if (!force_askpass && isatty(STDERR_FILENO)) {
		writemsg(prompt);
		goto out_ctx;
	}
	if ((askpass = getenv("SSH_ASKPASS")) == NULL)
		askpass = _PATH_SSH_ASKPASS_DEFAULT;
	if (*askpass == '\0') {
		debug3_f("cannot notify: no askpass");
		goto out;
	}
	if (getenv("DISPLAY") == NULL && getenv("WAYLAND_DISPLAY") == NULL &&
	    ((s = getenv(SSH_ASKPASS_REQUIRE_ENV)) == NULL ||
	    strcmp(s, "force") != 0)) {
		debug3_f("cannot notify: no display");
		goto out;
	}
	osigchld = ssh_signal(SIGCHLD, SIG_DFL);
	if ((pid = fork()) == -1) {
		error_f("fork: %s", strerror(errno));
		ssh_signal(SIGCHLD, osigchld);
		free(prompt);
		return NULL;
	}
	if (pid == 0) {
		if (stdfd_devnull(1, 1, 0) == -1)
			fatal_f("stdfd_devnull failed");
		closefrom(STDERR_FILENO + 1);
		setenv("SSH_ASKPASS_PROMPT", "none", 1); /* hint to UI */
		execlp(askpass, askpass, prompt, (char *)NULL);
		error_f("exec(%s): %s", askpass, strerror(errno));
		_exit(1);
		/* NOTREACHED */
	}
 out_ctx:
	if ((ret = calloc(1, sizeof(*ret))) == NULL) {
		if (pid != -1)
			kill(pid, SIGTERM);
		fatal_f("calloc failed");
	}
	ret->pid = pid;
	ret->osigchld = osigchld;
 out:
	free(prompt);
	return ret;
}

void
notify_complete(struct notifier_ctx *ctx, const char *fmt, ...)
{
	int ret;
	char *msg = NULL;
	va_list args;

	if (ctx != NULL && fmt != NULL && ctx->pid == -1) {
		/*
		 * notify_start wrote to stderr, so send conclusion message
		 * there too
		*/
		va_start(args, fmt);
		xvasprintf(&msg, fmt, args);
		va_end(args);
		writemsg(msg);
		free(msg);
	}

	if (ctx == NULL || ctx->pid <= 0) {
		free(ctx);
		return;
	}
	kill(ctx->pid, SIGTERM);
	while ((ret = waitpid(ctx->pid, NULL, 0)) == -1) {
		if (errno != EINTR)
			break;
	}
	if (ret == -1)
		fatal_f("waitpid: %s", strerror(errno));
	ssh_signal(SIGCHLD, ctx->osigchld);
	free(ctx);
}
