/* tcaposix.c */

#ifdef FEATURE_RCSID
char id_tcaposix[] = "$Id: tcaposix.h,v 2.13 2003/01/26 19:41:36 steve Exp $";
#endif

#include <termios.h>
#include <signal.h>
#include <unistd.h>

/* HPUX does a "#define ttysize winsize".  Elvis doesn't like that. */
#undef ttysize


#if USE_PROTOTYPES
static void catchsig(int signo);
#endif


static struct termios	oldtermio;	/* original tty mode */
static struct termios	newtermio;	/* cbreak/noecho tty mode */
static ELVBOOL		pscali_no_ttyraw = ElvFalse;


/* this function is used to catch signals */
static void catchsig(int signo)
{
	caught = (1 << signo);
}

/* get the original tty state */
static void ttyinit2()
{
	/* get the old tty state */
	tcgetattr(ttykbd, &oldtermio);
	{
		const char *flag = getenv("PSCALI_NO_TTYRAW");
		pscali_no_ttyraw = (flag && *flag) ? ElvTrue : ElvFalse;
	}
}

/* switch to the tty state that elvis runs in */
void ttyraw(char *erasekey)
{
#ifdef SA_NOMASK
	struct sigaction newsa;

	/* arrange for signals to be caught */
	newsa.sa_handler = catchsig;
	newsa.sa_flags = (SA_INTERRUPT|SA_NOMASK); /* not one-shot */
#if 0
	newsa.sa_restorer = NULL;
#endif
	sigaction(SIGINT, &newsa, NULL);
# ifdef SIGWINCH
	sigaction(SIGWINCH, &newsa, NULL);
# endif
#else
	signal(SIGINT, catchsig);
# ifdef SIGWINCH
	signal(SIGWINCH, catchsig);
# endif
#endif
	signal(SIGQUIT, SIG_IGN);

	if (pscali_no_ttyraw)
	{
		if (erasekey)
		{
			*erasekey = ELVCTRL('H');
		}
		return;
	}

	/* switch to raw mode */
	ospeed = cfgetospeed(&oldtermio);
	*erasekey = oldtermio.c_cc[VERASE];
	newtermio = oldtermio;
	newtermio.c_iflag &= (IXON|IXOFF|ISTRIP|IGNBRK);
	newtermio.c_oflag &= ~OPOST;
	newtermio.c_lflag &= ISIG;
	newtermio.c_cc[VINTR] = ELVCTRL('C'); /* always use ^C for interrupts */
#ifdef NDEBUG
	newtermio.c_cc[VQUIT] = 0;
#endif
	newtermio.c_cc[VMIN] = 1;
	newtermio.c_cc[VTIME] = 0;
#  ifdef VSWTCH
	newtermio.c_cc[VSWTCH] = 0;
#  endif
#  ifdef VSWTC /* is this a bug in Linux's headers? */
	newtermio.c_cc[VSWTC] = 0;
#  endif
#  ifdef VSUSP
	newtermio.c_cc[VSUSP] = 0;
#  endif
#  ifdef VDSUSP
	newtermio.c_cc[VDSUSP] = 0;
#  endif
	tcsetattr(ttykbd, TCSADRAIN, &newtermio);
}

/* switch back to the original tty state */
void ttynormal()
{
	if (pscali_no_ttyraw)
	{
		return;
	}
	tcsetattr(ttykbd, TCSADRAIN, &oldtermio);
}

/* Read from keyboard with timeout.  For POSIX, we use VMIN/VTIME to implement
 * the timeout.  For no timeout, VMIN should be 1 and VTIME should be 0; for
 * timeout, VMIN should be 0 and VTIME should be the timeout value.
 */
int ttyread(char *buf, int len, int timeout)
{
	struct termios t, oldt;
	int	bytes;	/* number of bytes actually read */

	/* clear the "caught" variable */
	caught = 0;

	if (pscali_no_ttyraw)
	{
		return (int)read(ttykbd, buf, (unsigned)len);
	}

#ifndef SA_NOMASK
	/* make sure the signal catcher hasn't been reset */
	signal(SIGINT, catchsig);
# ifdef SIGWINCH
	signal(SIGWINCH, catchsig);
# endif
#endif

	/* arrange for timeout, and disable control chars */
	tcgetattr(ttykbd, &t);
	oldt = t;
	if (timeout)
	{
		t.c_cc[VMIN] = 0;
		t.c_cc[VTIME] = timeout;
	}
	else
	{
		t.c_cc[VMIN] = 1;
		t.c_cc[VTIME] = 0;
	}
	t.c_cc[VINTR] = t.c_cc[VQUIT] = t.c_cc[VSTART] = t.c_cc[VSTOP] = 0;
	tcsetattr(ttykbd, TCSANOW, &t);

	/* Perform the read. */
	bytes = read(ttykbd, buf, (unsigned)len);

	/* revert to previous mode */
	tcsetattr(ttykbd, TCSANOW, &oldt);

	/* return the number of bytes read */
	return bytes;
}
