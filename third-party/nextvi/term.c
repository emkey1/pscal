static struct termios termios;
sbuf *term_sbuf;
int term_record;
int xrows, xcols;
unsigned int ibuf_pos, ibuf_cnt, ibuf_sz = 128, icmd_pos;
unsigned char *ibuf, icmd[4096];
unsigned int texec, tn;
#if !defined(PSCAL_TARGET_IOS)
static struct pollfd ufds[1];
#endif

/* iOS bridge for floating window rendering */
#if defined(PSCAL_TARGET_IOS)
extern void pscalTerminalBegin(int columns, int rows);
extern void pscalTerminalEnd(void);
extern void pscalTerminalRender(const char *utf8, int len, int row, int col, long fg, long bg, int attr);
extern void pscalTerminalClear(void);
extern void pscalTerminalMoveCursor(int row, int col);
extern void pscalTerminalClearEol(int row, int col);
extern void pscalTerminalClearBol(int row, int col);
extern void pscalTerminalClearLine(int row);
extern void pscalTerminalClearScreenFromCursor(int row, int col);
extern void pscalTerminalClearScreenToCursor(int row, int col);
extern void pscalTerminalInsertChars(int row, int col, int count);
extern void pscalTerminalDeleteChars(int row, int col, int count);
extern void pscalTerminalEnterAltScreen(void);
extern void pscalTerminalExitAltScreen(void);
extern void pscalTerminalSetCursorVisible(int visible);
extern void pscalTerminalInsertLines(int row, int count);
extern void pscalTerminalDeleteLines(int row, int count);
extern int pscalTerminalRead(unsigned char *buffer, int maxlen, int timeout_ms);
static int ios_row = 0;
static int ios_col = 0;
static int ios_wrap = 1;
static int ios_fg = -1;
static int ios_bg = -1;
static int ios_attr = 0; /* bit0=bold, bit1=underline, bit2=inverse, bit3=blink, bit4=faint, bit5=italic, bit6=strike */
static int ios_margin_top = 0;
static int ios_margin_bottom = 0;
static int ios_origin_mode = 0;
static int ios_wrap_mode = 1;
static int ios_saved_row = 0;
static int ios_saved_col = 0;
static int ios_tab_width = 8;
static unsigned char ios_tabs[256];
static int ios_bracketed_paste = 0;
static int ios_mouse_tracking = 0;
static FILE *ios_dump_fp = NULL;

static void ios_sync_cursor(void) {
	if (ios_row < 0) ios_row = 0;
	if (ios_row >= xrows) ios_row = xrows - 1;
	if (ios_col < 0) ios_col = 0;
	if (ios_col >= xcols) ios_col = xcols - 1;
	pscalTerminalMoveCursor(ios_row, ios_col);
}

static void ios_enforce_row_bounds(void) {
	if (ios_row < ios_margin_top) {
		ios_row = ios_margin_top;
	} else if (ios_row > ios_margin_bottom) {
		ios_row = ios_margin_bottom;
	}
}
enum {
	IOS_ATTR_BOLD = 1 << 0,
	IOS_ATTR_UNDER = 1 << 1,
	IOS_ATTR_INV = 1 << 2
};
static void ios_term_render_buf(const char *s, int n);
static void ios_tabs_reset(void) {
    int limit = xcols > 0 && xcols < (int)sizeof(ios_tabs) ? xcols : (int)sizeof(ios_tabs);
    for (int i = 0; i < limit; i++) {
        ios_tabs[i] = (i % ios_tab_width) == 0 ? 1 : 0;
    }
}

static void ios_term_reset(void) {
	ios_row = 0;
	ios_col = 0;
	ios_fg = -1;
	ios_bg = -1;
	ios_attr = 0;
	ios_margin_top = 0;
	ios_margin_bottom = xrows > 0 ? xrows - 1 : 0;
	ios_origin_mode = 0;
	ios_wrap_mode = 1;
	ios_saved_row = 0;
	ios_saved_col = 0;
	ios_tabs_reset();
	ios_bracketed_paste = 0;
	ios_mouse_tracking = 0;
	if (ios_dump_fp) {
		fclose(ios_dump_fp);
		ios_dump_fp = NULL;
	}
	ios_sync_cursor();
}

