//go:build darwin

package tcell

import (
	"io"
	"os"
	"strconv"
	"sync"
	"syscall"

	"github.com/zyedidia/poller"
)

func pscalUseEmbeddedStdio() bool {
	return true
}

func pscalParseStdioFD(name string, fallback int) int {
	value := os.Getenv(name)
	if value == "" {
		return fallback
	}
	fd, err := strconv.Atoi(value)
	if err != nil || fd < 0 {
		return fallback
	}
	return fd
}

func pscalDupFD(fd int) (int, error) {
	for {
		dupFD, err := syscall.Dup(fd)
		if err == nil {
			return dupFD, nil
		}
		if err != syscall.EINTR {
			return -1, err
		}
	}
}

var pscalStdioProviderMu sync.RWMutex
var pscalStdioProvider func() (int, int, bool)

func SetPSCALStdioProvider(provider func() (int, int, bool)) {
	pscalStdioProviderMu.Lock()
	pscalStdioProvider = provider
	pscalStdioProviderMu.Unlock()
}

func pscalResolveStdioFDs() (int, int) {
	pscalStdioProviderMu.RLock()
	provider := pscalStdioProvider
	pscalStdioProviderMu.RUnlock()
	if provider != nil {
		if inFD, outFD, ok := provider(); ok && inFD >= 0 && outFD >= 0 {
			return inFD, outFD
		}
	}
	inFD := pscalParseStdioFD("PSCALI_TCELL_STDIN_FD", int(os.Stdin.Fd()))
	outFD := pscalParseStdioFD("PSCALI_TCELL_STDOUT_FD", int(os.Stdout.Fd()))
	return inFD, outFD
}

func openTty() (io.Reader, io.Writer, error) {
	_ = pscalUseEmbeddedStdio
	inFD, outFD := pscalResolveStdioFDs()

	dupIn, err := pscalDupFD(inFD)
	if err != nil {
		return nil, nil, err
	}
	dupOut, err := pscalDupFD(outFD)
	if err != nil {
		_ = syscall.Close(dupIn)
		return nil, nil, err
	}

	in := os.NewFile(uintptr(dupIn), "/dev/tty")
	out := os.NewFile(uintptr(dupOut), "/dev/tty")
	if in == nil || out == nil {
		if in != nil {
			_ = in.Close()
		} else {
			_ = syscall.Close(dupIn)
		}
		if out != nil {
			_ = out.Close()
		} else {
			_ = syscall.Close(dupOut)
		}
		return nil, nil, syscall.EBADF
	}
	return in, out, nil
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
