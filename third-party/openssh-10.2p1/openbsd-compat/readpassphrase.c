/*	$OpenBSD: readpassphrase.c,v 1.26 2016/10/18 12:47:18 millert Exp $	*/

/*
 * Copyright (c) 2000-2002, 2007, 2010
 *	Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Sponsored in part by the Defense Advanced Research Projects
 * Agency (DARPA) and Air Force Research Laboratory, Air Force
 * Materiel Command, USAF, under agreement number F39502-99-1-0512.
 */

/* OPENBSD ORIGINAL: lib/libc/gen/readpassphrase.c */

#include "includes.h"

#ifndef HAVE_READPASSPHRASE

#include <termios.h>
#include <signal.h>
#include <ctype.h>
#include <fcntl.h>
#include <readpassphrase.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#ifndef TCSASOFT
/* If we don't have TCSASOFT define it so that ORing it it below is a no-op. */
# define TCSASOFT 0
#endif

/* SunOS 4.x which lacks _POSIX_VDISABLE, but has VDISABLE */
#if !defined(_POSIX_VDISABLE) && defined(VDISABLE)
#  define _POSIX_VDISABLE       VDISABLE
#endif

static volatile sig_atomic_t signo[_NSIG];

static void handler(int);

char *
readpassphrase(const char *prompt, char *buf, size_t bufsiz, int flags)
{
	ssize_t nr;
	int input, output, save_errno, i, need_restart;
	char ch, *p, *end;
	struct termios term, oterm;
	struct sigaction sa, savealrm, saveint, savehup, savequit, saveterm;
	struct sigaction savetstp, savettin, savettou, savepipe;

	/* I suppose we could alloc on demand in this case (XXX). */
	if (bufsiz == 0) {
		errno = EINVAL;
		return(NULL);
	}

restart:
	for (i = 0; i < _NSIG; i++) {
		if (signo[i]) {
#ifdef PSCAL_TARGET_IOS
			/* In shared process model, do not kill the process. Just continue/fail. */
			errno = EINTR;
#else
			kill(getpid(), i);
#endif
			switch (i) {
			case SIGTSTP:
			case SIGTTIN:
			case SIGTTOU:
				need_restart = 1;
			}
		}
	}
	if (need_restart)
		goto restart;

	if (save_errno)
		errno = save_errno;
	return(nr == -1 ? NULL : buf);
}
DEF_WEAK(readpassphrase);

#if 0
char *
getpass(const char *prompt)
{
	static char buf[_PASSWORD_LEN + 1];

	return(readpassphrase(prompt, buf, sizeof(buf), RPP_ECHO_OFF));
}
#endif

static void handler(int s)
{

	signo[s] = 1;
}
#endif /* HAVE_READPASSPHRASE */