static void ios_scroll_region_up(void) {
	if (ios_margin_top < 0 || ios_margin_top >= xrows) {
		return;
	}
	pscalTerminalMoveCursor(ios_margin_top, 0);
	pscalTerminalDeleteLines(ios_margin_top, 1);
	ios_row = ios_margin_bottom;
	ios_col = 0;
	ios_sync_cursor();
}

static void ios_scroll_region_down(void) {
	if (ios_margin_top < 0 || ios_margin_top >= xrows) {
		return;
	}
	pscalTerminalMoveCursor(ios_margin_top, 0);
	pscalTerminalInsertLines(ios_margin_top, 1);
	ios_col = 0;
	ios_sync_cursor();
}
static void ios_term_render_char(char ch) {
	if (xcols <= 0 || xrows <= 0)
		return;
	if (ch == '\r') {
		ios_col = 0;
		ios_sync_cursor();
		return;
	}
	if (ch == '\n') {
		ios_col = 0;
		ios_row++;
		if (ios_row > ios_margin_bottom) {
			ios_row = ios_margin_bottom;
			ios_scroll_region_up();
		}
		ios_sync_cursor();
		return;
	}
	if (ch == '\b') {
		if (ios_col > 0)
			ios_col--;
		ios_sync_cursor();
		return;
	}
	if (ch == '\t') {
		int next = ios_col + 1;
		int limit = xcols < (int)sizeof(ios_tabs) ? xcols : (int)sizeof(ios_tabs);
		while (next < limit && !ios_tabs[next]) {
			next++;
		}
		if (next >= xcols) {
			next = xcols - 1;
		}
		ios_col = next;
		ios_sync_cursor();
		return;
	}
	if (ch == 0x08) { /* backspace already handled; keep fallback */ }
	pscalTerminalRender(&ch, 1, ios_row, ios_col, ios_fg, ios_bg, ios_attr);
	ios_col++;
	if (ios_wrap && ios_col >= xcols) {
		ios_col = 0;
		ios_row++;
		if (ios_row >= xrows)
			ios_row = xrows - 1;
		ios_sync_cursor();
	} else if (!ios_wrap && ios_col >= xcols) {
		ios_col = xcols - 1;
	}
}
static void ios_term_clear_line_from_cursor(void) {
	if (xcols <= 0 || xrows <= 0)
		return;
	for (int c = ios_col; c < xcols; c++) {
		pscalTerminalRender(" ", 1, ios_row, c, 0, 0, 0);
	}
}
#endif

