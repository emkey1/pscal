# iOS/iPadOS Editor Integration Contract (Nextvi and Micro)

This document is the source of truth for editor integration on iOS/iPadOS in PSCAL.

## Non-Negotiable Rules

1. iOS/iPadOS must run editors in-process (single executable model).
2. `micro` must never fallback to `vi` or `nextvi`.
3. `micro` integration must not depend on runtime payload execution (`PSCALI_MICRO_PATH`, `.deflate`, or sidecar binary execution).
4. Build must fail if `micro` integration is missing or incomplete.
5. Build must fail if any required editor component is missing.

## Reference: How Nextvi Is Integrated

`nextvi` is integrated directly into `pscal_core_static`:

- Source inclusion:
  - `CMakeLists.txt` adds `third-party/nextvi/vi.c` to `PSCAL_CORE_SOURCES`.
- Entrypoint remap:
  - `main` is remapped to `nextvi_main_entry` via compile definitions.
- Dispatch path:
  - `smallclue` command `nextvi` calls `smallclueRunEditor`.
  - `smallclueRunEditor` calls `nextvi_main_entry` in-process.

This is the required model for `micro` as well: link-time integrated and in-process.

## Required Micro Integration Pattern

1. Produce/link a real in-process symbol:
   - `int pscal_micro_main_entry(int argc, char **argv);`
2. Ensure `pscal_micro_main_entry` is linked into `libpscal_core_static.a` for iOS/iPadOS builds.
3. Keep `smallclueRunMicro()` as a direct call to `pscal_micro_main_entry`.
4. Do not reference `nextvi` from `src/smallclue/src/micro_app.c`.
5. Do not add runtime fallbacks for missing `micro` binaries/payloads.
6. Treat missing Go toolchain/module resolution as a hard build failure.
7. If a Go TUI dependency assumes `/dev/tty` on Darwin, patch it for iOS embedding so it can use inherited stdio (PSCAL PTY) and never crash on typed-nil cleanup paths.
8. Embedded entrypoints must not re-panic across the C boundary; return a non-zero status on internal panic instead.
9. Interactive editors must set up and restore standard tty file descriptors around entrypoint invocation (same pattern used by `nextvi`) so UI init does not immediately return.
10. Embedded integrations that have config/home discovery logic must inject explicit writable config paths (for micro: `-config-dir`) to avoid iOS container root/path-truncation resolution issues.
11. iOS runtime bootstrap must seed writable editor config env (`HOME`, `XDG_CONFIG_HOME`, `MICRO_CONFIG_HOME`) under `Documents/home` before shell/app launch.
12. Embedded cleanup paths must be panic-safe for partially initialized terminal/screen state; cleanup panics must never mask the original startup error.
13. Embedded panic reporting must include a stack trace in stderr to keep iOS runtime failures diagnosable without LLDB.

## Integration Recipe (Any New Editor)

Use this exact sequence for future editors (`foo`, etc.):

1. Add a stable in-process C entrypoint symbol:
   - `int pscal_foo_main_entry(int argc, char **argv);`
2. Wire smallclue command dispatch directly to that symbol.
3. Link implementation artifacts into iOS static link graph at build time (no runtime payload execution path).
4. Add a hard validation gate in `ios/Tools/ensure_static_libs.sh`:
   - required archive exists
   - required symbol exists
   - forbidden fallback references are absent
5. Keep Xcode script phases fail-fast (`set -euo pipefail` and no `|| true` around required steps).
6. Verify on all supported Apple SDK variants used by PSCAL:
   - `iphoneos`
   - `iphonesimulator`
   - `macosx` (Mac Catalyst path)

## Build-Time Enforcement (Must Fail)

Current enforced checks in `ios/Tools/ensure_static_libs.sh`:

1. Required static archives must exist.
2. `libpscal_core_static.a` must export `nextvi_main_entry`.
3. `libpscal_core_static.a` must export `pscal_micro_main_entry`.
4. `libpscal_core_static.a` must export `pscal_micro_go_main_entry`.
5. `src/smallclue/src/micro_app.c` must not reference `nextvi`.
6. iOS static-lib ensure phase must run every build (not only when outputs are missing) so source edits cannot be masked by stale archives.

Any failure above is a hard build error.

## Build Prerequisites

1. A working Go toolchain must be available (`go`).
2. The micro module dependencies must be resolvable for the embed archive build.
3. If prerequisites are missing, configuration/build must stop with an explicit error.
4. Go dependency patching required for iOS embed must be deterministic and applied during build (no manual local edits in module cache).

