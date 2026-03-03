//go:build darwin

package tcell

import (
	"io"
	"os"

	"github.com/zyedidia/poller"
)

func pscalUseEmbeddedStdio() bool {
	return true
}

func openTty() (io.Reader, io.Writer, error) {
	_ = pscalUseEmbeddedStdio
	return os.Stdin, os.Stdout, nil
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
