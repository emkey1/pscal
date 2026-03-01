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

## Current Field Status (2026-03-01)

- `micro` no longer fails bridge initialization on second launch in the same tab (re-entry regression fixed).
- PTY bridge behavior remains sensitive to host PTY availability; shim PTY fallback is enabled by default and can be disabled with `PSCALI_MICRO_ALLOW_SHIM_PTY=0` for diagnostics.
- Bridge startup is strict by default (`PSCALI_MICRO_BRIDGE_STRICT=1` behavior): if no PTY is available, launch aborts with a bridge error instead of entering a blank non-PTY run.
- Observed on-device failure mode: host PTY allocation can fail with `errno=1 (Operation not permitted)` for active sessions, confirming sandbox denial of host PTY creation on some launches/devices.
- Current fallback order in `micro_app.c` is now:
  - host PTY (`/dev/ptmx` family),
  - host `openpty()`,
  - shim PTY (`vprocOpenShim("/dev/ptmx")`, unless disabled with `PSCALI_MICRO_ALLOW_SHIM_PTY=0`),
  - host pipe relay bridge (only used in non-strict mode).
- User-reported interactive parity gaps remain open in embedded micro:
  - Enter/Return handling is inconsistent in micro while other tools continue to receive input correctly.
  - Mouse input emits escape/control sequences instead of expected micro mouse interactions.
  - Resize/input behavior still needs a dedicated end-to-end stabilization pass for embedded micro.
- `ioctl` is now interposed on Apple targets in `vproc` and routed through `vprocIoctlShim`, so Darwin libc ioctl callers (including Go `x/sys/unix` `IoctlGetWinsize`) use the virtual PTY/session-aware path instead of bypassing shim state.
- Latest field trace (2026-03-01, iPadOS) confirms the winsize pipeline is active after session binding:
  - `hterm[...] runtime-forward source=native session=7 ...`
  - `runtime updateSessionWindowSize session=7 ... rc=0`
  - `vproc setSessionWinsize applied session=7 ...`
- Early `vproc setSessionWinsize missing-pty` for sessions 6/7 appears before tab/session PTY registration completes; treat this as startup ordering/race, not final steady-state behavior.
- Current blocking failure is no longer "winsize not delivered": `micro` still fails to render/open in-tab and leaves shell state stuck (typed `micro` remains, Return does not produce a fresh prompt) despite successful repeated winsize applies.
- Cross-session resize bleed mitigation is now in-tree (`pscalMicroNotifySessionWinsize` only applies updates for the active bridge session id); field validation is still pending.
- Bridge resize path now sends explicit `SIGWINCH` to micro's launch thread after applying `TIOCSWINSZ`, so embedded tcell receives resize notifications even when PTY-generated job-control signaling is unreliable.
- `nextvi` path-display hardening is now in-tree: absolute paths are normalized through `pathTruncateStrip` before buffer store/lookup (`bufs_open`, `ex_edit`, `ec_edit`, `ec_setpath`, `ec_read`) so "Present sandbox as /" is honored even if launch arguments arrive as container-prefixed host paths.
- Follow-up `nextvi` write-failure investigation (2026-03-01): an attempted global path-virtualization fd-routing change (`open/fopen/freopen` -> `vprocOpenShim` when vproc-active) regressed tab shell startup and was rolled back the same day.
- Current status: tab shell spawn stability takes priority; `nextvi` save-path fix must remain editor-local and avoid global stdio/path virtualization behavior changes.
- `nextvi` now has editor-local fd tracking for file I/O on iOS: file descriptors returned by `open`/`mkstemp` in nextvi write/read paths are adopted into the active vproc (`vprocAdoptHostFd`) before `read`/`write`/`close`, avoiding host-fd vs virtual-fd collisions under interposed I/O.
- `nextvi` launch thread now force-syncs cwd from the invoking shell vproc (`vprocShellGetcwdShim` -> `vprocChdirShim`, plus `PWD`) before editor startup to keep relative path opens stable across repeated invocations.
- `nextvi` argv file targets are now normalized to absolute virtual paths at launch (`/home/...`) using shell cwd, so reopening newly created relative files resolves consistently even if thread-local cwd drifts.
- Nextvi cwd resolution now prefers shell `PWD` (then vproc cwd, then host cwd) before argv normalization and thread cwd sync, reducing cross-context cwd drift when reopening relative filenames.

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
- 2026-03-01: Added host PTY allocation fallback in `src/smallclue/src/micro_app.c`:
  - bridge setup now attempts host `openpty()` when `/dev/ptmx` family paths are unavailable.
  - host PTY remains the default required path for in-tab rendering/input correctness.
