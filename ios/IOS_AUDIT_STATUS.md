# iOS Audit Status

Last updated: 2026-01-03

Legend: Open, In Progress, Needs Verification, Fixed, Blocked

## Testing
- 2026-01-03: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (inactive tabs now detach webviews + input focus gated by active tab).
- 2026-01-03: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (tab selection now drives active focus + hidden views resign first responder).
- 2026-01-03: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (tab views kept mounted to avoid Hterm detach on switch).
- 2026-01-03: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (ignore hterm detach on window loss + debug attach state).
- 2026-01-03: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (tab view identity + shell/ssh hterm log updates).
- 2026-01-03: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (hterm size gating + session I/O debug logging compile check).
- 2026-01-02: `cmake --build build/ios-host --target exsh` succeeded (vproc stdio activation debug + session I/O launch logging; existing unused helper warnings in vproc).
- 2026-01-02: `cmake --build build/ios-host --target exsh` succeeded (session/tool threads now apply runtime context TLS).
- 2026-01-02: `cmake --build build/ios-host --target exsh` succeeded (vprocCreate now adopts session pscal stdio).
- 2026-01-02: `cmake --build build/ios-host --target exsh` succeeded (session TIOCSCTTY parity with iSH).
- 2026-01-02: `cmake --build build/ios-host --target exsh` succeeded (shell interactive termios/signal state made thread-local).
- 2026-01-02: `cmake --build build/ios-host --target exsh` succeeded (shell session thread stack size set to 32 MiB).
- 2026-01-02: `cmake --build build/ios-host --target exsh` succeeded (per-session shell pid propagation + Mac Catalyst iOS shim enablement).
- 2026-01-02: `cmake --build build/ios-host --target exsh` succeeded (tool threads now inherit vproc session stdio).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (hterm install/uninstall logging).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (removed main-terminal fallback renderer).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (removed main-terminal fallback renderer to match iSH).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (hterm install/uninstall on load parity).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (hterm ScrollbarView autoresizing + opaque/scroll flag parity).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (hterm ScrollbarView contentView origin drift fix).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath ios/DerivedData/PSCAL build` succeeded (hterm loaded-state tracking + z-order guard for fallback overlay).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath ios/DerivedData/PSCAL build` succeeded (hterm ScrollbarView contentView sync + scroll debug logging).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath ios/DerivedData/PSCAL build` succeeded (hterm ScrollbarView parity for scrollback/viewport positioning).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath ios/DerivedData/PSCAL build` succeeded (hterm immediate markHtermLoaded when already loaded).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath ios/DerivedData/PSCAL build` succeeded (hterm syncFocus parity adjustment).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath ios/DerivedData/PSCAL build` succeeded (hterm CustomWebView + large initial frame parity).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath ios/DerivedData/PSCAL build` succeeded (hterm attach/detach parity + focus sync + style apply on load).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath ios/DerivedData/PSCAL build` succeeded (hterm iso-2022 write path + applicationCursor sync + input scroll-to-bottom parity).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath ios/DerivedData/PSCAL build` succeeded (hterm scroll sync hooks wired for scrollback).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (hterm window visibility no longer gated on key window; focus retry for non-key).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (lazy hterm controller init to avoid dispatch_once deadlock).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (main-thread hterm controller init to avoid WebKit crash).
- 2026-01-02: `cmake --build build/ios-host --target exsh` succeeded (stdio shim compile check; existing unused helper warnings in shell backend).
- 2026-01-02: `cmake --build build/ios-host --target exsh` succeeded (shell vproc stdio funopen + tool runner bypass for active session stdio).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (hterm active window/key visibility gating).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (hterm visibility attach/detach wiring + instance id logging).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (hterm debug logs capture cursor row text + computed screen style).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (hterm debug hex/ascii dump on first writes).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (hterm debug row/cursor logging + screen outline).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (hterm debug flag injection + visible default colors + JS write fixes).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (direct output now switches to session context before queueing output).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (hterm + runtime output logging instrumentation).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (hterm reload/reset handling + color fallback + JS error logging).
- 2026-01-02: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (prompt/output buffering changes).
- 2026-01-02: `RUN_VPROC_TESTS=1 python Tests/exsh/exsh_test_harness.py --executable build/ios-host/bin/exsh --only jobs_wait_all` failed (stdout mismatch: only one active job listed after wait).
- 2026-01-02: Jobspec kill regression stabilized (job id collision guard + terminate cleanup); `jobspec_kill` suite passes in ios-host build.
- 2026-01-02: `RUN_VPROC_TESTS=1 python Tests/exsh/exsh_test_harness.py --executable build/ios-host/bin/exsh --only jobspec_kill` passed.
- 2026-01-02: `cmake --build build/ios-host --target exsh` succeeded (post jobspec fixes).
- 2026-01-02: `cmake --build build/ios-host --target exsh` succeeded (runtime session stub made weak to avoid duplicate symbol).
- 2026-01-01: `cmake --build build/ios-device` succeeded (smallclue link check after session-context stubs).
- 2026-01-01: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (runtime interpose disabled for app build).
- 2026-01-01: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (Swift runtime context wrappers + session registration).
- 2026-01-01: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (runtime context TLS + API refactor).
- 2026-01-01: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (context refactor compile check).
- 2026-01-01: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (compile check after P2 fixes).
- 2026-01-01: `Tests/run_ios_vproc_tests.sh` passed after enabling explicit PATH_TRUNCATE virtualization outside vproc and relaxing default session-leader handling.
- 2026-01-01: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' build` failed (DerivedData permission + CoreSimulator connection errors).
- 2026-01-01: `cmake --build build` reported no work to do.
- 2026-01-01: `Tests/run_exsh_ios_host_tests.sh` failed while building OpenSSL (missing sysroot headers, `sys/types.h`).
- 2026-01-01: `Tests/run_exsh_ios_host_tests.sh` built `exsh` but failed jobspec kill cases (`jobspec_kill_stability`, `jobspec_kill_vproc_required`, `jobspec_kill_shell_vproc`) with return code 1.
- 2026-01-01: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` failed (CoreSimulator connection invalid; actool reports no available simulator runtimes).
- 2026-01-01: Bash parity check shows `Tests/exsh/tests/jobspec_kill.exsh` fails under bash (killed job still appears as Terminated), so expectations may need adjustment alongside vproc job id fixes.
- 2026-01-01: `cmake --build build/ios-host --target exsh` succeeded after P0 fixes.
- 2026-01-01: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` failed (CoreSimulator connection invalid; no simulator runtimes for asset catalog).
- 2026-01-01: `cmake --build --preset ios-simulator --target pscal_core_static pscal_exsh_static pscal_pascal_static pscal_clike_static pscal_rea_static pscal_vm_static pscal_json2bc_static pscal_pscald_static pscal_tool_runner` succeeded (rebuild triggered OpenSSL/curl; warnings in `src/backend_ast/shell.c` about unused helpers).
- 2026-01-01: `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath build/DerivedData-ios build` succeeded (arm64 simulator build).