#if defined(PSCAL_TARGET_IOS)
static void ios_term_write(const char *s, int n) {
	if (!s || n <= 0)
		return;
	if (!ios_dump_fp) {
		const char *dump_path = getenv("PSCALI_TERM_ESC_LOG");
		if (dump_path && *dump_path) {
			ios_dump_fp = fopen(dump_path, "ab");
		}
	}
	if (ios_dump_fp) {
		fprintf(ios_dump_fp, "CHUNK %d bytes: ", n);
		for (int idx = 0; idx < n; idx++) {
			fprintf(ios_dump_fp, "%02X ", (unsigned char)s[idx]);
		}
		fputc('\n', ios_dump_fp);
		fflush(ios_dump_fp);
	}
	write(1, s, n);
	int i = 0;
	while (i < n) {
		unsigned char ch = (unsigned char)s[i];
		/* Handle single ESC-prefixed sequences (no '[') */
		if (ch == 0x1B && i + 1 < n && s[i + 1] == ']') {
			/* OSC: ignore payload until BEL or ST */
			i += 2;
			while (i < n) {
				if (s[i] == '\a') { i++; break; }
				if (s[i] == 0x1B && i + 1 < n && s[i + 1] == '\\') { i += 2; break; }
				i++;
			}
			continue;
		}
		if (ch == 0x1B && i + 1 < n && s[i + 1] != '[') {
			unsigned char esc = (unsigned char)s[i + 1];
			if (esc == '7') { /* DECSC */
				ios_saved_row = ios_row;
				ios_saved_col = ios_col;
			} else if (esc == '8') { /* DECRC */
				ios_row = ios_saved_row;
				ios_col = ios_saved_col;
				ios_enforce_row_bounds();
				if (ios_col >= xcols) ios_col = xcols - 1;
				pscalTerminalMoveCursor(ios_row, ios_col);
			} else if (esc == 'D') { /* IND */
				ios_term_render_char('\n');
			} else if (esc == 'E') { /* NEL */
				ios_term_render_char('\r');
				ios_term_render_char('\n');
			} else if (esc == 'M') { /* RI */
				if (ios_row > ios_margin_top) {
					ios_row--;
				} else {
					ios_scroll_region_down();
				}
				ios_sync_cursor();
			} else if (esc == 'H') { /* HTS */
				if (ios_col >= 0 && ios_col < (int)sizeof(ios_tabs)) {
					ios_tabs[ios_col] = 1;
				}
			}
			i += 2;
			continue;
		}
		if (ch == 0x1B && i + 1 < n && s[i + 1] == '[') {
			i += 2;
			int nums[8] = {0};
			int numcnt = 0;
			int private_mode = 0;
			if (i < n && s[i] == '?') {
				private_mode = 1;
				i++;
			}
			while (i < n) {
				char c = s[i];
				if (c >= '0' && c <= '9') {
					if (numcnt < 8)
						nums[numcnt] = nums[numcnt] * 10 + (c - '0');
					i++;
					continue;
				}
				if (c == ';') {
					if (numcnt < 7)
						numcnt++;
					i++;
					continue;
				}
				int p1 = nums[0];
				int p2 = numcnt >= 1 ? nums[1] : 0;
				if (!private_mode) {
					if (c == 'H' || c == 'f') {
						int r = p1 > 0 ? p1 - 1 : 0;
						int ccol = p2 > 0 ? p2 - 1 : 0;
						if (ios_origin_mode) {
							r += ios_margin_top;
						}
						ios_row = r;
						ios_col = ccol;
						if (ios_row >= xrows) ios_row = xrows - 1;
						if (ios_col >= xcols) ios_col = xcols - 1;
						ios_sync_cursor();
					} else if (c == 'J') {
						int mode = p1;
						if (mode == 0) {
							pscalTerminalClearScreenFromCursor(ios_row, ios_col);
						} else if (mode == 1) {
							pscalTerminalClearScreenToCursor(ios_row, ios_col);
						} else {
							pscalTerminalClear();
							ios_term_reset();
						}
						ios_sync_cursor();
					} else if (c == 'K') {
						int mode = p1;
						if (mode == 0) {
							ios_term_clear_line_from_cursor();
						} else if (mode == 1) {
							pscalTerminalClearBol(ios_row, ios_col);
						} else {
							pscalTerminalClearLine(ios_row);
						}
						ios_sync_cursor();
					} else if (c == 'A') {
						int step = p1 > 0 ? p1 : 1;
						ios_row -= step;
						ios_enforce_row_bounds();
						ios_sync_cursor();
					} else if (c == 'B') {
						int step = p1 > 0 ? p1 : 1;
						ios_row += step;
						ios_enforce_row_bounds();
						ios_sync_cursor();
					} else if (c == 'C') {
						int step = p1 > 0 ? p1 : 1;
						ios_col += step;
						if (ios_col >= xcols) ios_col = xcols - 1;
						ios_sync_cursor();
					} else if (c == 'D') {
						int step = p1 > 0 ? p1 : 1;
						ios_col -= step;
						if (ios_col < 0) ios_col = 0;
						ios_sync_cursor();
					} else if (c == 'L') {
						int count = p1 > 0 ? p1 : 1;
						ios_enforce_row_bounds();
						pscalTerminalInsertLines(ios_row, count);
						ios_sync_cursor();
					} else if (c == 'M') {
						int count = p1 > 0 ? p1 : 1;
						ios_enforce_row_bounds();
						pscalTerminalDeleteLines(ios_row, count);
						ios_sync_cursor();
					} else if (c == 'S') {
						int count = p1 > 0 ? p1 : 1;
						for (int step = 0; step < count; step++) {
							ios_scroll_region_up();
						}
						ios_enforce_row_bounds();
						ios_sync_cursor();
					} else if (c == 'T') {
						int count = p1 > 0 ? p1 : 1;
						for (int step = 0; step < count; step++) {
							ios_scroll_region_down();
						}
						ios_enforce_row_bounds();
						ios_sync_cursor();
					} else if (c == 's') { /* save cursor */
						ios_saved_row = ios_row;
						ios_saved_col = ios_col;
					} else if (c == 'u') { /* restore cursor */
						ios_row = ios_saved_row;
						ios_col = ios_saved_col;
						ios_enforce_row_bounds();
						if (ios_col >= xcols) ios_col = xcols - 1;
						ios_sync_cursor();
                    } else if (c == 'r') {
                        int top = p1 > 0 ? p1 - 1 : 0;
                        int bot = (p2 > 0 ? p2 - 1 : (xrows - 1));
                        if (top < 0) top = 0;
                        if (bot < top) bot = top;
                        if (bot >= xrows) bot = xrows - 1;
                        ios_margin_top = top;
                        ios_margin_bottom = bot;
                        ios_row = ios_margin_top;
                        if (ios_col >= xcols) ios_col = xcols - 1;
                        ios_sync_cursor();
                    } else if (c == 'g') { /* TBC */ 
                        int mode = p1;
                        int limit = xcols < (int)sizeof(ios_tabs) ? xcols : (int)sizeof(ios_tabs);
                        if (mode == 0) {
                            if (ios_col >= 0 && ios_col < limit) ios_tabs[ios_col] = 0;
                        } else if (mode == 3) {
                            for (int t = 0; t < limit; ++t) ios_tabs[t] = 0;
                        }
                    } else if (c == '@') {
                        int count = p1 > 0 ? p1 : 1;
                        pscalTerminalInsertChars(ios_row, ios_col, count);
						ios_sync_cursor();
					} else if (c == 'P') {
						int count = p1 > 0 ? p1 : 1;
						pscalTerminalDeleteChars(ios_row, ios_col, count);
						ios_sync_cursor();
                    } else if (c == 'm') {
                        if (numcnt == 0 && p1 == 0) {
                            ios_fg = -1;
                            ios_bg = -1;
                            ios_attr = 0;
                        } else {
                            for (int idx = 0; idx <= numcnt; idx++) {
                                int code = nums[idx];
                                if (code == 0) {
                                    ios_fg = -1;
                                    ios_bg = -1;
                                    ios_attr = 0;
                                } else if (code == 1) {
                                    ios_attr |= IOS_ATTR_BOLD;
                                } else if (code == 2) {
                                    ios_attr |= (1 << 4); /* faint */ 
                                } else if (code == 3) {
                                    ios_attr |= (1 << 5); /* italic */
                                } else if (code == 4) {
                                    ios_attr |= IOS_ATTR_UNDER;
                                } else if (code == 5) {
                                    ios_attr |= (1 << 3); /* blink */ 
                                } else if (code == 7) {
                                    ios_attr |= IOS_ATTR_INV;
                                } else if (code == 8) {
                                    /* hidden - ignore */ 
                                } else if (code == 9) {
                                    ios_attr |= (1 << 6); /* strike */ 
                                } else if (code == 21 || code == 22) {
                                    ios_attr &= ~IOS_ATTR_BOLD;
                                    ios_attr &= ~(1 << 4);
                                } else if (code == 23) {
                                    ios_attr &= ~(1 << 5);
                                } else if (code == 24) {
                                    ios_attr &= ~IOS_ATTR_UNDER;
                                } else if (code == 25) {
                                    ios_attr &= ~(1 << 3);
                                } else if (code == 27) {
                                    ios_attr &= ~IOS_ATTR_INV;
                                } else if (code == 29) {
                                    ios_attr &= ~(1 << 6);
                                } else if (code == 39) {
                                    ios_fg = -1;
                                } else if (code == 49) {
                                    ios_bg = -1;
                                } else if (code >= 30 && code <= 37) {
                                    ios_fg = code - 30;
                                } else if (code >= 40 && code <= 47) {
                                    ios_bg = code - 40;
                                } else if (code >= 90 && code <= 97) {
                                    ios_fg = (code - 90) + 8;
                                } else if (code >= 100 && code <= 107) {
                                    ios_bg = (code - 100) + 8;
                                } else if (code == 38 || code == 48) {
                                    if (idx + 2 <= numcnt && nums[idx + 1] == 5) {
                                        int val = nums[idx + 2];
                                        if (val >= 0 && val <= 255) {
                                            if (code == 38)
                                                ios_fg = val;
                                            else
                                                ios_bg = val;
                                        }
                                        idx += 2;
                                    } else if (idx + 3 <= numcnt && nums[idx + 1] == 2) {
                                        int r = nums[idx + 2];
                                        int g = nums[idx + 3];
                                        int b = (idx + 4 <= numcnt) ? nums[idx + 4] : 0;
                                        int rr = r < 0 ? 0 : (r > 255 ? 255 : r);
                                        int gg = g < 0 ? 0 : (g > 255 ? 255 : g);
                                        int bb = b < 0 ? 0 : (b > 255 ? 255 : b);
                                        int rc = (rr * 5 + 127) / 255;
                                        int gc = (gg * 5 + 127) / 255;
                                        int bc = (bb * 5 + 127) / 255;
                                        int idx256 = 16 + 36 * rc + 6 * gc + bc;
                                        if (code == 38)
                                            ios_fg = idx256;
                                        else
                                            ios_bg = idx256;
                                        idx += 4;
                                    }
                                }
                            }
                        }
                    } else if (c == 'n') {
                        if (p1 == 6) {
                            char resp[32];
                            int len = snprintf(resp, sizeof(resp), "\x1b[%d;%dR", ios_row + 1, ios_col + 1);
                            if (len > 0)
                                write(1, resp, (size_t)len);
                        } else if (p1 == 5) {
                            const char ok[] = "\x1b[0n";
                            write(1, ok, sizeof(ok) - 1);
                        }
                    }
                } else {
                    if (c == 'h' || c == 'l') {
                        int on = c == 'h';
                        for (int idx = 0; idx <= numcnt; idx++) {
                            int mode = nums[idx];
                            if (mode == 7) {
                                ios_wrap = on;
                                ios_wrap_mode = on;
                            } else if (mode == 6) {
                                ios_origin_mode = on;
                                ios_enforce_row_bounds();
                                pscalTerminalMoveCursor(ios_row, ios_col);
                            } else if (mode == 25) {
                                pscalTerminalSetCursorVisible(on);
                            } else if (mode == 47 || mode == 1049) {
                                if (on)
                                    pscalTerminalEnterAltScreen();
                                else
                                    pscalTerminalExitAltScreen();
                                ios_term_reset();
                                pscalTerminalMoveCursor(ios_row, ios_col);
                            } else if (mode == 2004) {
                                /* bracketed paste toggle: ignore but consume */ 
                            } else if (mode == 1000 || mode == 1002 || mode == 1006) {
                                /* mouse tracking toggles: ignore */ 
                            }
                        }
                    }
                }
				i++;
				break;
			}
			continue;
		}
		int start = i;
		while (i < n && !(s[i] == 0x1B && i + 1 < n && s[i + 1] == '['))
			i++;
		ios_term_render_buf(s + start, i - start);
	}
}
#endif

