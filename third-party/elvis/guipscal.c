/* guipscal.c */
/* iOS/iPadOS-friendly GUI that talks to PSCAL TerminalBuffer */

#include "elvis.h"
#ifdef FEATURE_RCSID
char id_guipscal[] = "$Id: guipscal.c, PSCAL integration $";
#endif

#ifdef GUI_PSCAL

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

extern void pscalRuntimeDebugLog(const char *message);

typedef struct {
	int	rows;
	int	columns;
	int	cursorRow;
	int	cursorCol;
	ELVCURSOR shape;
} PSCALTWIN;

static PSCALTWIN mainwin;
static PSCALTWIN *currentwin;
static ELVBOOL running = ElvFalse;

extern void pscalTerminalBegin(int columns, int rows);
extern void pscalTerminalEnd(void);
extern void pscalTerminalResize(int columns, int rows);
extern void pscalTerminalRender(const char *utf8, int len, int row, int col, long fg, long bg, int attr);
extern void pscalTerminalClear(void);
extern void pscalTerminalClearEol(int row, int col);
extern void pscalTerminalMoveCursor(int row, int col);
extern int pscalTerminalRead(uint8_t *buffer, int maxlen, int timeout);

static int pstest(void)
{
	return 1;
}

static int psResolveRows(void) {
	int rows = (int)o_ttyrows;
	if (rows <= 0) {
		const char *env = getenv("LINES");
		if (env && *env) {
			rows = atoi(env);
		}
	}
	if (rows <= 0) rows = 24;
	return rows;
}

static int psResolveColumns(void) {
	int cols = (int)o_ttycolumns;
	if (cols <= 0) {
		const char *env = getenv("COLUMNS");
		if (env && *env) {
			cols = atoi(env);
		}
	}
	if (cols <= 0) cols = 80;
	return cols;
}

static void psreset(void)
{
	if (!currentwin)
	{
		return;
	}
	mainwin.rows = psResolveRows();
	mainwin.columns = psResolveColumns();
	pscalTerminalResize(mainwin.columns, mainwin.rows);
}

static int psinit(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	optpreset(o_session, toCHAR("ram"), OPT_LOCK);
	mainwin.rows = psResolveRows();
	mainwin.columns = psResolveColumns();
	mainwin.cursorRow = 0;
	mainwin.cursorCol = 0;
	running = ElvTrue;
	char buf[160];
	snprintf(buf, sizeof(buf), "[guipscal] psinit rows=%d cols=%d o_ttyrows=%ld o_ttycols=%ld",
		mainwin.rows, mainwin.columns, (long)o_ttyrows, (long)o_ttycolumns);
	pscalRuntimeDebugLog(buf);
	pscalTerminalBegin(mainwin.columns, mainwin.rows);
	return argc;
}

static void psterm(void)
{
	running = ElvFalse;
	pscalRuntimeDebugLog("[guipscal] psterm");
	pscalTerminalEnd();
}

static ELVBOOL pscreategw(char *name, char *firstcmd)
{
	pscalRuntimeDebugLog("[guipscal] pscreategw");
	char logbuf[128];
	if (currentwin)
	{
		pscalRuntimeDebugLog("[guipscal] pscreategw rejected: currentwin already active");
		return ElvFalse;
	}

	currentwin = &mainwin;
	currentwin->rows = psResolveRows();
	currentwin->columns = psResolveColumns();
	currentwin->cursorRow = 0;
	currentwin->cursorCol = 0;
	currentwin->shape = CURSOR_NONE;

	if (!eventcreate((GUIWIN *)currentwin, NULL, name, currentwin->rows, currentwin->columns))
	{
		pscalRuntimeDebugLog("[guipscal] pscreategw eventcreate failed");
		currentwin = NULL;
		return ElvFalse;
	}
	snprintf(logbuf, sizeof(logbuf), "[guipscal] pscreategw created rows=%d cols=%d",
		currentwin->rows, currentwin->columns);
	pscalRuntimeDebugLog(logbuf);

	pscalTerminalClear();
	eventfocus((GUIWIN *)currentwin, ElvTrue);

	if (firstcmd)
	{
		winoptions(winofgw((GUIWIN *)currentwin));
		exstring(windefault, toCHAR(firstcmd), "+cmd");
	}

	return ElvTrue;
}

static void psdestroygw(GUIWIN *gw, ELVBOOL force)
{
	(void)force;
	if (gw)
	{
		eventdestroy(gw);
	}
	currentwin = NULL;
}

static void psbeep(GUIWIN *gw)
{
	(void)gw;
}