- 2026-03-01: Retained strict fallback policy for embedded micro bridge:
  - shim PTY fallback remains disabled by default and only enabled via `PSCALI_MICRO_ALLOW_SHIM_PTY=1`.
  - rationale: avoid regressions where micro output appears in Xcode console rather than the active PSCAL terminal tab.
- 2026-03-01: Verified static-library rebuilds after bridge updates:
  - `cmake --build build/ios-device --target pscal_core_static`
  - `cmake --build build/ios-simulator --target pscal_core_static`
- 2026-03-01: Corrected tracking source:
  - this document (`Docs/ios_micro_nextvi_integration_contract.md`) is the authoritative integration tracker for ongoing micro/nextvi iOS work; earlier updates accidentally written to `Notes/smallclue.md` were superseded here.
- 2026-03-01: Relaxed bridge startup policy in `src/smallclue/src/micro_app.c`:
  - `microHostStdioBridgeSetup` failures now emit explicit `errno` + strerror diagnostics.
  - `smallclueRunMicro` now continues without bridge on setup failure (instead of hard aborting), unless `PSCALI_MICRO_BRIDGE_STRICT=1` is set.
  - compile validation:
    - `cmake --build build/ios-device --target pscal_core_static`
    - `cmake --build build/ios-simulator --target pscal_core_static`
- 2026-03-01: Added pipe-relay bridge fallback in `src/smallclue/src/micro_app.c` for host-PTY-denied sessions:
  - when host PTY and optional shim PTY setup are unavailable, micro stdio is redirected through host pipes and bridged to session input/output threads.
  - goal: keep embedded micro attached to the active terminal tab even when iOS denies host PTY creation (`errno=1`).
  - compile validation:
    - `cmake --build build/ios-device --target pscal_core_static`
    - `cmake --build build/ios-simulator --target pscal_core_static`
- 2026-03-01: Corrected iOS bridge policy to prefer vproc virtual PTY by default:
  - `PSCALI_MICRO_ALLOW_SHIM_PTY` now defaults to enabled (can be disabled explicitly with `PSCALI_MICRO_ALLOW_SHIM_PTY=0` for diagnostics).
  - intent: avoid host-PTY dependency on iOS where `/dev/ptmx` is sandbox-denied (`errno=1`) and keep micro aligned with vproc virtual PTY architecture.
  - compile validation:
    - `cmake --build build/ios-device --target pscal_core_static`
    - `cmake --build build/ios-simulator --target pscal_core_static`
- 2026-03-01: Added host stdio capture guardrail to prevent Xcode-console leakage:
  - micro bridge now always installs host stdin/stdout/stderr capture pipes while active, even when vproc virtual PTY mode is enabled.
  - session input is relayed into captured stdin, and captured stdout/stderr is relayed back via `vprocSessionEmitOutput`, so writes that bypass vproc interpose still remain in-tab.
  - bridge failure policy is now strict by default (`PSCALI_MICRO_BRIDGE_STRICT=1` default behavior; set `PSCALI_MICRO_BRIDGE_STRICT=0` to allow degraded no-bridge launch for diagnostics).
  - compile validation:
    - `cmake --build build/ios-device --target pscal_core_static`
    - `cmake --build build/ios-simulator --target pscal_core_static`
