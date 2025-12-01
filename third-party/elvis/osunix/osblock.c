/* unix/osblock.c */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#ifndef S_ISDIR
# define S_ISDIR(mode)	((mode & 0170000) == 0040000)
#endif
#include "elvis.h"
#include <limits.h>
#ifdef FEATURE_RCSID
char id_osblock[] = "$Id: osblock.c,v 2.30 2003/10/17 17:41:23 steve Exp $";
#endif
#ifndef DEFAULT_SESSION
# define DEFAULT_SESSION "%s/elvis%d.ses"
#endif
#ifndef F_OK
# define F_OK	0
#endif
#ifndef W_OK
# define W_OK	2
#endif
#ifndef O_BINARY
# ifdef _O_BINARY
#  define O_BINARY _O_BINARY
# else
#  define O_BINARY 0
# endif
#endif


static int fd = -1; /* file descriptor of the session file */
#ifdef PSCAL_TARGET_IOS
/* iOS sandboxed storage causes session files to behave poorly; keep session
 * state entirely in memory so elvis never touches the filesystem.
 */
static BLK **iosblk;
static int iosnblks;
static void iosblkreset(void) {
	if (iosnblks > 0 && iosblk) {
		for (int i = 0; i < iosnblks; i++) {
			free(iosblk[i]);
		}
		free(iosblk);
	}
	iosblk = NULL;
	iosnblks = 0;
}
#endif
#ifdef FEATURE_RAM
static BLK **blklist;
static int nblks;

static void blkreset(void) {
	if (nblks > 0) {
		for (int i = 0; i < nblks; i++) {
			if (blklist[i]) {
				free(blklist[i]);
			}
		}
		free(blklist);
		blklist = NULL;
		nblks = 0;
	}
}
#endif
/* This function creates a new block file, and returns ElvTrue if successful,
 * or ElvFalse if failed because the file was already busy.
 */