## Follow-ups
- Re-test multi-tab shell sessions after applying runtime context TLS in session/tool threads.
- Re-test multi-tab shell sessions after setting controlling TTY (TIOCSCTTY) on session start.
- Re-test multi-tab shell sessions after making interactive termios state thread-local.
- Re-test `rea` and `clike` frontends after increasing shell session thread stack size.
- Re-run `Tests/run_exsh_ios_host_tests.sh` to cover the full ios-host jobs/vproc suite after the jobspec fixes.
- Investigate `jobs_wait_all` stdout mismatch (unexpected job-start lines and missing second job entry in `jobs` output).
- Investigate missing shell prompt on app launch and new tabs, plus shell restarts when switching tabs (hterm attach/detach vs session lifetime).
- Retest startup after moving hterm controller init onto the main thread (previous WebKit crash when instantiated on background queues).
- Retest startup after lazy hterm controller creation (avoid dispatch_once deadlock between location queue and main thread).
- Confirm Pascal tool runner output still bypasses hterm (smallclue applets now OK; `pwd` OK); verify `PSCALI_TOOL_RUNNER_PATH` visibility and vproc session activation for tool threads.
- Confirm hterm visibility attach/detach logs show the active instance id when switching tabs or returning to the main terminal.
- Capture hterm JS debug logs (terminalReady/updateStyle/writeBytes row count) with `PSCALI_HERM_DEBUG=1` or `PSCALI_PTY_OUTPUT_LOG=1` to confirm output reaches the DOM.
- Verify hterm web content reset handling + color fallback now restores prompts after WebContent crashes.
- Compare iSH PTY/TTY handoff + backpressure to PSCAL's vproc output threads to align output flow and resume semantics.
- Re-test tab switching after persistent hterm controller changes (webview reuse per session) to confirm prompts persist.
- If Intel simulator support is needed, add an x86_64 build preset or fat simulator libs (current iOS simulator libs are arm64-only).
- Validate multi-tab behavior after per-session shell pid propagation (prompts in new tabs and no foreground stalls).
- Re-test Pascal frontend output on Mac Catalyst builds now that iOS shims are enabled in CMake.

