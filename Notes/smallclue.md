# smallclue Notes

- `cp`, `mv`, and `rm` are implemented as smallclue applets and
  intentionally **not** exposed as exsh builtins.  Only add them to
  exsh (or introduce any new shell builtin) when explicitly requested
  by the project owner.  They are still invoked from exsh through the
  smallclue integration wrappers (see `src/smallclue/integration.c`) so
  that pipelines and scripts can call them just like the other applets.
- On iOS/iPadOS, expose new smallclue applets to exsh by (1) adding them to
  `kSmallclueApplets` (core), (2) defining wrappers + registering them in
  `src/smallclue/integration.c`, and (3) listing their names in
  `src/shell/builtins.c` (iOS section) so `shellIsBuiltinName()` recognises
  them.  Skipping any step leaves exsh reporting "command unavailable in
  pipeline" even though `bin/smallclue` works.
- `stty` applet emits `ESC[8;rows;cols]t` so the iOS terminal (via
  TerminalBuffer) can request a resize.  This is how manual row/column
  tweaks (e.g. `stty rows 40 cols 120`) communicate with the host UI and
  `PSCALRuntimeUpdateWindowSize` path.
- Recently added text-processing applets (`sort`, `uniq`, `sed`, `cut`,
  `tr`) and utility helpers (`id`) all follow the same pattern as the
  other commands: smallclue implementation + integration wrapper +
  iOS-only shell builtin entry so exsh pipelines work.
- `micro` on iOS currently requires a host PTY (`/dev/ptmx` or
  `/private/dev/ptmx`) for reliable in-tab rendering/input. Forcing the
  internal shim PTY path can make `micro` render into the Xcode console
  because Go/tcell writes bypass vproc shim on some paths.
- Shim PTY fallback for `micro` is therefore opt-in only via
  `PSCALI_MICRO_ALLOW_SHIM_PTY=1`; default behavior is to fail fast if a
  host PTY is unavailable instead of hijacking host console output.
