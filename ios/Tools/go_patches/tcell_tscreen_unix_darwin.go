//go:build darwin

package tcell

import (
	"io"
	"os"

	"github.com/zyedidia/poller"
)

func pscalUseEmbeddedStdio() bool {
	return os.Getenv("PSCAL_MICRO_EMBEDDED") == "1"
}

func openTty() (io.Reader, io.Writer, error) {
	if pscalUseEmbeddedStdio() {
		return os.Stdin, os.Stdout, nil
	}
	inFD, err := poller.Open("/dev/tty", poller.O_RO)
	if err != nil {
		// iOS containers frequently block /dev/tty; stdin/stdout are already
		// bound to the PSCAL PTY in that case.
		return os.Stdin, os.Stdout, nil
	}
	outFD, err := poller.Open("/dev/tty", poller.O_WO)
	if err != nil {
		_ = inFD.Close()
		return os.Stdin, os.Stdout, nil
	}

	return inFD, outFD, nil
}

func closeTty(f interface{}) {
	if f == nil {
		return
	}
	switch fd := f.(type) {
	case *poller.FD:
		if fd != nil {
			_ = fd.Close()
		}
	case *os.File:
		if fd != nil && fd != os.Stdin && fd != os.Stdout && fd != os.Stderr {
			_ = fd.Close()
		}
	}
}

func (t *tScreen) fd() int {
	switch fd := t.out.(type) {
	case *poller.FD:
		if fd != nil {
			return fd.Sysfd()
		}
	case *os.File:
		if fd != nil {
			return int(fd.Fd())
		}
	}
	return int(os.Stdout.Fd())
}
