/* $OpenBSD: atomicio.c,v 1.30 2019/01/24 02:42:23 dtucker Exp $ */
/*
 * Copyright (c) 2006 Damien Miller. All rights reserved.
 * Copyright (c) 2005 Anil Madhavapeddy. All rights reserved.
 * Copyright (c) 1995,1999 Theo de Raadt.  All rights reserved.
 * All rights reserved.
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

#include <sys/uio.h>

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "atomicio.h"

#ifdef PSCAL_TARGET_IOS
static int
pscal_ios_atomicio_translate_fd(int fd)
{
	VProc *vp = vprocCurrent();
	if (vp == NULL)
		return fd;
	int host_fd = vprocTranslateFd(vp, fd);
	if (host_fd >= 0)
		return host_fd;
	return fd;
}

static ssize_t
pscal_ios_atomicio_read_cb(int fd, void *buf, size_t n)
{
	return pscal_ios_read(fd, buf, n);
}

static ssize_t
pscal_ios_atomicio_write_cb(int fd, void *buf, size_t n)
{
	return pscal_ios_write(fd, buf, n);
}

static ssize_t
pscal_ios_atomicio_readv_cb(int fd, const struct iovec *iov, int iovcnt)
{
	return readv(pscal_ios_atomicio_translate_fd(fd), iov, iovcnt);
}

static ssize_t
pscal_ios_atomicio_writev_cb(int fd, const struct iovec *iov, int iovcnt)
{
	return writev(pscal_ios_atomicio_translate_fd(fd), iov, iovcnt);
}
#endif /* PSCAL_TARGET_IOS */

/*
 * ensure all of data on socket comes through. f==read || f==vwrite
 */
size_t
atomicio6(ssize_t (*f) (int, void *, size_t), int fd, void *_s, size_t n,
    int (*cb)(void *, size_t), void *cb_arg)
{
	char *s = _s;
	size_t pos = 0;
	ssize_t res;
	struct pollfd pfd;
	int expect_read = 0;

#ifdef PSCAL_TARGET_IOS
	ssize_t (*raw_write_fn)(int, void *, size_t) =
	    (ssize_t (*)(int, void *, size_t))write;
	if (f == read) {
		f = pscal_ios_atomicio_read_cb;
		expect_read = 1;
	} else if (f == raw_write_fn) {
		f = pscal_ios_atomicio_write_cb;
		expect_read = 0;
	} else {
		expect_read = (f == read);
	}
#else
	expect_read = (f == read);
#endif

	pfd.fd = fd;
#ifndef BROKEN_READ_COMPARISON
	pfd.events = expect_read ? POLLIN : POLLOUT;
#else
	pfd.events = POLLIN|POLLOUT;
#endif
	while (n > pos) {
		res = (f) (fd, s + pos, n - pos);
		switch (res) {
		case -1:
			if (errno == EINTR) {
				/* possible SIGALARM, update callback */
				if (cb != NULL && cb(cb_arg, 0) == -1) {
					errno = EINTR;
					return pos;
				}
				continue;
			} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
				(void)poll(&pfd, 1, -1);
				continue;
			}
			return 0;
		case 0:
			errno = EPIPE;
			return pos;
		default:
			pos += (size_t)res;
			if (cb != NULL && cb(cb_arg, (size_t)res) == -1) {
				errno = EINTR;
				return pos;
			}
		}
	}
	return pos;
}

size_t
atomicio(ssize_t (*f) (int, void *, size_t), int fd, void *_s, size_t n)
{
	return atomicio6(f, fd, _s, n, NULL, NULL);
}

/*
 * ensure all of data on socket comes through. f==readv || f==writev
 */
size_t
atomiciov6(ssize_t (*f) (int, const struct iovec *, int), int fd,
    const struct iovec *_iov, int iovcnt,
    int (*cb)(void *, size_t), void *cb_arg)
{
	size_t pos = 0, rem;
	ssize_t res;
	struct iovec iov_array[IOV_MAX], *iov = iov_array;
	struct pollfd pfd;
	int expect_read = 0;

#ifdef PSCAL_TARGET_IOS
	ssize_t (*raw_writev_fn)(int, const struct iovec *, int) =
	    (ssize_t (*)(int, const struct iovec *, int))writev;
	if (f == readv) {
		f = pscal_ios_atomicio_readv_cb;
		expect_read = 1;
	} else if (f == raw_writev_fn) {
		f = pscal_ios_atomicio_writev_cb;
		expect_read = 0;
	} else {
		expect_read = (f == readv);
	}
#else
	expect_read = (f == readv);
#endif

	if (iovcnt < 0 || iovcnt > IOV_MAX) {
		errno = EINVAL;
		return 0;
	}
	/* Make a copy of the iov array because we may modify it below */
	memcpy(iov, _iov, (size_t)iovcnt * sizeof(*_iov));

	pfd.fd = fd;
#ifndef BROKEN_READV_COMPARISON
	pfd.events = expect_read ? POLLIN : POLLOUT;
#else
	pfd.events = POLLIN|POLLOUT;
#endif
	for (; iovcnt > 0 && iov[0].iov_len > 0;) {
		res = (f) (fd, iov, iovcnt);
		switch (res) {
		case -1:
			if (errno == EINTR) {
				/* possible SIGALARM, update callback */
				if (cb != NULL && cb(cb_arg, 0) == -1) {
					errno = EINTR;
					return pos;
				}
				continue;
			} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
				(void)poll(&pfd, 1, -1);
				continue;
			}
			return 0;
		case 0:
			errno = EPIPE;
			return pos;
		default:
			rem = (size_t)res;
			pos += rem;
			/* skip completed iov entries */
			while (iovcnt > 0 && rem >= iov[0].iov_len) {
				rem -= iov[0].iov_len;
				iov++;
				iovcnt--;
			}
			/* This shouldn't happen... */
			if (rem > 0 && (iovcnt <= 0 || rem > iov[0].iov_len)) {
				errno = EFAULT;
				return 0;
			}
			if (iovcnt == 0)
				break;
			/* update pointer in partially complete iov */
			iov[0].iov_base = ((char *)iov[0].iov_base) + rem;
			iov[0].iov_len -= rem;
		}
		if (cb != NULL && cb(cb_arg, (size_t)res) == -1) {
			errno = EINTR;
			return pos;
		}
	}
	return pos;
}

size_t
atomiciov(ssize_t (*f) (int, const struct iovec *, int), int fd,
    const struct iovec *_iov, int iovcnt)
{
	return atomiciov6(f, fd, _iov, iovcnt, NULL, NULL);
}