## Explicitly Forbidden Behavior

1. Running `nextvi` when `micro` is requested.
2. Silent or implicit fallback from `micro` to any other editor.
3. Shipping a build where `micro` command exists but `pscal_micro_main_entry` is absent.

## Status

- `smallclue` micro dispatch is wired to `pscal_micro_main_entry`.
- `pscal_micro_main_entry` is now implemented in-process and forwards into embedded Go micro (`pscal_micro_go_main_entry`).
- Build gate fails if `pscal_micro_main_entry` is missing.
- Build gate fails if `src/smallclue/src/micro_app.c` references `nextvi`.
- iOS/iPadOS/Catalyst static builds now include an embedded `libpscal_micro_embed.a` archive built from `third-party/micro`.

## Progress Log

- 2026-02-27: Added hard validation in `ios/Tools/ensure_static_libs.sh` for:
  - required archive presence
  - `nextvi_main_entry` symbol in `libpscal_core_static.a`
  - `pscal_micro_main_entry` symbol in `libpscal_core_static.a`
  - forbidden `nextvi` reference in `src/smallclue/src/micro_app.c`
- 2026-02-27: Validation run confirmed hard failure when `pscal_micro_main_entry` is missing.
- 2026-02-27: Added embedded micro Go export in `third-party/micro/cmd/micro/pscal_embed.go` (`pscal_micro_go_main_entry`).
- 2026-02-27: Updated `third-party/micro/cmd/micro/micro.go` for embedded mode:
  - per-run flagset reset
  - non-process-exiting embedded return path (panic/recover status handoff)
- 2026-02-27: Added build tool `ios/Tools/build_micro_embed_archive.sh` to produce `c-archive` for iOS targets.
- 2026-02-27: Integrated micro embed archive into CMake iOS path (`pscal_micro_embed` target and transitive link).
- 2026-02-27: Added required Apple frameworks (`CoreFoundation`, `Security`) to iOS static transitive link set for embedded micro archive symbols.
- 2026-02-27: Verified:
  - `cmake --build --preset ios-simulator --target pscal_core_static pscal_tool_runner`
  - `SDK_NAME=iphonesimulator bash ios/Tools/ensure_static_libs.sh`
  - `cmake --build --preset ios-device --target pscal_core_static pscal_tool_runner`
  - `SDK_NAME=iphoneos bash ios/Tools/ensure_static_libs.sh`
  - `cmake -S . -B build/ios-maccatalyst-arm64 ... -DPSCAL_FORCE_IOS=ON -DSDL=OFF`
  - `cmake --build build/ios-maccatalyst-arm64 --target pscal_tool_runner`
  - `SDK_NAME=macosx NATIVE_ARCH_ACTUAL=arm64 bash ios/Tools/ensure_static_libs.sh`
- 2026-02-28: Fixed undefined-symbol root cause for `_pscal_micro_go_main_entry` by embedding `libpscal_micro_embed.a` into `libpscal_core_static.a` during the iOS archive merge step.
- 2026-02-28: Hardened symbol validation logic to require **defined** symbols (not undefined `U` references) in `ensure_static_libs.sh`.
- 2026-02-28: Re-verified device/simulator/Catalyst archives all export:
  - `nextvi_main_entry`
  - `pscal_micro_main_entry`
  - `pscal_micro_go_main_entry`