## Priority 0 - Stability & Safety
| ID | Issue | Files | Status | Notes |
| --- | --- | --- | --- | --- |
| P0-A | argv missing NULL terminator | ios/Sources/App/SshRuntimeSession.swift, ios/Sources/App/ShellRuntimeSession.swift, ios/Sources/App/PscalRuntimeBootstrap.swift | Needs Verification | Appended `nil` terminators for session argv; `PscalRuntimeBootstrap` already appended. |
| P0-B | Use-after-free in output callbacks | ios/Sources/App/SshRuntimeSession.swift, ios/Sources/App/ShellRuntimeSession.swift, ios/Sources/App/PscalRuntimeBootstrap.swift | Needs Verification | Switched to retained handler contexts with callback draining before release. |
| P0-C | Interposer recursion risk | ios/Sources/Bridge/PSCALInterpose.c | Needs Verification | `pscalRawIoctl` now resolves via explicit libSystem/kernel handle only. |
| P0-D | Output buffer ownership coupling | ios/Sources/App/PscalRuntimeBootstrap.swift, ios/Sources/Bridge/PSCALRuntime.mm | Needs Verification | `consumeOutput` now uses `Data(bytesNoCopy:...,.free)` to own C buffer. |

## Priority 1 - Performance & Battery
| ID | Issue | Files | Status | Notes |
| --- | --- | --- | --- | --- |
| P1-A | Greedy output drain loop | ios/Sources/App/SshRuntimeSession.swift, ios/Sources/App/ShellRuntimeSession.swift | Needs Verification | Added per-cycle byte/iteration caps with queued follow-up drain. |
| P1-B | PTY pump busy-wait | ios/Sources/Bridge/PSCALRuntime.mm, src/ios/vproc.c | Needs Verification | Added poll-based wait for read readiness and EAGAIN backoff. |
| P1-C | Blocking input writes on background queue | ios/Sources/Bridge/PSCALRuntime.mm | Needs Verification | Added async input queue + writer thread. |

## Priority 2 - Logic & UX Correctness
| ID | Issue | Files | Status | Notes |
| --- | --- | --- | --- | --- |
| P2-A | Canonical backspace echo | ios/Sources/Bridge/PSCALRuntime.mm | Fixed | Echoes `\\x08 \\x08` when VERASE consumes a byte in canonical mode. |
| P2-B | Double backspace cursor move | ios/Sources/App/TerminalBuffer.swift | Fixed | `handleEchoScalar` routes 0x7F to `applyBackspaceErase` without an extra cursor move. |
| P2-C | Signal + input duplication | ios/Sources/Bridge/PSCALRuntime.mm | Fixed | Signal bytes are consumed without forwarding to stdin. |
| P2-D | Resize data loss | ios/Sources/App/TerminalBuffer.swift | Fixed | `adjustRowCount` appends trimmed rows into scrollback before removal. |
| P2-E | UTF-8 decoder data drop | ios/Sources/App/TerminalBuffer.swift | Fixed | Invalid continuation bytes are replayed after emitting U+FFFD. |

## Priority 3 - Architecture & Compliance
| ID | Issue | Files | Status | Notes |
| --- | --- | --- | --- | --- |
| P3-A | Single-session limitation (global state) | ios/Sources/Bridge/PSCALRuntime.mm, ios/Sources/Bridge/PSCALRuntime.h, ios/Sources/App/PscalRuntimeBootstrap.swift, ios/Sources/App/ShellRuntimeSession.swift, ios/Sources/App/SshRuntimeSession.swift, src/ios/vproc.c, src/ios/vproc.h, src/shell/main.c, src/backend_ast/shell/shell_builtins.inc | Needs Verification | Swift now sets per-session runtime context/TLS, registers session contexts, and destroys contexts on exit; shell session + tool threads now apply runtime context TLS; vproc propagates per-session shell pid into TLS when stdio is activated to avoid cross-session job-control interference. |
| P3-B | App Store risk from interposition | ios/Sources/Bridge/PSCALInterpose.c | Needs Verification | Runtime interpose disabled when `PSCALI_IOS_APP` is set; compile-time vproc shims remain the execution path. |