ELVBOOL blkopen(ELVBOOL force, BLK *buf) {
static char	dfltname[1024];
	char	dir[1024];
	struct stat st;
	int	i, j;
	long	oldcount;

#ifdef PSCAL_TARGET_IOS
	(void)force;
	/* Reset all session state and force a fresh temporary in-memory session. */
	iosblkreset();
	iosnblks = 1024;
	iosblk = (BLK **)calloc(iosnblks, sizeof(BLK *));
	if (!iosblk) {
		msg(MSG_FATAL, "no memory for session");
	}
	iosblk[0] = (BLK *)malloc(o_blksize);
	if (!iosblk[0]) {
		msg(MSG_FATAL, "no memory for session");
	}
	memcpy(iosblk[0], buf, o_blksize);
	buf->super.inuse = getpid();
	fd = -1;
	o_session = NULL;
	o_sessionpath = NULL;
	o_recovering = ElvFalse;
	o_tempsession = ElvTrue;
	o_newsession = ElvTrue;
	return ElvTrue;
#endif

#ifdef FEATURE_RAM
	if (o_session && !CHARcmp(o_session, toCHAR("ram"))) {
		nblks = 1024;
		blklist = (BLK **)calloc(nblks, sizeof(BLK *));
		blklist[0] = (BLK *)malloc(o_blksize);
		memcpy(blklist[0], buf, o_blksize);
		buf->super.inuse = getpid();
		fd = -1;
		o_tempsession = ElvTrue;
		return ElvTrue;
	}
#endif

	/* If no session file was explicitly requested, try successive
	 * defaults until we find an existing file (if we're trying to
	 * recover) or a non-existent file (if we're not trying to recover).
	 */
	if (!o_session) {
		/* search through sessionpath for a writable directory */
		if (!o_sessionpath)
			o_sessionpath = toCHAR("~:.");
		size_t dircap = sizeof(dir);
		for (i = 0, *dir = '\0'; o_sessionpath[i] && !*dir; ) {
			/* copy next name from o_sessionpath to dfltname */
			j = 0;
			if (o_sessionpath[i] == '~' && !elvalnum(o_sessionpath[i + 1])) {
				const char *home = tochar8(o_home);
				size_t homelen = home ? strlen(home) : 0;
				if (homelen >= dircap) homelen = dircap - 1;
				memcpy(dir, home, homelen);
				j = (int)homelen;
				i++;
			}
			while (o_sessionpath[i] && o_sessionpath[i] != ':') {
				if ((size_t)j + 1 >= dircap) {
					/* segment too long; skip this entry */
					while (o_sessionpath[i] && o_sessionpath[i] != ':') {
						i++;
					}
					j = 0;
					break;
				}
				dir[j++] = o_sessionpath[i++];
			}
			dir[j] = '\0';
			if (j == 0)
				snprintf(dir, dircap, "%s", ".");
			if (o_sessionpath[i] == ':')
				i++;

			/* If not writable directory, forget it */
			if (stat(dir, &st) != 0
			 || !S_ISDIR(st.st_mode)
			 || !(st.st_uid == geteuid()
			 	? (st.st_mode & S_IRWXU) == S_IRWXU
			 	: st.st_gid == getegid()
			 		? (st.st_mode & S_IRWXG) == S_IRWXG
			 		: (st.st_mode & S_IRWXO) == S_IRWXO)) {
				*dir = '\0';
			}
		}
		if (!*dir) {
			msg(MSG_FATAL, "set SESSIONPATH to a writable directory");
		}
		if (!o_directory) {
			optpreset(o_directory, CHARkdup(toCHAR(dir)), OPT_FREE);
		}

		/* choose the name of a session file */
		i = 1;
		oldcount = 0;
		do {
			/* protect against trying a ridiculous number of names */
			if (i >= 1000) {
				msg(MSG_FATAL, o_recovering
					? "[s]no session file found in $1"
					: "[s]too many session files in $1", dir);
			}
			snprintf(dfltname, sizeof(dfltname), DEFAULT_SESSION, dir, i++);

			/* if the file exists and is writable by this user,
			 * and we aren't recovering, then remember it so we
			 * can print a warning later, so the user will know
			 * he should delete it or recover it eventually.
			 */
			if (!o_recovering && access(dfltname, W_OK) == 0) {
				oldcount++;
			}

			/* if user wants to cancel, then fail */
			if (chosengui->poll && (*chosengui->poll)(ElvFalse)) {
				return ElvFalse;
			}
		} while (o_recovering ? (access(dfltname, F_OK) != 0)
				      : ((fd = open(dfltname, O_RDWR|O_CREAT|O_EXCL|O_BINARY, 0600)) < 0));
		o_session = toCHAR(dfltname);
		o_tempsession = ElvTrue;
		if (oldcount > 0)
			msg(MSG_WARNING, "[d]skipping $1 old session file($1!=1?\"s\")", oldcount);
	}

	/* Try to open the session file (if not opened in the above loop) */
	if (fd < 0 && (fd = open(tochar8(o_session), O_RDWR|O_BINARY)) >= 0) {
		/* we're opening an existing session -- definitely not temporary */
		o_tempsession = ElvFalse;
	} else {
		/* either we're about to open an existing session that was
		 * explicitly named via "-f session", or we have already
		 * created a temporary session and just need to initialize it.
		 */

		/* if we don't have a temp session already open, then we must
		 * want to create the session file now.
		 */
		if (fd < 0 && errno == ENOENT)
			fd = open(tochar8(o_session), O_RDWR|O_CREAT|O_EXCL|O_BINARY, 0600);
		if (fd < 0)
			msg(MSG_FATAL, "no such session");

		/* either way, we now have an open session.  Initialize it! */
		o_newsession = ElvTrue;
		if (write(fd, (char *)buf, (unsigned)o_blksize) < o_blksize) {
			close(fd);
			unlink(tochar8(o_session));
			fd = -1;
			errno = ENOSPC;
		} else {
			lseek(fd, 0L, 0);
		}
	}

	/* if elvis runs other programs, they shouldn't inherit this fd */
	fcntl(fd, F_SETFL, 1);

	/* Read the first block & mark the session file as being "in use".
	 * If already marked as "in use" and !force, then fail.
	 */
	/* lockf(fd, LOCK, sizeof buf->super); */
	if (read(fd, (char *)buf, sizeof buf->super) != sizeof buf->super) {
		msg(MSG_FATAL, "blkopen's read failed");
	}
	if (buf->super.inuse && !force) {
#ifdef PSCALI_IGNORE_SESSION_LOCKS
		buf->super.inuse = getpid();
#else
		/* lockf(fd, ULOCK, o_blksize); */
		return ElvFalse;
#endif
	}
	buf->super.inuse = getpid();
	lseek(fd, 0L, 0);
	(void)write(fd, (char *)buf, sizeof buf->super);
	/* lockf(fd, ULOCK, o_blksize); */

	/* done! */
	return ElvTrue;
}


/* This function closes the session file, given its handle */
void blkclose(BLK *buf) {
#ifdef PSCAL_TARGET_IOS
	(void)buf;
	iosblkreset();
	o_session = NULL;
	o_sessionpath = NULL;
	o_recovering = ElvFalse;
	return;
#endif
#ifdef FEATURE_RAM
	if (nblks > 0) {
		blkreset();
	}
#endif
	if (fd < 0)
		return;
	blkread(buf, 0);
	buf->super.inuse = 0L;
	blkwrite(buf, 0);
	close(fd);
	fd = -1;
	if (o_tempsession) {
		unlink(tochar8(o_session));
	}
#ifdef FEATURE_RAM
	blkreset();
#endif
}

