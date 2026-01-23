import SwiftUI
import UIKit
import Combine
import os

// MARK: - Main Terminal View

struct TerminalView: View {
    let showsOverlay: Bool
    let isActive: Bool
    let runtime: PscalRuntimeBootstrap

    @ObservedObject private var fontSettings = TerminalFontSettings.shared
    @State private var showingSettings = false

    init(showsOverlay: Bool = true, isActive: Bool = true, runtime: PscalRuntimeBootstrap) {
        self.showsOverlay = showsOverlay
        self.isActive = isActive
        self.runtime = runtime
    }

    var body: some View {
        if TerminalDebugFlags.printChanges {
            let _ = Self._printChanges()
            traceViewChanges("TerminalView body")
        }
        return ZStack(alignment: .bottomTrailing) {
            GeometryReader { proxy in
                TerminalContentView(
                    availableSize: proxy.size,
                    fontSettings: fontSettings,
                    isActive: isActive,
                    runtime: runtime
                )
                // Ensure each tab's terminal view gets its own UIKit backing view.
                .id(runtime.runtimeId)
                // We must frame the content view to the available size to ensure it fills the space
                .frame(width: proxy.size.width, height: proxy.size.height)
            }

            if showsOverlay {
                overlayButtons
            }
        }
        .sheet(isPresented: $showingSettings) {
            TerminalSettingsView()
        }
        .onChange(of: isActive) { active in
            if active {
                requestInputFocus()
            }
        }
        .onChange(of: showingSettings) { isPresented in
            if isPresented {
                hideSoftKeyboard()
            } else {
                requestInputFocus()
            }
        }
        .background(Color(fontSettings.backgroundColor))
    }

    // MARK: Overlay

    @ViewBuilder
    private var overlayButtons: some View {
        HStack(alignment: .center, spacing: 10) {
//            Button {
//                runtime.resetTerminalState()
//                requestInputFocus()
//                runtime.send(" ")
//                runtime.send("\u{7F}") // delete
//            } label: {
//                Text("RT")
//                    .font(.system(size: 14, weight: .semibold, design: .rounded))
//                    .padding(9)
//                    .background(.ultraThinMaterial, in: Circle())
//            }
//            .accessibilityLabel("Reset Terminal")

            Button {
                showingSettings = true
            } label: {
                Image(systemName: "textformat.size")
                    .font(.system(size: 16, weight: .semibold))
                    .padding(8)
                    .background(.ultraThinMaterial, in: Circle())
            }
            .accessibilityLabel("Adjust Font Size")
            
//            Button {
//                runtime.forceExshRestart()
//                requestInputFocus()
//            } label: {
//                Image(systemName: "arrow.triangle.2.circlepath")
//                    .font(.system(size: 16, weight: .semibold))
//                    .padding(8)
//                    .background(.ultraThinMaterial, in: Circle())
//            }
//            .accessibilityLabel("Restart exsh shell")
            
        }
        .padding(.bottom, 16)
        .padding(.trailing, 10)
    }

    private func hideSoftKeyboard() {
        UIApplication.shared.sendAction(#selector(UIResponder.resignFirstResponder),
                                        to: nil,
                                        from: nil,
                                        for: nil)
    }

    private func requestInputFocus() {
        NotificationCenter.default.post(name: .terminalInputFocusRequested, object: nil)
    }
}

// MARK: - Terminal Content View

struct TerminalContentView: View {
    static let topPadding: CGFloat = 32.0

    let availableSize: CGSize

    @ObservedObject private var fontSettings: TerminalFontSettings
    let runtime: PscalRuntimeBootstrap
    private let isActive: Bool

    @State private var lastReportedMetrics: TerminalGeometryCalculator.TerminalGeometryMetrics?
    @State private var hasMeasuredGeometry: Bool = false
    @State private var runtimeStarted: Bool = false
    @State private var initialFocusRequested: Bool = false
    @State private var promptKickRemaining: Int = 0
    @State private var localHtermLoaded: Bool = false
    @State private var htermController: HtermTerminalController?
    @State private var exitStatus: Int32?
    @State private var editorRenderTokenState: UInt64

    init(
        availableSize: CGSize,
        fontSettings: TerminalFontSettings,
        isActive: Bool,
        runtime: PscalRuntimeBootstrap
    ) {
        self.availableSize = availableSize
        _fontSettings = ObservedObject(wrappedValue: fontSettings)
        self.isActive = isActive
        self.runtime = runtime
        _exitStatus = State(initialValue: runtime.exitStatus)
        _editorRenderTokenState = State(initialValue: runtime.editorRenderToken)
    }

