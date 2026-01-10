# PSCAL iPadOS Scope & Components

This document lists the major subsystems that influence the iPadOS build so we can review them piecemeal. Each section notes the key files and the kinds of stability checks we need before release.

## 1. Runtime Bootstrap & Bridge
- **Files**: `ios/Sources/Bridge/PSCALRuntime.mm`, `ios/Sources/App/PscalRuntimeBootstrap.swift`, `ios/Sources/App/PscalRuntimeBootstrap.swift`, `ios/Sources/App/TerminalInputBridge.swift`
- **Responsibilities**: Launching `exsh_main`, PTY/pipe setup, TTY emulation callbacks (`pscalTerminal*`), SDL/ASAN shims, Swift glue for output/input.
- **Review items**:
  - Thread safety around `s_runtime_mutex` and window-resize state.
  - Error handling when PTY allocation fails; verify fallback to virtual TTY on simulator.
  - Signal routing (`SIGWINCH`, `SIGPIPE`) and cleanup when the shell exits/crashes.
  - Swift bridge: ensure `EditorTerminalBridge` state stays consistent if exsh restarts automatically.

## 2. Terminal UI Layer
- **Files**: `ios/Sources/App/TerminalView.swift`, `ios/Sources/App/TerminalRendererView.swift`, `ios/Sources/App/TerminalDisplayTextView.swift`, `ios/Sources/App/TerminalSettingsView.swift`
- **Responsibilities**: Rendering the main VT100 surface, managing fonts/colors, handling input gestures.
- **Review items**:
  - Scrolling logic for both inline and Editor snapshots (recent fixes need validation).
  - Colour/theme propagation, especially between light/dark mode and the floating window.
  - Keyboard focus + resilience to hardware keyboard disconnects.
  - Settings persistence (font selection, colours, Editor toggle) and thread-safe notifications.

## 3. Floating Windows (Editor/Gwin)
- **Files**: `ios/Sources/App/EditorWindowManager.swift`, `ios/Sources/App/GwinWindowManager.swift`, `ios/Sources/App/TerminalEditorViewController.swift`
- **Responsibilities**: Managing secondary scenes, syncing geometry, input bridging.
- **Review items**:
  - Scene lifecycle (`requestSceneSessionActivation/Destruction`) and focus handoff when windows close.
  - Synchronization between `EditorTerminalBridge` snapshots and SwiftUI renderer.
  - Input pipeline (key repeat, modifiers) and cursor visibility.
  - Interaction with new task registry (future) / synthetic PIDs once implemented.

## 4. Smallclue Integration & GUI Hooks
- **Files**: `src/smallclue/core.c`, `src/smallclue/integration.c`, `src/smallclue/src/nextvi_app.c`, `src/common/pscal_terminal_host.c`
- **Responsibilities**: iOS-specific wrappers for applets, environment overrides, GUI shims.
- **Review items**:
  - Arg parsing fixes (e.g., `ls --color`) and regression risk for other applets.
  - Environment overrides for editor (TERM, TERMCAP) and cleanup across restarts.
  - `pscal_terminal_host`’s weak stubs vs iOS implementations; ensure there are no leak/buffer issues during frequent GUI opens.

## 5. Shell Runtime & Tool Runner
- **Files**: `src/backend_ast/shell/shell_execution.inc`, `src/backend_ast/shell/shell_builtins.inc`, `src/common/builtin_shared.c`, `src/backend_ast/builtin.c`
- **Responsibilities**: Command parsing, builtin dispatch, tool runner fallback on iOS.
- **Review items**:
  - `shellRunToolBuiltin` error paths on iOS (ensuring we always tear down tasks/threads).
  - Impact of GUI tasks on signal handling and job control.
  - History/prompt code paths that may behave differently without a real PTY.

## 6. Asset Installer & Sandbox Setup
- **Files**: `ios/Sources/App/RuntimeAssetInstaller.swift`, `ios/Sources/App/TerminalBuffer.swift`, `ios/Sources/App/TerminalGeometry.swift`
- **Responsibilities**: Copying runtime assets to `~/Documents`, configuring env vars, ensuring Examples/etc exist.
- **Review items**:
  - Failure modes when the user’s Documents directory is full or has existing conflicting files.
  - Permissions/symlink handling when migrating old installs.
  - Ensuring env vars (`HOME`, `PSCALI_*`) are always set before launching exsh.

## 7. Testing Harness / CI Scripts
- **Files**: `Docs/ios_build.md`, `ios/README.md`, potential xcconfigs.
- **Responsibilities**: Build/test instructions, toggles for simulator vs device.
- **Review items**:
  - Update docs with any new environment knobs (e.g., Editor floating toggle, synthetic tasks once added).
  - Ensure CI covers `cmake --preset ios-simulator` at least for static libs.

---
Next steps: for each component, perform a focused review (audit recent commits, run targeted tests on device/simulator), fix any stability issues, and document findings/conclusions. This checklist will serve as the roadmap for the “full code review” you requested.