#if defined(PSCAL_TARGET_IOS)
#undef term_write
#define term_write(s, n) ios_term_write(s, n)
#endif

void term_init(void)
{
	if (xvis & 2)
		return;
	struct termios newtermios;
	sbuf_make(term_sbuf, 2048)
	tcgetattr(0, &termios);
	newtermios = termios;
	newtermios.c_lflag &= ~(ICANON | ISIG | ECHO);
	tcsetattr(0, TCSAFLUSH, &newtermios);
	if (getenv("LINES"))
		xrows = atoi(getenv("LINES"));
	if (getenv("COLUMNS"))
		xcols = atoi(getenv("COLUMNS"));
#if !defined(PSCAL_TARGET_IOS)
	struct winsize win;
	if (!ioctl(0, TIOCGWINSZ, &win)) {
		xcols = win.ws_col;
		xrows = win.ws_row;
	}
#endif
	xcols = xcols ? xcols : 80;
	xrows = xrows ? xrows : 25;
#if defined(PSCAL_TARGET_IOS)
	pscalTerminalBegin(xcols, xrows);
	pscalTerminalClear();
	ios_term_reset();
#endif
}

void term_done(void)
{
	if (xvis & 2)
		return;
	term_commit();
	sbuf_free(term_sbuf);
	term_sbuf = NULL;
	tcsetattr(0, 0, &termios);
#if defined(PSCAL_TARGET_IOS)
	pscalTerminalEnd();
#endif
}