static void psmoveto(GUIWIN *gw, int column, int row)
{
	(void)gw;
	char logbuf[160];
	if (!currentwin)
	{
		return;
	}
	if (column < 0)
		column = 0;
	if (row < 0)
		row = 0;
	if (row >= currentwin->rows)
		row = currentwin->rows - 1;
	if (column >= currentwin->columns)
		column = currentwin->columns - 1;
	currentwin->cursorCol = column;
	currentwin->cursorRow = row;
	snprintf(logbuf, sizeof(logbuf), "[guipscal] psmoveto row=%d col=%d (rows=%d cols=%d)",
		row, column, currentwin->rows, currentwin->columns);
	pscalRuntimeDebugLog(logbuf);
	pscalTerminalMoveCursor(row, column);
}

static void psdraw(GUIWIN *gw, long fg, long bg, int bits, CHAR *text, int len)
{
	(void)gw;
	char logbuf[160];
	if (!currentwin || len <= 0 || !text)
	{
		return;
	}
	snprintf(logbuf, sizeof(logbuf), "[guipscal] psdraw row=%d col=%d len=%d", currentwin->cursorRow, currentwin->cursorCol, len);
	pscalRuntimeDebugLog(logbuf);
	pscalTerminalRender((const char *)text, len,
			currentwin->cursorRow, currentwin->cursorCol, fg, bg, bits);
	currentwin->cursorCol += len;
	if (currentwin->cursorCol >= currentwin->columns)
	{
		currentwin->cursorCol = currentwin->columns ? currentwin->columns - 1 : 0;
	}
}

static ELVBOOL psclrtoeol(GUIWIN *gw)
{
	(void)gw;
	if (!currentwin)
	{
		return ElvFalse;
	}
	pscalTerminalClearEol(currentwin->cursorRow, currentwin->cursorCol);
	return ElvTrue;
}

static void psloop(void)
{
	uint8_t rawbuf[32];
	CHAR keybuf[32];
	MAPSTATE mst = MAP_CLEAR;

	pscalRuntimeDebugLog("[guipscal] psloop starting");
	if (mainfirstcmd(windefault))
	{
		pscalRuntimeDebugLog("[guipscal] mainfirstcmd handled command, exiting loop");
		return;
	}

	while (running && currentwin)
	{
		pscalRuntimeDebugLog("[guipscal] psloop tick");
		currentwin->shape = eventdraw((GUIWIN *)currentwin);
		char logbuf[128];
		snprintf(logbuf, sizeof(logbuf), "[guipscal] eventdraw shape=%d row=%d col=%d",
			(int)currentwin->shape, currentwin->cursorRow, currentwin->cursorCol);
		pscalRuntimeDebugLog(logbuf);
		int readlen = pscalTerminalRead(rawbuf, (int)sizeof(rawbuf),
					   (mst == MAP_CLEAR) ? 0 : 2);
		snprintf(logbuf, sizeof(logbuf), "[guipscal] psloop readlen=%d mst=%d",
			readlen, (int)mst);
		pscalRuntimeDebugLog(logbuf);
		if (readlen <= 0)
		{
			continue;
		}
		if (readlen > (int)QTY(keybuf))
		{
			readlen = (int)QTY(keybuf);
		}
		for (int i = 0; i < readlen; i++)
		{
			keybuf[i] = (CHAR)rawbuf[i];
		}
		mst = eventkeys((GUIWIN *)currentwin, keybuf, readlen);
	}
}

GUI guipscal =
{
	"pscal",	/* name */
	"PSCAL integrated terminal",
	ElvFalse,	/* exonly */
	ElvFalse,	/* newblank */
	ElvTrue,	/* minimizeclr */
	ElvTrue,	/* scrolllast */
	ElvFalse,	/* shiftrows */
	2,		/* movecost */
	0,		/* opts */
	NULL,		/* optdescs */
	pstest,
	psinit,
	NULL,		/* usage */
	psloop,
	NULL,		/* poll */
	psterm,
	pscreategw,
	psdestroygw,
	NULL,		/* focusgw */
	NULL,		/* retitle */
	psreset,
	NULL,		/* flush */
	psmoveto,
	psdraw,
	NULL,		/* shift */
	NULL,		/* scroll */
	psclrtoeol,
	NULL,		/* textline */
	psbeep,
	NULL,		/* msg */
	NULL,		/* scrollbar */
	NULL,		/* status */
	NULL,		/* keylabel */
	NULL,		/* clipopen */
	NULL,		/* clipwrite */
	NULL,		/* clipread */
	NULL,		/* clipclose */
	coloransi,	/* color */
	NULL,		/* freecolor */
	NULL,		/* setbg */
	NULL,		/* guicmd */
	NULL,		/* tabcmd */
	NULL,		/* save */
	NULL,		/* wildcard */
	NULL,		/* prgopen */
	NULL,		/* prgclose */
	NULL		/* stop */
};

#endif /* GUI_PSCAL */