- 2026-03-01: Corrected stale bridge-mode notes after field validation:
  - the "always install host stdio capture pipes" path above was superseded; it caused blank-screen launches when micro/tcell lost PTY semantics.
  - current behavior in `src/smallclue/src/micro_app.c` is mode-separated:
    - PTY mode (`host` or `vproc shim`): stdio stays attached to PTY slave, session input is injected into PTY master, output is drained from PTY master to `vprocSessionEmitOutput`.
    - pipe relay mode (no PTY available): stdio capture pipes are used.
  - strict bridge policy now treats pipe-relay-only startup as failure (`micro: unable to initialize PTY bridge (strict mode, no PTY available)`) instead of launching into blank output.
  - compile validation:
    - `cmake --build build/ios-device --target pscal_core_static`
    - `cmake --build build/ios-simulator --target pscal_core_static`
- 2026-03-01: Fixed repeated `TIOCGWINSZ` bypass on embedded micro by interposing libc `ioctl` at `vproc` layer:
  - added Apple dyld interpose hook in `src/ios/vproc.c` mapping `ioctl` -> `vprocIoctlShim`.
  - interpose path bypasses itself safely via existing `vprocInterposeBypass*` gates and falls through to host raw ioctl when interpose is not ready.
  - goal: make Go `x/sys/unix` ioctl callers (tcell winsize probing) honor virtual PTY/session stdio mapping instead of host-only fd semantics.
  - compile validation:
    - `cmake --build build/ios-device --target pscal_micro_embed_archive`
    - `cmake --build build/ios-simulator --target pscal_micro_embed_archive`
    - direct compile checks for `src/ios/vproc.c` object in both `build/ios-device` and `build/ios-simulator` via ninja command extraction.
- 2026-03-01: Field incident review (window-size registration focus) from Xcode console trace:
  - observed startup-order misses: `vproc setSessionWinsize missing-pty` before stable session<->PTY binding.
  - observed steady-state success: repeated `runtime updateSessionWindowSize ... rc=0` and `vproc setSessionWinsize applied ...` for active session 7.
  - no direct evidence in this trace of bridge setup abort, embedded panic, or `micro: already running` rejection.
  - failure mode persists: micro UI does not appear, shell prompt remains blocked after `micro`; debugging focus shifts to launch/bridge/job-control lifecycle after successful winsize registration.
  - next capture requirements:
    - `PSCALI_MICRO_RESIZE_TRACE=1`
    - `PSCALI_MICRO_DEBUG=1`
    - `PSCALI_IO_DEBUG=1`
    - `PSCALI_SSH_RESIZE_DEBUG=1`
    - `PSCALI_PTY_TRACE=1`
    - confirm whether `micro bridgeSetup active`, `micro notifySessionWinsize ... match=...`, and `micro bridgeApplySessionWinsize ... applied=...` stay pinned to the launch session id.
- 2026-03-01: Winsize registration hardening in `src/smallclue/src/micro_app.c`:
  - `pscalMicroNotifySessionWinsize` no longer applies non-matching session updates when a bridge is active (removed drift-tolerant cross-session fallback).
  - `microSignalResizeLocked` now delivers `SIGWINCH` to the tracked micro launch thread after bridge winsize updates.
  - intent: stop multi-tab resize cross-talk (session 6/7 interleave) and ensure embedded tcell sees resize events even without reliable PTY-driven signal propagation.
  - compile validation:
    - `cmake --build build/ios-simulator --target pscal_core_static`
    - `cmake --build build/ios-device --target pscal_core_static`
- 2026-03-01: Added bridge-thread exit diagnostics in `src/smallclue/src/micro_app.c`:
  - `microIoBridgeThreadMain` now emits `[micro-resize] micro ioBridgeThread exit ...` with reason/errno and read/write mode flags.
  - intent: make it explicit whether micro launch stalls come from bridge read EOF, write failure, or stop-request teardown.
  - capture requirement: enable `PSCALI_MICRO_RESIZE_TRACE=1` for these exit logs.
  - compile validation:
    - `cmake --build build/ios-simulator --target pscal_core_static`
    - `cmake --build build/ios-device --target pscal_core_static`
- 2026-03-01: Fixed `nextvi` not honoring "Present sandbox as /" when opened paths were host-expanded:
  - `third-party/nextvi/ex.c` now normalizes absolute paths with `pathTruncateStrip` before path storage and buffer matching in `bufs_open`, `ex_edit`, `ec_edit`, `ec_setpath`, and `ec_read`.
  - intent: keep editor-visible paths virtualized (`/...`) even when argv/path sources provide container host paths.
  - compile validation:
    - `cmake --build build/ios-simulator --target pscal_core_static`
    - `cmake --build build/ios-device --target pscal_core_static`