void term_clean(void)
{
	term_write("\x1b[2J", 4);	/* clear screen */
	term_write("\x1b[H", 3);		/* cursor topleft */
#if defined(PSCAL_TARGET_IOS)
	pscalTerminalClear();
	ios_term_reset();
#endif
}

void term_suspend(void)
{
	term_done();
	kill(0, SIGSTOP);
	term_init();
}

void term_commit(void)
{
	if (!term_sbuf) {
		term_record = 0;
		return;
	}
	term_write(term_sbuf->s, term_sbuf->s_n);
	sbuf_cut(term_sbuf, 0);
	term_record = 0;
}

static void term_out(char *s)
{
	if (term_record) {
		if (!term_sbuf) {
			term_record = 0;
			term_write(s, strlen(s));
			return;
		}
		sbufn_str(term_sbuf, s);
	} else {
		term_write(s, strlen(s));
	}
}

void term_chr(int ch)
{
	char s[4] = {ch};
	term_out(s);
}

void term_kill(void)
{
	term_out("\33[K");
}

void term_room(int n)
{
	char cmd[64] = "\33[";
	if (!n)
		return;
	char *s = itoa(abs(n), cmd+2);
	s[0] = n < 0 ? 'M' : 'L';
	s[1] = '\0';
	term_out(cmd);
}

