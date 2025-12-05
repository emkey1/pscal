# PSCAL PTY Emulation Notes

This app runs the shell in-process on iOS, falling back to a virtual TTY built on pipes when `openpty` is unavailable. The virtual TTY emulates a small subset of PTY behavior and still has limitations.

## Current behavior
- Default termios state (ICANON, ECHO, ISIG, CR->NL, ONLCR) applied to the virtual TTY.
- Minimal line discipline: canonical buffering, erase handling, echo/echonl, and generation of SIGINT/SIGQUIT/SIGTSTP for VINTR/VQUIT/VSUSP.
- Window size updates propagate to the runtime thread and deliver SIGWINCH.

## Known limitations
- No true controlling TTY: job control (tcsetpgrp/tcgetpgrp), TIOCGPGRP/TIOCSPGRP, and ctty semantics are not implemented.
- Termios changes via `tcsetattr` on STDIN will not affect the virtual TTY unless explicitly wired through the bridge.
- Many ioctls are unimplemented (TIOCSTI, tcflush, etc.); behavior may differ from a real PTY.
- Backpressure is basic; heavy output can still starve the UI without further batching.

## Future improvements
- Stub pgrp/ctty ioctls and propagate foreground process group to improve job control.
- Bridge tcgetattr/tcsetattr for virtual TTY so tools can adjust echo/raw modes dynamically.
- Expand ioctl coverage and add better flow control/buffering for large output bursts.