- 2026-02-28: Updated micro entrypoint flow so embedded execution calls `pscalMicroMain()` (named internal entry), with standalone `main()` kept as a wrapper.
- 2026-02-28: Fixed iOS micro crash path from Darwin tcell/poller typed-nil tty cleanup by adding deterministic build-time patching of `tcell` (`tscreen_unix_darwin.go`) for stdio fallback + nil-safe close.
- 2026-02-28: Hardened micro runtime environment setup in `micro_app.c` to force writable iOS config paths (`HOME`/`XDG_CONFIG_HOME` under `PSCALI_WORKDIR`) and set `TERM=xterm-256color`.
- 2026-02-28: Hardened embedded Go panic handling so internal micro panics return status `1` instead of crashing the host app process.
- 2026-02-28: Aligned `micro` launch with `nextvi` tty fd handling by saving/restoring stdin/stdout/stderr and binding editor execution to `/dev/tty` (or PTY fallback) before calling `pscal_micro_main_entry`.
- 2026-02-28: Added embedded micro argv injection for writable config path (`-config-dir $PSCALI_WORKDIR/.config/micro`) with pre-create logic to bypass iOS home/path-truncation fallback to container root.
- 2026-02-28: Hardened workdir resolution for embedded micro config paths to prefer `PSCALI_WORKDIR`, then fallback to `PSCALI_CONTAINER_ROOT/Documents/home`, and force `HOME`/`XDG_CONFIG_HOME`/`MICRO_CONFIG_HOME` accordingly before launch.
- 2026-02-28: Re-verified after tty/config/panic hardening:
  - `cmake --build --preset ios-device --target pscal_core_static pscal_tool_runner`
  - `cmake --build --preset ios-simulator --target pscal_core_static pscal_tool_runner`
  - `cmake --build build/ios-maccatalyst-arm64 --target pscal_core_static pscal_tool_runner`
  - `SDK_NAME=iphoneos bash ios/Tools/ensure_static_libs.sh`
  - `SDK_NAME=iphonesimulator bash ios/Tools/ensure_static_libs.sh`
  - `SDK_NAME=macosx NATIVE_ARCH_ACTUAL=arm64 bash ios/Tools/ensure_static_libs.sh`
  - `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -destination 'generic/platform=iOS' CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY='' build`
- 2026-02-28: Fixed stale-archive risk in iOS Xcode integration:
  - `Ensure PSCAL Static Libs` phase is now forced out-of-date each build (`alwaysOutOfDate = 1`)
  - `ios/Tools/ensure_static_libs.sh` now always runs incremental CMake build for the active SDK instead of only building when archives are missing.
  - `ios/Tools/ensure_static_libs.sh` now invokes CMake with explicit source/build paths so preset resolution works when run from Xcode's `ios/` working directory.
- 2026-02-28: Seeded runtime editor config environment in iOS bootstrap (`PscalRuntimeBootstrap.swift`) so shell/editor startup has writable defaults:
  - `HOME=$PSCALI_WORKDIR`
  - `XDG_CONFIG_HOME=$PSCALI_WORKDIR/.config`
  - `MICRO_CONFIG_HOME=$PSCALI_WORKDIR/.config/micro`
- 2026-02-28: Fixed embedded micro status masking:
  - `pscalMicroMain` defer now preserves existing panic/exit status instead of overwriting with `exit(0)`.
  - `config.InitConfigDir` failure now exits non-zero immediately.
- 2026-02-28: Hardened script-phase tool resolution for cross-machine Xcode builds:
  - `ios/Tools/build_micro_embed_archive.sh` now resolves `go` from PATH by default (no hardcoded `/opt/homebrew/bin/go` requirement).
  - `ios/Tools/ensure_static_libs.sh` now resolves CMake via `CMAKE_BIN`/`PATH` with explicit fail-fast diagnostics when missing.
  - `ios/Tools/build_micro_embed_archive.sh` now forces `TMPDIR` to a writable build-local temp directory to prevent cgo/clang temporary-file failures under restricted environments.
- 2026-02-28: Fixed embedded micro panic masking during startup failure handling:
  - `third-party/micro/cmd/micro/micro.go` now wraps `screen.Screen.Fini()` in panic-safe cleanup so `close of nil channel` in partially initialized tcell state does not hide the real error.
  - `third-party/micro/cmd/micro/pscal_embed.go` now prints a full panic stack (`runtime/debug.Stack`) to stderr for embedded-mode panics.
- 2026-02-28: Extended deterministic tcell iOS patching in `ios/Tools/build_micro_embed_archive.sh`:
  - `tscreen_unix.go` is now patched to allow embedded PSCAL runs to continue when `term.MakeRaw` fails under iOS container PTY constraints (`PSCALI_WORKDIR` present), rather than hard-failing startup.
  - `termioFini` cleanup in patched `tscreen_unix.go` is nil-safe for partially initialized channels/signals.
  - Darwin `closeTty` patch now avoids closing process stdio handles (`os.Stdin`/`os.Stdout`/`os.Stderr`) when stdio fallback is used.
- 2026-02-28: Added deterministic tcell `tscreen.go` patch in micro embed build to guard `close(t.quit)` with nil checks during `finish()` so partial-init `Fini()` paths cannot panic with `close of nil channel`.
- 2026-02-28: Aligned `smallclueRunMicro` terminal-mode lifecycle with `nextvi`:
  - save/restore termios via `termios_shim` (`smallclueTcgetattr`/`smallclueTcsetattr`)
  - enter raw mode before micro launch and flush input/output on restore
  - reset terminal control state/alt-screen on return (`\033[r ... \033[?1049l ...`)
  - preserve strict no-fallback behavior (still direct `pscal_micro_main_entry` dispatch only)