void term_pos(int r, int c)
{
	char buf[64] = "\r\33[", *s;
	if (r < 0) {
		memcpy(itoa(MAX(0, c), buf+3), c > 0 ? "C" : "D", 2);
		term_out(buf);
	} else {
		s = itoa(r + 1, buf+3);
		if (c > 0) {
			*s++ = ';';
			s = itoa(c + 1, s);
		}
		memcpy(s, "H", 2);
		term_out(buf+1);
	}
}

#if defined(PSCAL_TARGET_IOS)
static void ios_term_render_buf(const char *s, int n) {
	if (!s || n <= 0 || xcols <= 0 || xrows <= 0)
		return;
	int i = 0;
	while (i < n) {
		char ch = s[i];
		if (ch == '\r' || ch == '\n' || ch == '\b' || ch == '\t') {
			ios_term_render_char(ch);
			i++;
			continue;
		}
		if (ios_wrap && ios_col >= xcols) {
			ios_col = 0;
			ios_row++;
			if (ios_row >= xrows)
				ios_row = xrows - 1;
			pscalTerminalMoveCursor(ios_row, ios_col);
		} else if (!ios_wrap && ios_col >= xcols) {
			ios_col = xcols - 1;
		}
		int start = i;
		int avail = xcols - ios_col;
		int len = 0;
		while (i < n && len < avail) {
			ch = s[i];
			if (ch == '\r' || ch == '\n' || ch == '\b' || ch == '\t')
				break;
			len++;
			i++;
		}
		if (len > 0) {
			pscalTerminalRender(s + start, len, ios_row, ios_col, ios_fg, ios_bg, ios_attr);
			ios_col += len;
			if (!ios_wrap && ios_col >= xcols)
				ios_col = xcols - 1;
			pscalTerminalMoveCursor(ios_row, ios_col);
		} else {
			i++;
		}
	}
}
#endif

#if defined(PSCAL_TARGET_IOS)
/* term_write is left intact; we mirror only explicit character writes. */
#endif

