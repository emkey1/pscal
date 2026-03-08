//go:build aix || darwin || dragonfly || freebsd || linux || netbsd || openbsd || solaris || zos
// +build aix darwin dragonfly freebsd linux netbsd openbsd solaris zos

package tcell

import (
	"os"
	"os/signal"
	"strconv"
	"syscall"
	"time"

	"golang.org/x/sys/unix"
	"golang.org/x/term"
)

func pscalAllowNonRawMode() bool {
	return os.Getenv("PSCALI_TCELL_ALLOW_NONRAW") == "1"
}

func pscalUseEmbeddedStdioMode() bool {
	return os.Getenv("PSCALI_TCELL_USE_STDIO") == "1" ||
		os.Getenv("PSCAL_MICRO_EMBEDDED") == "1"
}

func pscalUseResizePoller() bool {
	return pscalAllowNonRawMode() || pscalUseEmbeddedStdioMode()
}

func (t *tScreen) pscalSafePostResizeEvent(cols int, rows int) {
	if t.evch == nil {
		return
	}
	defer func() {
		_ = recover()
	}()
	_ = t.PostEvent(NewEventResize(cols, rows))
}

func (t *tScreen) pscalApplyResize(cols int, rows int) {
	defer func() {
		_ = recover()
	}()
	if cols <= 0 || rows <= 0 {
		return
	}
	t.Lock()
	if t.fini {
		t.Unlock()
		return
	}
	if cols != t.w || rows != t.h {
		t.cx = -1
		t.cy = -1
		t.cells.Resize(cols, rows)
		t.cells.Invalidate()
		t.h = rows
		t.w = cols
		t.pscalSafePostResizeEvent(cols, rows)
		t.draw()
	}
	t.Unlock()
}

// PSCALApplyResize is an embedded-host hook used by micro's iOS bridge to
// apply a runtime-reported geometry update and enqueue a resize event in one
// step. This keeps tcell's internal size synchronized for handlers that read
// screen.Size() during EventResize processing.
func (t *tScreen) PSCALApplyResize(cols int, rows int) {
	t.pscalApplyResize(cols, rows)
}

func (t *tScreen) pscalStartResizePoller() {
	if !pscalUseResizePoller() {
		return
	}
	go func() {
		defer func() {
			_ = recover()
		}()

		// Init() creates t.quit after termioInit(). Wait briefly so we can
		// exit cleanly when the screen is torn down.
		for i := 0; i < 100; i++ {
			if t.quit != nil {
				break
			}
			time.Sleep(10 * time.Millisecond)
		}

		lastW, lastH := -1, -1
		if w, h, err := t.getWinSize(); err == nil && w > 0 && h > 0 {
			lastW, lastH = w, h
		}

		ticker := time.NewTicker(150 * time.Millisecond)
		defer ticker.Stop()

		for {
			if t.quit != nil {
				select {
				case <-t.quit:
					return
				default:
				}
			}

			<-ticker.C
			w, h, err := t.getWinSize()
			if err != nil || w <= 0 || h <= 0 {
				continue
			}
			if w == lastW && h == lastH {
				continue
			}
			lastW, lastH = w, h
			t.pscalApplyResize(w, h)
		}
	}()
}

func (t *tScreen) termioInit() error {
	var e error
	var state *term.State

	if t.in, t.out, e = openTty(); e != nil {
		goto failed
	}

	state, e = term.MakeRaw(t.fd())
	if e != nil {
		// In embedded stdio mode, raw can fail with EPERM on iOS even when
		// stdio is correctly bridged to a PTY. Fall back to non-raw instead
		// of aborting screen init.
		if !pscalAllowNonRawMode() && !pscalUseEmbeddedStdioMode() {
			goto failed
		}
		t.saved = nil
	} else {
		t.saved = state
	}

	// Embedded iOS mode drives resize via the poller path only; disabling
	// signal.Notify avoids late shutdown SIGWINCH delivery races.
	if t.sigwinch != nil && !pscalUseResizePoller() {
		signal.Notify(t.sigwinch, syscall.SIGWINCH)
	}
	t.pscalStartResizePoller()

	if w, h, e := t.getWinSize(); e == nil && w != 0 && h != 0 {
		t.cells.Resize(w, h)
	}

	return nil

failed:
	if t.in != nil {
		closeTty(t.in)
	}
	if t.out != nil {
		closeTty(t.out)
	}
	return e
}

func (t *tScreen) termioFini() {
	if t.sigwinch != nil && !pscalUseResizePoller() {
		signal.Stop(t.sigwinch)
	}

	if t.indoneq != nil {
		<-t.indoneq
	}

	if t.out != nil && t.saved != nil {
		term.Restore(t.fd(), t.saved)
	}
	if t.out != nil {
		closeTty(t.out)
	}

	if t.in != nil {
		closeTty(t.in)
	}
}

func (t *tScreen) getWinSize() (int, int, error) {
	wsz, err := unix.IoctlGetWinsize(t.fd(), unix.TIOCGWINSZ)
	cols := 0
	rows := 0
	if err == nil {
		cols = int(wsz.Col)
		rows = int(wsz.Row)
	} else if !pscalAllowNonRawMode() && !pscalUseEmbeddedStdioMode() {
		return -1, -1, err
	}
	if cols == 0 {
		if pscalUseEmbeddedStdioMode() && t.w > 0 {
			// In embedded multi-runtime mode COLUMNS/LINES are process-global and can
			// cross-contaminate instances. Keep current screen width unless ioctl
			// reports a concrete value.
			cols = t.w
		} else {
			colsEnv := os.Getenv("COLUMNS")
			if colsEnv != "" {
				if parsed, parseErr := strconv.Atoi(colsEnv); parseErr == nil && parsed > 0 {
					cols = parsed
				}
			}
		}
	}
	if rows == 0 {
		if pscalUseEmbeddedStdioMode() && t.h > 0 {
			rows = t.h
		} else {
			rowsEnv := os.Getenv("LINES")
			if rowsEnv != "" {
				if parsed, parseErr := strconv.Atoi(rowsEnv); parseErr == nil && parsed > 0 {
					rows = parsed
				}
			}
		}
	}
	if cols <= 0 {
		cols = t.ti.Columns
	}
	if rows <= 0 {
		rows = t.ti.Lines
	}
	if cols <= 0 {
		cols = 80
	}
	if rows <= 0 {
		rows = 24
	}
	return cols, rows, nil
}

func (t *tScreen) Beep() error {
	t.writeString(string(byte(7)))
	return nil
}