- 2026-02-28: Fixed embedded micro terminal attachment for iOS virtual stdio:
  - `smallclueRunMicro` now installs a host-fd stdio bridge (host `0/1/2` <-> PSCAL vproc stdio) around `pscal_micro_main_entry` so Go/tcell direct syscalls attach to the active PSCAL terminal session.
  - removed the orphan PTY fallback in `micro_app.c` (`microOpenPty` path) that could leave micro detached from the visible terminal.
  - verified compile/integration with:
    - `cmake --build --preset ios-simulator --target pscal_core_static`
    - `SDK_NAME=iphonesimulator bash ios/Tools/ensure_static_libs.sh`
- 2026-02-28: Added PTY visibility to `top` for runtime debugging:
  - `VProcSnapshot` now records per-vproc fake PTY number (`tty_pty_num`).
  - iOS vproc snapshot population resolves `pid -> vproc stdio fake-pty` from each task's `VPROC_FD_PSCAL` fd-table entries (stdin/stdout/stderr), not from session-level controlling tty mapping.
  - `top` now renders a `PTY` column (`pts/<n>` or `-`) in both tree and flat modes.
- 2026-02-28: Fixed micro host-bridge stdio detachment risk:
  - `smallclueRunMicro` now skips `/dev/tty` stdio rebinding when the iOS host-stdio bridge is active, so embedded micro stays bound to the bridge path (`host 0/1/2 <-> active vproc fake PTY`).
- 2026-02-28: Fixed micro host-bridge session-input routing:
  - bridge thread I/O on vproc endpoints now uses `vprocReadShim`/`vprocWriteShim` (not raw `read`/`write`) so duplicated stdin descriptors are routed through PSCAL's shared session input queue and job-control aware tty path.
- 2026-02-28: Replaced micro host bridge transport with host PTY:
  - embedded micro now runs on a real host PTY slave (`host 0/1/2`) instead of anonymous pipes, while bridge threads relay between host PTY master and PSCAL vproc stdio.
  - this preserves tty/ioctl semantics expected by Go/tcell while keeping I/O attached to the active PSCAL fake PTY session.
- 2026-02-28: Hardened micro bridge worker thread I/O:
  - bridge worker threads now use direct fd `read`/`write` on vproc-side duplicated descriptors (instead of `vprocReadShim`/`vprocWriteShim`) to avoid shim/session-context deadlocks from non-vproc helper threads.
- 2026-02-28: Added embedded micro re-entry crash guards:
  - `third-party/micro/cmd/micro/pscal_embed.go` now rejects concurrent embedded launches with explicit `micro: already running` and non-zero return, preventing overlapping in-process runs from corrupting shared micro/tcell global state.
  - `third-party/micro/cmd/micro/micro.go` event poll goroutine now has panic-safe recovery and uses invocation-local screen/event channel bindings, reducing cross-session goroutine interference during repeated embedded launches.
  - embedded `signal.Notify` no longer subscribes to `SIGABRT` in micro's main loop.
  - compile validation: `cmake --build --preset ios-simulator --target pscal_core_static`.
- 2026-02-28: Added iOS vproc interpose bypass around embedded micro entry:
  - `smallclueRunMicro` now wraps `pscal_micro_main_entry(...)` with `vprocRegisterInterposeBypassThread` + `vprocInterposeBypassEnter/Exit`, and unregisters on return.
  - intent: isolate Go runtime syscalls (notably runtime netpoll initialization/use) from active vproc shim context to prevent host fd corruption/EBADF fatal paths during embedded launch.
  - compile validation:
    - `cmake --build --preset ios-simulator --target pscal_core_static`
    - `cmake --build --preset ios-device --target pscal_core_static`

## Entrypoint Naming Notes

1. `nextvi` integration renames its C `main` to `nextvi_main_entry` at compile time.
2. `micro` integration exports `pscal_micro_go_main_entry` from Go `c-archive` mode and wraps it via `pscal_micro_main_entry`.
3. Inside micro, the CLI body is in `pscalMicroMain()`; `main()` is only a thin wrapper for standalone builds.
4. No public `_main` symbol is used for micro integration in the final archive path.