/* read s before reading from the terminal */
void term_push(char *s, unsigned int n)
{
	static unsigned int tibuf_pos, tibuf_cnt;
	if (texec == '@' && xquit > 0) {
		xquit = 0;
		tn = 0;
		ibuf_cnt = tibuf_cnt;
		ibuf_pos = tibuf_cnt;
	}
	if (ibuf_cnt + n >= ibuf_sz || ibuf_sz - ibuf_cnt + n > 128) {
		ibuf_sz = ibuf_cnt + n + 128;
		ibuf = erealloc(ibuf, ibuf_sz);
	}
	if (texec) {
		if (tibuf_pos != ibuf_pos)
			tn = 0;
		memmove(ibuf + ibuf_pos + n + tn,
			ibuf + ibuf_pos + tn, ibuf_cnt - ibuf_pos - tn);
		memcpy(ibuf + ibuf_pos + tn, s, n);
		tn += n;
		tibuf_pos = ibuf_pos;
	} else
		memcpy(ibuf + ibuf_cnt, s, n);
	tibuf_cnt = ibuf_cnt;
	ibuf_cnt += n;
}

void term_back(int c)
{
	char s[1] = {c};
	term_push(s, 1);
}

int term_read(void)
{
#if defined(PSCAL_TARGET_IOS)
	if (ibuf_pos >= ibuf_cnt) {
		if (texec) {
			xquit = !xquit ? 1 : xquit;
			if (texec == '&')
				return 0;
		}
		/* In iOS bridge mode, timeout-driven zero reads are interpreted by
		 * nextvi as TK_INT and can corrupt command-repeat flow (e.g. '.' after
		 * 'dd'). Block until input or shutdown to match desktop behavior. */
		while (ibuf_pos >= ibuf_cnt) {
			int n = pscalTerminalRead(ibuf, 1, 0);
			if (n > 0) {
				ibuf_cnt = (unsigned int)n;
				ibuf_pos = 0;
				break;
			}
			/* n == 0 means timeout/no data: retry instead of emitting TK_INT. */
			if (n == 0)
				continue;
			/* n < 0 means terminal/editor shutdown. */
			if (texec)
				xquit = texec == '&' ? -1 : 1;
			return 0;
		}
	}
	if (icmd_pos < sizeof(icmd))
		icmd[icmd_pos++] = ibuf[ibuf_pos];
	return ibuf[ibuf_pos++];
#else
	if (ibuf_pos >= ibuf_cnt) {
		if (texec) {
			xquit = !xquit ? 1 : xquit;
			if (texec == '&')
				goto err;
		}
		ufds[0].fd = STDIN_FILENO;
		ufds[0].events = POLLIN;
		/* read a single input character */
		if (xquit < 0 || poll(ufds, 1, -1) <= 0 ||
				read(STDIN_FILENO, ibuf, 1) <= 0) {
			xquit = !isatty(STDIN_FILENO) ? -1 : xquit;
			err:
			*ibuf = 0;
		}
		ibuf_cnt = 1;
		ibuf_pos = 0;
	}
	if (icmd_pos < sizeof(icmd))
		icmd[icmd_pos++] = ibuf[ibuf_pos];
	return ibuf[ibuf_pos++];
#endif
}

/* return a static string that changes text attributes to att */
char *term_att(int att)
{
	static char buf[128];
	char *s = buf;
	int fg = SYN_FG(att);
	int bg = SYN_BG(att);
	*s++ = '\x1b';
	*s++ = '[';
	if (att & SYN_BD)
		{*s++ = ';'; *s++ = '1';}
	if (att & SYN_IT)
		{*s++ = ';'; *s++ = '3';}
	else if (att & SYN_RV)
		{*s++ = ';'; *s++ = '7';}
	if (SYN_FGSET(att)) {
		*s++ = ';';
		if (fg < 8)
			s = itoa(30 + fg, s);
		else
			s = itoa(fg, (char*)memcpy(s, "38;5;", 5)+5);
	}
	if (SYN_BGSET(att)) {
		*s++ = ';';
		if (bg < 8)
			s = itoa(40 + bg, s);
		else
			s = itoa(bg, (char*)memcpy(s, "48;5;", 5)+5);
	}
	s[0] = 'm';
	s[1] = '\0';
	return buf;
}

