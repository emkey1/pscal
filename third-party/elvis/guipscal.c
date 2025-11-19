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
	int     cmdRow;
	int     cmdCol;
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
static void psresetcmd(void) {
	if (!currentwin) {
		return;
	}
	currentwin->cmdRow = (currentwin->rows > 0) ? (currentwin->rows - 1) : 0;
	currentwin->cmdCol = 0;
}

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
	psresetcmd();
}

static int psinit(int argc, char **argv)
{
	(void)argc;
	(void)argv;
#ifdef FEATURE_RAM
	optpreset(o_session, toCHAR("ram"), OPT_LOCK);
#endif
	o_exrefresh = ElvTrue;
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
	psresetcmd();
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
	pscalTerminalMoveCursor(row, column);
}

static void psdraw(GUIWIN *gw, long fg, long bg, int bits, CHAR *text, int len)
{
	(void)gw;
	if (!currentwin || len <= 0 || !text)
	{
		return;
	}
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

static void pstextline(GUIWIN *gw, CHAR *text, int len)
{
	(void)gw;
	if (!currentwin || !text || len <= 0)
	{
		return;
	}
	if (currentwin->rows <= 0)
	{
		currentwin->rows = psResolveRows();
	}
	if (currentwin->columns <= 0)
	{
		currentwin->columns = psResolveColumns();
	}
	if (currentwin->cmdRow < 0 || currentwin->cmdRow >= currentwin->rows)
	{
		psresetcmd();
	}
	char dbg[128];
	int dbglen = len < 64 ? len : 64;
	snprintf(dbg, sizeof(dbg), "[guipscal] textline len=%d first=%.*s", len, dbglen, (char *)text);
	pscalRuntimeDebugLog(dbg);
	for (int i = 0; i < len; ++i)
	{
		int ch = (int)text[i] & 0xff;
		switch (ch)
		{
			case '\r':
				currentwin->cmdCol = 0;
				pscalTerminalMoveCursor(currentwin->cmdRow, currentwin->cmdCol);
				pscalTerminalClearEol(currentwin->cmdRow, currentwin->cmdCol);
				break;
			case '\n':
				if (currentwin->cmdRow + 1 < currentwin->rows)
				{
					currentwin->cmdRow++;
				}
				currentwin->cmdCol = 0;
				pscalTerminalMoveCursor(currentwin->cmdRow, currentwin->cmdCol);
				pscalTerminalClearEol(currentwin->cmdRow, currentwin->cmdCol);
				break;
			case '\b':
				if (currentwin->cmdCol > 0)
				{
					currentwin->cmdCol--;
					pscalTerminalMoveCursor(currentwin->cmdRow, currentwin->cmdCol);
					char blank = ' ';
					pscalTerminalRender(&blank, 1, currentwin->cmdRow, currentwin->cmdCol, 0, 0, 0);
					pscalTerminalMoveCursor(currentwin->cmdRow, currentwin->cmdCol);
				}
				break;
			case '\t': {
				int nextStop = ((currentwin->cmdCol / 8) + 1) * 8;
				while (currentwin->cmdCol < nextStop && currentwin->cmdCol < currentwin->columns)
				{
					char blank = ' ';
					pscalTerminalRender(&blank, 1, currentwin->cmdRow, currentwin->cmdCol, 0, 0, 0);
					currentwin->cmdCol++;
				}
				pscalTerminalMoveCursor(currentwin->cmdRow, currentwin->cmdCol);
				break;
			}
			default:
				if (ch >= 0x20)
				{
					char out = (char)ch;
					pscalTerminalRender(&out, 1, currentwin->cmdRow, currentwin->cmdCol, 0, 0, 0);
					if (currentwin->cmdCol + 1 < currentwin->columns)
					{
						currentwin->cmdCol++;
					}
					pscalTerminalMoveCursor(currentwin->cmdRow, currentwin->cmdCol);
				}
				break;
		}
	}
	currentwin->cursorRow = currentwin->cmdRow;
	currentwin->cursorCol = currentwin->cmdCol;
}

static ELVBOOL psmsg(GUIWIN *gw, MSGIMP imp, CHAR *text, int len)
{
	(void)gw;
	(void)len;
	if (!currentwin || imp != MSG_STATUS)
	{
		return ElvFalse;
	}
	if (currentwin->rows <= 0)
	{
		currentwin->rows = psResolveRows();
	}
	if (currentwin->columns <= 0)
	{
		currentwin->columns = psResolveColumns();
	}
	int row = currentwin->rows > 0 ? currentwin->rows - 1 : 0;
	currentwin->cmdRow = row;
	currentwin->cmdCol = 0;
	pscalTerminalMoveCursor(row, 0);
	pscalTerminalClearEol(row, 0);
	if (text && *text)
	{
		const char *narrow = tochar8(text);
		if (narrow && *narrow)
		{
			int outlen = (int)strlen(narrow);
			if (outlen > currentwin->columns)
			{
				outlen = currentwin->columns;
			}
			if (outlen > 0)
			{
				pscalTerminalRender(narrow, outlen, row, 0, 0, 0, 0);
				currentwin->cmdCol = outlen;
				if (currentwin->cmdCol >= currentwin->columns)
				{
					currentwin->cmdCol = currentwin->columns ? currentwin->columns - 1 : 0;
				}
			}
		}
	}
	currentwin->cursorRow = row;
	pscalTerminalMoveCursor(row, currentwin->cmdCol);
	currentwin->cursorCol = currentwin->cmdCol;
	return ElvTrue;
}

static ELVBOOL psstatus(GUIWIN *gw, CHAR *left, long line, long column, _CHAR_ key, char *mode)
{
	(void)gw;
	(void)line;
	(void)column;
	(void)key;
	(void)mode;
	if (!currentwin) {
		return ElvTrue;
	}
	WINDOW win = winofgw(gw);
	if (win && win->state && (win->state->flags & ELVIS_BOTTOM)) {
		return ElvTrue;
	}
	if (currentwin->rows <= 0) {
		currentwin->rows = psResolveRows();
	}
	int row = currentwin->rows > 0 ? currentwin->rows - 1 : 0;
	currentwin->cmdRow = row;
	currentwin->cmdCol = 0;
	pscalTerminalMoveCursor(row, 0);
	pscalTerminalClearEol(row, 0);
	if (left && *left) {
		const char *narrow = tochar8(left);
		if (narrow) {
			int len = (int)strlen(narrow);
			if (len > currentwin->columns) {
				len = currentwin->columns;
			}
			pscalTerminalRender(narrow, len, row, 0, 0, 0, 0);
			currentwin->cmdCol = len;
		}
	}
	currentwin->cursorRow = row;
	currentwin->cursorCol = currentwin->cmdCol;
	pscalTerminalMoveCursor(row, currentwin->cmdCol);
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
	pstextline,
	psbeep,
	psmsg,
	NULL,		/* scrollbar */
	psstatus,
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