## Additional Issues
| ID | Issue | Files | Status | Notes |
| --- | --- | --- | --- | --- |
| A1 | Stdout/stderr bypasses vproc in applets | src/ios/vproc_stdio_shim.h, src/ios/vproc_shim.h, src/backend_ast/shell/shell_builtins.inc | Needs Verification | Added stdio shim to route printf/fwrite/perror stdout/stderr through `vprocWriteShim` and pass session stdio into tool threads; smallclue applets now OK, Pascal tool output still needs verification. |
| A11 | Mac Catalyst lacks iOS shims | CMakeLists.txt | Needs Verification | Treat Mac Catalyst as iOS for shim/`PSCAL_TARGET_IOS` enablement while keeping host OpenSSL. |
| A12 | Session controlling TTY parity with iSH | src/shell/main.c | Needs Verification | Shell now issues `TIOCSCTTY` for PTY-backed sessions after adopting pscal stdio (matches iSH session init). |
| A13 | vprocCreate ignores pscal stdio | src/ios/vproc.c | Needs Verification | vprocCreate now adopts pscal stdio from the active session when using PTY-backed sessions (debug via `PSCALI_VPROC_DEBUG`). |
| A2 | Hterm controller recreated on tab switch | ios/Sources/App/HtermTerminalView.swift, ios/Sources/App/ShellRuntimeSession.swift, ios/Sources/App/SshRuntimeSession.swift, ios/Sources/App/PscalRuntimeBootstrap.swift | Needs Verification | Persisted one `HtermTerminalController` per session/runtime; views now reattach to the same webview instead of rebuilding it. Runtime controller is now lazily created on main to avoid WebKit init crashes and dispatch_once deadlocks. |
| A15 | SwiftUI tab reuse confuses hterm controller | ios/Sources/App/ShellTerminalView.swift, ios/Sources/App/SshTerminalView.swift | Needs Verification | Keyed per-session tab content with `.id(session.sessionId)` so SwiftUI recreates the representable and coordinator per session. |
| A16 | Tabs detach webviews on switch | ios/Sources/App/SshWindowManager.swift, ios/Sources/App/HtermTerminalView.swift | Needs Verification | Keep all tab views mounted in a ZStack (opacity/hit-testing toggle) and ignore detach-on-window-loss to keep Hterm attached across tab switches. |
| A3 | Hterm scrollback not wired | ios/Sources/App/HtermTerminalView.swift, ios/Sources/App/TerminalWeb/term.js | Needs Verification | Added scroll height/top bridging and external scroll view sync to mirror iSH scrollback behavior. |
| A4 | Hterm application cursor + input parity | ios/Sources/App/HtermTerminalView.swift, ios/Sources/App/TerminalInputBridge.swift | Needs Verification | `propUpdate` now tracks `applicationCursor`, arrow/return sequences honor mode, and input triggers `setUserGesture` + `scrollToBottom` like iSH. |
| A5 | Hterm output encoding parity | ios/Sources/App/HtermTerminalView.swift, ios/Sources/App/TerminalWeb/term.js | Needs Verification | Output now uses ISO-2022 prefs with Latin-1 JS write path to match iSH `exports.write` behavior. |
| A6 | Hterm attach/focus parity | ios/Sources/App/HtermTerminalView.swift | Needs Verification | Attach/detach now follows view lifetime (not key-window gating); window key state drives `setFocused` like iSH. |
| A7 | Hterm web view parity (initial frame + action suppression) | ios/Sources/App/HtermTerminalView.swift | Needs Verification | Match iSH `CustomWebView` behavior and large initial frame to avoid zero-size layout quirks. |
| A8 | Hterm syncFocus parity | ios/Sources/App/HtermTerminalView.swift | Needs Verification | `syncFocus` now reasserts focus without forcing first responder, mirroring iSH behavior. |
| A9 | Hterm ScrollbarView parity | ios/Sources/App/HtermTerminalView.swift | Needs Verification | Scrollback now uses ScrollbarView to keep WKWebView pinned while scrolling, matching iSH behavior; avoid resetting `contentView` during layout and drop origin adjustment on assignment to prevent drift; switch to autoresizing (no constraints) and match iSH clear/opaque + clip settings; install/uninstall web view only when `loaded` like iSH. |
| A10 | Hterm fallback renderer removal | ios/Sources/App/TerminalView.swift | Needs Verification | Main terminal no longer overlays the fallback renderer, matching iSH’s always-hterm pipeline and avoiding occlusion. |
| A14 | Hterm output flush before host size | ios/Sources/App/HtermTerminalView.swift | Needs Verification | Gate output flush until host view size is known; trigger hterm resize on layout/load/install to avoid giant row counts and off-screen output in new tabs. |
| A17 | Active tab focus gating | ios/Sources/App/SshWindowManager.swift, ios/Sources/App/TerminalView.swift, ios/Sources/App/ShellTerminalView.swift, ios/Sources/App/SshTerminalView.swift, ios/Sources/App/HtermTerminalView.swift | Needs Verification | Tab selection now triggers focus for the active view; inactive tabs detach webviews, resign first responder, and block focus/attach until reselected. |