static int cmd_make(char **argv, int *ifd, int *ofd)
{
	int pid;
	int pipefds0[2] = {-1, -1};
	int pipefds1[2] = {-1, -1};
	if (ifd)
		pipe(pipefds0);
	if (ofd)
		pipe(pipefds1);
	if (!(pid = fork())) {
		if (ifd) {		/* setting up stdin */
			close(0);
			dup(pipefds0[0]);
			close(pipefds0[1]);
			close(pipefds0[0]);
		}
		if (ofd) {		/* setting up stdout and stderr */
			close(1);
			dup(pipefds1[1]);
			close(2);
			dup(pipefds1[1]);
			close(pipefds1[0]);
			close(pipefds1[1]);
		}
		execvp(argv[0], argv);
		exit(1);
	}
	if (ifd)
		close(pipefds0[0]);
	if (ofd)
		close(pipefds1[1]);
	if (pid < 0) {
		if (ifd)
			close(pipefds0[1]);
		if (ofd)
			close(pipefds1[0]);
		return -1;
	}
	if (ifd)
		*ifd = pipefds0[1];
	if (ofd)
		*ofd = pipefds1[0];
	return pid;
}

char *xgetenv(char **q)
{
	char *r = NULL;
	while (*q && !r) {
		if (**q == '$')
			r = getenv(*q+1);
		else
			return *q;
		q++;
	}
	return r;
}

/* execute a command; pass in input if ibuf and process output if oproc */
sbuf *cmd_pipe(char *cmd, sbuf *ibuf, int oproc, int *status)
{
	static char *sh[] = {"$SHELL", "sh", NULL};
	struct pollfd fds[3];
	char buf[512];
	int ifd = -1, ofd = -1;
	int nw = 0;
	char *argv[5];
	argv[0] = xgetenv(sh);
	argv[1] = xish ? "-i" : argv[0];
	argv[2] = "-c";
	argv[3] = cmd;
	argv[4] = NULL;
	int pid = cmd_make(argv+!xish, ibuf ? &ifd : NULL, oproc ? &ofd : NULL);
	if (pid <= 0) {
		if (status)
			*status = 127;
		return NULL;
	}
	sbuf *sb;
	sbuf_make(sb, sizeof(buf)+1)
	if (!ibuf) {
		signal(SIGINT, SIG_IGN);
		term_done();
	} else if (ifd >= 0)
		fcntl(ifd, F_SETFL, fcntl(ifd, F_GETFL, 0) | O_NONBLOCK);
	fds[0].fd = ofd;
	fds[0].events = POLLIN;
	fds[1].fd = ifd;
	fds[1].events = POLLOUT;
	fds[2].fd = ibuf ? 0 : -1;
	fds[2].events = POLLIN;
	while ((fds[0].fd >= 0 || fds[1].fd >= 0) && poll(fds, 3, 200) >= 0) {
		if (fds[0].revents & POLLIN) {
			int ret = read(fds[0].fd, buf, sizeof(buf));
			if (ret > 0 && oproc == 2) {
				term_write(buf, ret);
			}
			if (ret > 0) {
				sbuf_mem(sb, buf, ret);
			} else {
				close(fds[0].fd);
				fds[0].fd = -1;
			}
		} else if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			close(fds[0].fd);
			fds[0].fd = -1;
		}
		if (fds[1].revents & POLLOUT && ibuf) {
			int ret = write(fds[1].fd, ibuf->s + nw, ibuf->s_n - nw);
			if (ret > 0)
				nw += ret;
			if (ret <= 0 || nw == ibuf->s_n) {
				close(fds[1].fd);
				fds[1].fd = -1;
			}
		} else if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			close(fds[1].fd);
			fds[1].fd = -1;
		}
		if (fds[2].revents & POLLIN) {
			int ret = read(fds[2].fd, buf, sizeof(buf));
			for (int i = 0; i < ret; i++)
				if ((unsigned char) buf[i] == TK_CTL('c'))
					kill(pid, SIGINT);
		} else if (fds[2].revents & (POLLERR | POLLHUP | POLLNVAL))
			fds[2].fd = -1;
	}
	if (fds[0].fd >= 0)
		close(ofd);
	if (fds[1].fd >= 0)
		close(ifd);
	waitpid(pid, status, 0);
	signal(SIGTTOU, SIG_IGN);
	tcsetpgrp(STDIN_FILENO, getpgrp());
	signal(SIGTTOU, SIG_DFL);
	if (!ibuf) {
		term_init();
		signal(SIGINT, SIG_DFL);
	}
if (oproc)
    sbufn_ret(sb, sb)
sbuf_free(sb)
return NULL;
}
