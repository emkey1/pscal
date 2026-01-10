/* config_pscal_ios.h
 *
 * This configuration is adapted from the Elvis 2.2.1 "configure unix"
 * output but trimmed down for the PSCAL iOS runtime.  Only the termcap/open
 * text user interfaces are enabled to keep dependencies small and to avoid
 * linking against X11/curses.  Networking GUIs/protocols which rely on
 * platform libraries that aren't available on iOS are disabled as well.
 */

#pragma once

/* User interfaces */
#undef GUI_X11
#undef GUI_CURSES
#define GUI_TERMCAP
#define GUI_OPEN
#define GUI_PSCAL

/* Display modes & optional features */
#define DISPLAY_HEX
#define DISPLAY_HTML
#define DISPLAY_MAN
#define DISPLAY_TEX
#define DISPLAY_SYNTAX
#undef PROTOCOL_HTTP
#undef PROTOCOL_FTP
#define FEATURE_ALIAS
#define FEATURE_ARRAY
#define FEATURE_AUTOCMD
#define FEATURE_BACKTICK
#define FEATURE_BROWSE
#define FEATURE_CACHEDESC
#define FEATURE_CALC
#define FEATURE_COMPLETE
#define FEATURE_EQUALTILDE
#define FEATURE_FOLD
#define FEATURE_G
#define FEATURE_HLOBJECT
#define FEATURE_HLSEARCH
#undef FEATURE_IMAGE
#define FEATURE_INCSEARCH
#define FEATURE_LISTCHARS
#define FEATURE_LITRE
#define FEATURE_LPR
#define FEATURE_MAKE
#define FEATURE_MAPDB
#define FEATURE_MISC
#define FEATURE_MKEXRC
#define FEATURE_NORMAL
#define FEATURE_PROTO
#define FEATURE_RCSID
#define FEATURE_REGION
#define FEATURE_SHOWTAG
#define FEATURE_SMARTARGS
#define FEATURE_SPELL
#define FEATURE_SPLIT
#define FEATURE_STDIN
#define FEATURE_TAGS
#define FEATURE_TEXTOBJ
#define FEATURE_V
#define FEATURE_RAM
#undef FEATURE_XFT
#define FEATURE_PERSIST

/* Replacement/compat helpers */
#undef NEED_ABORT
#undef NEED_ASSERT
#define NEED_TGETENT
#undef NEED_WINSIZE
#undef NEED_SPEED_T
#undef NEED_STRDUP
#undef NEED_MEMMOVE
#undef NEED_OSPEED
#define NEED_BC
#define NEED_SETPGID
#define NEED_FREOPEN
#define NEED_CTYPE
#undef NEED_WAIT_H
#undef NEED_SELECT_H
#define NEED_IOCTL_H
#undef NEED_XOS_H
#undef NEED_IN_H
#undef NEED_SOCKET_H
#undef NEED_XRMCOMBINEFILEDATABASE
#undef NEED_INET_ATON

/* Debugging */
#define NDEBUG
#undef DEBUG_ALLOC
#undef DEBUG_SCAN
#undef DEBUG_SESSION
#undef DEBUG_EVENT
#undef DEBUG_MARKUP
#undef DEBUG_REGEXP
#undef DEBUG_MARK
#undef DEBUG_REGION

/* Default option values */
#define OSLPOUT "!lp -s"
#define OSLIBPATH "~/.elvis:/etc/elvis:/usr/share/elvis/:/usr/share/elvis/doc/"

/* PSCAL-specific helpers */
#define PSCALI_IGNORE_SESSION_LOCKS 1
