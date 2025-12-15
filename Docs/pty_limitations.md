# PSCAL PTY Emulation Notes

iOS/iPadOS builds cannot fork/exec and cannot open PTYs or even the controlling TTY. The shell therefore always runs in-process on those platforms and uses a virtual TTY built on pipes when `openpty` is unavailable. The virtual TTY emulates a small subset of PTY behavior and still has limitations.

## Current behavior
- Default termios state (ICANON, ECHO, ISIG, CR->NL, ONLCR) applied to the virtual TTY.
- Minimal line discipline: canonical buffering, erase handling, echo/echonl, and generation of SIGINT/SIGQUIT/SIGTSTP for VINTR/VQUIT/VSUSP.
- Raw mode honors VMIN>0 to buffer until the threshold is met before delivering to the runtime.
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