    var body: some View {
        if TerminalDebugFlags.printChanges {
            let _ = Self._printChanges()
            traceViewChanges("TerminalContentView body")
        }
        let editorTokenState = editorRenderTokenState
        let editorActive = runtime.isEditorModeActive()
        let editorVisible = EditorWindowManager.shared.isVisible
        let currentFont = fontSettings.currentFont
        let controller = htermController ?? runtime.htermControllerIfCreated()
        let htermReady = (controller != nil) && (runtime.htermReady || localHtermLoaded)

        let externalWindowEnabled = EditorWindowManager.externalWindowEnabled
        let showEditorSnapshot = editorActive && (!externalWindowEnabled || !editorVisible)
        let showBlank = editorActive && editorVisible && externalWindowEnabled
        return VStack(spacing: 0) {
            ZStack {
                if let controller {
                    HtermTerminalView(
                        controller: controller,
                        font: currentFont,
                        foregroundColor: fontSettings.foregroundColor,
                        backgroundColor: fontSettings.backgroundColor,
                        focusToken: 0,
                        isActive: isActive,
                        onInput: handleInput,
                        onPaste: handlePaste,
                        onInterrupt: {
                            pscalRuntimeRequestSigint()
                        },
                        onSuspend: {
                            pscalRuntimeRequestSigtstp()
                        },
                        onResize: { cols, rows in
                            applyTerminalSize(columns: cols, rows: rows, source: "webview")
                            startRuntimeIfNeeded()
                        },
                        onReady: { controller in
                            tabInitLog("TerminalView ready runtime=\(runtime.runtimeId) controller=\(controller.instanceId) loaded=\(controller.isLoaded)")
                            controller.onLoaded = {
                                tabInitLog("TerminalView onLoaded runtime=\(runtime.runtimeId) controller=\(controller.instanceId)")
                                tabInitLog("TerminalView loaded runtime=\(runtime.runtimeId) controller=\(controller.instanceId)")
                                runtime.markHtermLoaded()
                                DispatchQueue.main.async {
                                    if !localHtermLoaded {
                                        localHtermLoaded = true
                                    }
                                }
                            }
                            if controller.isLoaded {
                                runtime.markHtermLoaded()
                                DispatchQueue.main.async {
                                    if !localHtermLoaded {
                                        localHtermLoaded = true
                                        kickPromptIfNeeded()
                                        schedulePromptKicks()
                                    }
                                }
                            }
                            runtime.attachHtermController(controller)
                        },
                        onDetach: { controller in
                            tabInitLog("TerminalView detach runtime=\(runtime.runtimeId) controller=\(controller.instanceId)")
                            runtime.detachHtermController(controller)
                        },
                        onLoadStateChange: { loaded in
                            if !loaded {
                                tabInitLog("TerminalView loadState runtime=\(runtime.runtimeId) loaded=false")
                                runtime.markHtermUnloaded()
                            } else {
                                tabInitLog("TerminalView loadState runtime=\(runtime.runtimeId) loaded=true")
                            }
                            DispatchQueue.main.async {
                                if localHtermLoaded != loaded {
                                    localHtermLoaded = loaded
                                    if loaded {
                                        kickPromptIfNeeded()
                                        schedulePromptKicks()
                                    }
                                    if loaded && isActive && !initialFocusRequested {
                                        initialFocusRequested = true
                                        requestInputFocus()
                                    }
                                }
                            }
                        }
                    )
                    .zIndex(htermReady ? 1 : 0)
                }

                if showEditorSnapshot {
                    TerminalRendererView(
                        text: runtime.screenText,
                        cursor: runtime.cursorInfo,
                        backgroundColor: fontSettings.backgroundColor,
                        foregroundColor: fontSettings.foregroundColor,
                        isEditorMode: editorActive,
                        isEditorWindowVisible: editorVisible,
                        editorRenderToken: editorTokenState,
                        font: currentFont,
                        fontPointSize: fontSettings.pointSize,
                        editorSnapshot: runtime.editorBridge.snapshot(),
                        onPaste: handlePaste,
                        onInput: runtime.send,
                        mouseMode: runtime.mouseMode,
                        mouseEncoding: runtime.mouseEncoding,
                        onGeometryChange: { cols, rows in
                            applyTerminalSize(columns: cols, rows: rows, source: "renderer")
                            startRuntimeIfNeeded()
                        }
                    )
                }

                if showBlank {
                    Color(fontSettings.backgroundColor)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Color(fontSettings.backgroundColor))

            if let status = exitStatus {
                Divider()
                Text("Process exited with status \(status)")
                    .font(.footnote)
                    .foregroundColor(.secondary)
                    .padding(.vertical, 8)
                    .frame(maxWidth: .infinity)
                    .background(Color(.secondarySystemBackground))
            }
        }
        // Align to top so that if we have extra vertical space (e.g. fractional line height)
        // it appears at the bottom.
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .padding(.top, Self.topPadding)
        .background(Color(fontSettings.backgroundColor))
        .contentShape(Rectangle())
        .onTapGesture {
            requestInputFocus()
        }
        .onChange(of: isActive) { active in
            if active {
                ensureHtermControllerIfNeeded()
                updateTerminalGeometry()
                startRuntimeIfNeeded()
                if !initialFocusRequested {
                    initialFocusRequested = true
                    requestInputFocus()
                }
            } else {
                detachHtermControllerIfNeeded()
            }
        }
        .onAppear {
            tabInitLog("TerminalView appear runtime=\(runtime.runtimeId) active=\(isActive)")
            ensureHtermControllerIfNeeded()
            updateTerminalGeometry()
            startRuntimeIfNeeded()
            if isActive && !initialFocusRequested {
                initialFocusRequested = true
                requestInputFocus()
            }
        }
        .onChange(of: availableSize) { _ in
            updateTerminalGeometry()
        }
        .onChange(of: fontSettings.pointSize) { _ in
            hasMeasuredGeometry = false
            updateTerminalGeometry()
        }
        .onChange(of: exitStatus) { _ in
            updateTerminalGeometry()
        }
        .onReceive(runtime.$exitStatus.removeDuplicates()) { value in
            exitStatus = value
        }
        .onReceive(runtime.$editorRenderToken.removeDuplicates()) { token in
            editorRenderTokenState = token
        }
        .onDisappear {
            detachHtermControllerIfNeeded()
        }
    }

    // MARK: Input handlers

    private func handleInput(_ text: String) {
        runtime.send(text)
    }

    private func handlePaste(_ text: String) {
        runtime.sendPasted(text)
    }

    // MARK: Geometry

    private func updateTerminalGeometry() {
        guard isActive else { return }
        let font = fontSettings.currentFont
        let showingStatus = exitStatus != nil

        // Calculate usable height for the terminal text area.
        // availableSize.height is the total space from GeometryReader (including padding area).
        var usableHeight = availableSize.height

        // Subtract top padding (applied to VStack)
        usableHeight -= Self.topPadding

        // If status is shown, we subtract the status overlay height.
        // The Divider is part of this space, effectively.
        if showingStatus {
            usableHeight -= TerminalGeometryCalculator.statusOverlayHeight
        }

        let usableSize = CGSize(width: availableSize.width, height: usableHeight)

        let grid = TerminalGeometryCalculator.calculateCapacity(
            for: usableSize,
            font: font
        )

        let metrics = TerminalGeometryCalculator.TerminalGeometryMetrics(
            columns: max(1, grid.columns),
            rows: max(1, grid.rows)
        )
        let context = "\(availableSize.width)x\(availableSize.height) status=\(showingStatus ? 1 : 0)"
        applyTerminalSize(columns: metrics.columns, rows: metrics.rows, source: context, statusShown: showingStatus)
    }

    private func startRuntimeIfNeeded() {
        guard hasMeasuredGeometry, !runtimeStarted else {
            tabInitLog("TerminalView start skip runtime=\(runtime.runtimeId) measured=\(hasMeasuredGeometry) started=\(runtimeStarted)")
            return
        }
        runtimeStarted = true
        tabInitLog("TerminalView start runtime=\(runtime.runtimeId)")
        runtime.start()
        schedulePromptKicks(reset: true)
        if isActive && !initialFocusRequested {
            initialFocusRequested = true
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
                requestInputFocus()
            }
        }
    }