- 2026-03-01: Rolled back attempted global fd-routing change in `src/common/path_virtualization.c`:
  - reverted behavior: non-device `open/fopen/freopen` no longer force `vprocOpenShim` under active vproc context.
  - reason: regression where new tabs no longer reliably spawned interactive shell sessions after launch.
  - compile validation after rollback:
    - `cmake --build build/ios-simulator --target pscal_core_static`
    - `cmake --build build/ios-device --target pscal_core_static`
- 2026-03-01: Added nextvi-local fd adoption fix (no global path virtualization changes):
  - `third-party/nextvi/ex.c` now routes file opens in editor read/write paths through helpers that adopt host fds into vproc (`vprocAdoptHostFd`) on iOS.
  - adopted paths include readonly opens, write-truncate fallback opens, and `mkstemp` temp-file creation used by atomic save.
  - `third-party/nextvi/vi.c` now includes `ios/vproc.h` for the adopted-fd path.
  - compile validation:
    - `cmake --build build/ios-simulator --target pscal_core_static`
    - `cmake --build build/ios-device --target pscal_core_static`
- 2026-03-01: Adjusted nextvi fd strategy for reopen-empty regression:
  - readonly file opens in `third-party/nextvi/ex.c` now stay as plain host `open(..., O_RDONLY)` (no vproc adopt).
  - rationale: avoid fd-number aliasing on readonly paths that could make existing files reopen as empty buffers.
  - write-path helpers (`mkstemp` / write-truncate fallback) remain on the adopted-fd path for save stability.
  - compile validation:
    - `cmake --build build/ios-simulator --target pscal_core_static`
    - `cmake --build build/ios-device --target pscal_core_static`
- 2026-03-01: Added nextvi cwd sync guard in `src/smallclue/src/nextvi_app.c`:
  - at editor thread activation, nextvi now aligns cwd/PWD with the shell session cwd before opening targets.
  - intent: prevent relative `nextvi <filename>` opens from drifting to a different virtual cwd and appearing as empty "new" buffers while the shell sees populated files.
  - compile validation:
    - `cmake --build build/ios-simulator --target pscal_core_static`
    - `cmake --build build/ios-device --target pscal_core_static`
- 2026-03-01: Added nextvi launch-argv path normalization in `src/smallclue/src/nextvi_app.c`:
  - non-option file arguments are rewritten to absolute virtual paths using shell cwd before thread launch.
  - absolute host/container-prefixed paths are stripped to virtual paths (`/home/...`) when path truncation is enabled.
  - intent: eliminate reopen mismatches where relative file opens resolved to a different cwd than the shell.
  - compile validation:
    - `cmake --build build/ios-simulator --target pscal_core_static`
    - `cmake --build build/ios-device --target pscal_core_static`
- 2026-03-01: Tightened nextvi cwd source ordering in `src/smallclue/src/nextvi_app.c`:
  - `smallclueResolveEditorCwd` now prefers `getenv("PWD")` (with path-truncate strip), then falls back to `vprocShellGetcwdShim`, then host `getcwd`.
  - same resolver is used for both argv absolute-path normalization and launch-thread `vprocChdirShim`/`PWD` sync.
  - intent: keep reopen target resolution identical to shell-visible cwd for relative `nextvi <filename>` usage.
  - compile validation:
    - `cmake --build build/ios-simulator --target pscal_core_static`
    - `cmake --build build/ios-device --target pscal_core_static`

## Entrypoint Naming Notes

1. `nextvi` integration renames its C `main` to `nextvi_main_entry` at compile time.
2. `micro` integration exports `pscal_micro_go_main_entry` from Go `c-archive` mode and wraps it via `pscal_micro_main_entry`.
3. Inside micro, the CLI body is in `pscalMicroMain()`; `main()` is only a thin wrapper for standalone builds.
4. No public `_main` symbol is used for micro integration in the final archive path.