/* Write the contents of buf into record # blkno, for the block file
 * identified by blkhandle.  Blocks are numbered starting at 0.  The
 * requested block may be past the end of the file, in which case
 * this function is expected to extend the file.
 */
void blkwrite(BLK *buf, _BLKNO_ blkno)
{
#ifdef PSCAL_TARGET_IOS
	if (iosnblks > 0) {
		if (blkno >= iosnblks) {
			int newcount = iosnblks;
			while (blkno >= newcount) {
				newcount += 1024;
			}
			BLK **tmp = (BLK **)realloc(iosblk, newcount * sizeof(BLK *));
			if (!tmp) {
				msg(MSG_FATAL, "blkwrite failed");
			}
			memset(&tmp[iosnblks], 0, (newcount - iosnblks) * sizeof(BLK *));
			iosblk = tmp;
			iosnblks = newcount;
		}
		if (!iosblk[blkno]) {
			iosblk[blkno] = (BLK *)malloc(o_blksize);
			if (!iosblk[blkno]) {
				msg(MSG_FATAL, "blkwrite failed");
			}
		}
		memcpy(iosblk[blkno], buf, o_blksize);
		return;
	}
#endif
#ifdef FEATURE_RAM
	/* store it in RAM */
	if (nblks > 0) {
		if (blkno >= nblks) {
			blklist = (BLK **)realloc(blklist,
						(nblks + 1024) * sizeof(BLK *));
			memset(&blklist[nblks], 0, 1024 * sizeof(BLK *));
			nblks += 1024;
		}
		if (!blklist[blkno])
			blklist[blkno] = malloc(o_blksize);
		memcpy(blklist[blkno], buf, o_blksize);
		return;
	}
#endif

	/* write the block */
	lseek(fd, (off_t)blkno * (off_t)o_blksize, 0);
	if (write(fd, (char *)buf, (size_t)o_blksize) != o_blksize) {
		msg(MSG_FATAL, "blkwrite failed");
	}
}

/* Read the contends of record # blkno into buf, for the block file
 * identified by blkhandle.  The request block will always exist;
 * it will never be beyond the end of the file.
 */
void blkread(BLK *buf, _BLKNO_ blkno) {
#ifdef PSCAL_TARGET_IOS
	if (iosnblks > 0) {
		if (blkno >= iosnblks) {
			int newcount = iosnblks;
			while (blkno >= newcount) {
				newcount += 1024;
			}
			BLK **tmp = (BLK **)realloc(iosblk, newcount * sizeof(BLK *));
			if (!tmp) {
				msg(MSG_FATAL, "[d]blkread($1) failed", (int)blkno);
			}
			memset(&tmp[iosnblks], 0, (newcount - iosnblks) * sizeof(BLK *));
			iosblk = tmp;
			iosnblks = newcount;
		}
		if (!iosblk[blkno]) {
			iosblk[blkno] = (BLK *)calloc(1, o_blksize);
			if (!iosblk[blkno]) {
				msg(MSG_FATAL, "[d]blkread($1) failed", (int)blkno);
			}
		}
		memcpy(buf, iosblk[blkno], o_blksize);
		return;
	}
#endif
	/* read the block */
	lseek(fd, (off_t)blkno * o_blksize, 0);
	ssize_t nread = read(fd, (char *)buf, (size_t)o_blksize);
	if (nread == (ssize_t)o_blksize) {
		return;
	}
	if (nread < 0) {
		msg(MSG_FATAL, "[d]blkread($1) failed", (int)blkno);
	}
	/* Short reads can occur on iOS if a session file is truncated by the
	 * sandbox; pad with zeros instead of aborting so the editor remains usable.
	 */
#ifdef PSCAL_TARGET_IOS
	if (nread >= 0 && nread < (ssize_t)o_blksize) {
		memset(((char *)buf) + nread, 0, (size_t)(o_blksize - nread));
		return;
	}
#endif
	msg(MSG_FATAL, "[d]blkread($1) failed", (int)blkno);
}

/* Force changes out to disk.  Ideally we would only force the session file's
 * blocks out to the disk, but UNIX doesn't offer a way to do that, so we
 * force them all out.  Major bummer.
 */
void blksync(void) {
#ifdef PSCAL_TARGET_IOS
	return;
#endif
#ifdef FEATURE_RAM
	if (nblks > 0)
		return;
#endif

	sync();
}