    private func ensureHtermControllerIfNeeded() {
        guard isActive else { return }
        if htermController == nil {
            htermController = runtime.ensureHtermController()
        }
    }

    private func detachHtermControllerIfNeeded() {
        guard let controller = htermController else { return }
        runtime.detachHtermController(controller)
        htermController = nil
        localHtermLoaded = false
    }

    private func applyTerminalSize(columns: Int, rows: Int, source: String? = nil, statusShown: Bool? = nil) {
        guard columns > 0, rows > 0 else { return }
        if !hasMeasuredGeometry {
            hasMeasuredGeometry = true
        }
        let metrics = TerminalGeometryCalculator.TerminalGeometryMetrics(columns: columns, rows: rows)
        guard lastReportedMetrics != metrics else {
            return
        }
        lastReportedMetrics = metrics
        if let source {
            RuntimeLogger.runtime.append("[TerminalView] Geometry update (\(source)) -> \(metrics.columns) cols x \(metrics.rows) rows")
        }
        if let statusShown {
            tabInitLog("TerminalView geometry runtime=\(runtime.runtimeId) cols=\(metrics.columns) rows=\(metrics.rows) status=\(statusShown)")
        } else {
            tabInitLog("TerminalView geometry runtime=\(runtime.runtimeId) cols=\(metrics.columns) rows=\(metrics.rows)")
        }
        runtime.updateTerminalSize(columns: metrics.columns, rows: metrics.rows)
    }

    private func requestInputFocus() {
        NotificationCenter.default.post(name: .terminalInputFocusRequested, object: nil)
    }

    private func kickPromptIfNeeded() {
        guard promptKickRemaining > 0 else { return }
        promptKickRemaining -= 1
        runtime.send(" ")
        runtime.send("\u{7F}")
    }

    private func schedulePromptKicks(reset: Bool = false) {
        if reset {
            promptKickRemaining = 3
        }
        kickPromptIfNeeded()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
            kickPromptIfNeeded()
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
            kickPromptIfNeeded()
        }
    }
}
