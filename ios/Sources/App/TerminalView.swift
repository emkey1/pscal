import SwiftUI
import UIKit
import Combine
import os

// MARK: - Main Terminal View

struct TerminalView: View {
    let showsOverlay: Bool
    let isActive: Bool
    @ObservedObject var runtime: PscalRuntimeBootstrap

    @ObservedObject private var fontSettings = TerminalFontSettings.shared
    @State private var showingSettings = false
    @State private var focusAnchor: Int = 0

    init(showsOverlay: Bool = true, isActive: Bool = true, runtime: PscalRuntimeBootstrap) {
        self.showsOverlay = showsOverlay
        self.isActive = isActive
        _runtime = ObservedObject(wrappedValue: runtime)
    }

    var body: some View {
        ZStack(alignment: .bottomTrailing) {
            GeometryReader { proxy in
                TerminalContentView(
                    availableSize: proxy.size,
                    fontSettings: fontSettings,
                    focusAnchor: $focusAnchor,
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
            Button {
                runtime.resetTerminalState()
                requestInputFocus()
                runtime.send(" ")
                runtime.send("\u{08}") // backspace
            } label: {
                Text("RT")
                    .font(.system(size: 14, weight: .semibold, design: .rounded))
                    .padding(9)
                    .background(.ultraThinMaterial, in: Circle())
            }
            .accessibilityLabel("Reset Terminal")

            Button {
                showingSettings = true
            } label: {
                Image(systemName: "textformat.size")
                    .font(.system(size: 16, weight: .semibold))
                    .padding(8)
                    .background(.ultraThinMaterial, in: Circle())
            }
            .accessibilityLabel("Adjust Font Size")
            
            Button {
                runtime.forceExshRestart()
                requestInputFocus()
            } label: {
                Image(systemName: "arrow.triangle.2.circlepath")
                    .font(.system(size: 16, weight: .semibold))
                    .padding(8)
                    .background(.ultraThinMaterial, in: Circle())
            }
            .accessibilityLabel("Restart exsh shell")
            
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
        focusAnchor &+= 1
    }
}

// MARK: - Terminal Content View

struct TerminalContentView: View {
    static let topPadding: CGFloat = 32.0

    let availableSize: CGSize

    @ObservedObject private var fontSettings: TerminalFontSettings
    @ObservedObject var runtime: PscalRuntimeBootstrap
    @Binding private var focusAnchor: Int
    private let isActive: Bool

    @State private var lastLoggedMetrics: TerminalGeometryCalculator.TerminalGeometryMetrics?
    @State private var hasMeasuredGeometry: Bool = false
    @State private var runtimeStarted: Bool = false
    @State private var localHtermLoaded: Bool = false

    init(
        availableSize: CGSize,
        fontSettings: TerminalFontSettings,
        focusAnchor: Binding<Int>,
        isActive: Bool,
        runtime: PscalRuntimeBootstrap
    ) {
        self.availableSize = availableSize
        _fontSettings = ObservedObject(wrappedValue: fontSettings)
        _focusAnchor = focusAnchor
        self.isActive = isActive
        _runtime = ObservedObject(wrappedValue: runtime)
    }

    var body: some View {
        let elvisToken = runtime.elvisRenderToken
        let elvisActive = runtime.isElvisModeActive()
        let elvisVisible = EditorWindowManager.shared.isVisible
        let currentFont = fontSettings.currentFont
        let controller = runtime.htermControllerIfCreated() ?? (isActive ? runtime.ensureHtermController() : nil)
        let htermReady = (controller != nil) && (runtime.htermReady || localHtermLoaded)

        let externalWindowEnabled = EditorWindowManager.externalWindowEnabled
        let showElvisSnapshot = elvisActive && (!externalWindowEnabled || !elvisVisible)
        let showBlank = elvisActive && elvisVisible && externalWindowEnabled
        return VStack(spacing: 0) {
            ZStack {
                if let controller {
                    HtermTerminalView(
                        controller: controller,
                        font: currentFont,
                        foregroundColor: fontSettings.foregroundColor,
                        backgroundColor: fontSettings.backgroundColor,
                        focusToken: focusAnchor,
                        isActive: isActive,
                        onInput: handleInput,
                        onPaste: handlePaste,
                        onInterrupt: { runtime.send("\u{03}") },
                        onSuspend: { runtime.send("\u{1A}") },
                        onResize: { cols, rows in
                            runtime.updateTerminalSize(columns: cols, rows: rows)
                            hasMeasuredGeometry = true
                            startRuntimeIfNeeded()
                        },
                        onReady: { controller in
                            tabInitLog("TerminalView ready runtime=\(runtime.runtimeId) controller=\(controller.instanceId) loaded=\(controller.isLoaded)")
                            controller.onLoaded = {
                                tabInitLog("TerminalView onLoaded runtime=\(runtime.runtimeId) controller=\(controller.instanceId)")
                                tabInitLog("TerminalView loaded runtime=\(runtime.runtimeId) controller=\(controller.instanceId)")
                                runtime.markHtermLoaded()
                                DispatchQueue.main.async {
                                    localHtermLoaded = true
                                }
                            }
                            if controller.isLoaded {
                                runtime.markHtermLoaded()
                                DispatchQueue.main.async {
                                    localHtermLoaded = true
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
                                localHtermLoaded = loaded
                            }
                        }
                    )
                    .zIndex(htermReady ? 1 : 0)
                }

                if showElvisSnapshot {
                    TerminalRendererView(
                        text: runtime.screenText,
                        cursor: runtime.cursorInfo,
                        backgroundColor: fontSettings.backgroundColor,
                        foregroundColor: fontSettings.foregroundColor,
                        isElvisMode: elvisActive,
                        isElvisWindowVisible: elvisVisible,
                        elvisRenderToken: elvisToken,
                        font: currentFont,
                        fontPointSize: fontSettings.pointSize,
                        elvisSnapshot: runtime.editorBridge.snapshot(),
                        onPaste: handlePaste,
                        onInput: runtime.send,
                        mouseMode: runtime.mouseMode,
                        mouseEncoding: runtime.mouseEncoding,
                        onGeometryChange: { cols, rows in
                            hasMeasuredGeometry = true
                            runtime.updateTerminalSize(columns: cols, rows: rows)
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

            if let status = runtime.exitStatus {
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
                requestInputFocus()
            }
        }
        .onAppear {
            tabInitLog("TerminalView appear runtime=\(runtime.runtimeId) active=\(isActive)")
            if !hasMeasuredGeometry {
                updateTerminalGeometry()
            }
            startRuntimeIfNeeded()
        }
        .onChange(of: availableSize) { _ in
            if !hasMeasuredGeometry { updateTerminalGeometry() }
        }
        .onChange(of: fontSettings.pointSize) { _ in
            hasMeasuredGeometry = false
            updateTerminalGeometry()
        }
        .onChange(of: runtime.exitStatus) { _ in
            if !hasMeasuredGeometry { updateTerminalGeometry() }
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
        let font = fontSettings.currentFont
        let showingStatus = runtime.exitStatus != nil

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
            columns: grid.columns,
            rows: grid.rows
        )
        hasMeasuredGeometry = true

        if lastLoggedMetrics != metrics {
            RuntimeLogger.runtime.append("[TerminalView] Geometry update: \(availableSize.width)x\(availableSize.height) -> \(metrics.columns) cols x \(metrics.rows) rows")
            lastLoggedMetrics = metrics
        }
        tabInitLog("TerminalView geometry runtime=\(runtime.runtimeId) cols=\(metrics.columns) rows=\(metrics.rows) status=\(showingStatus)")

        runtime.updateTerminalSize(columns: metrics.columns, rows: metrics.rows)
    }

    private func startRuntimeIfNeeded() {
        guard hasMeasuredGeometry, !runtimeStarted else {
            tabInitLog("TerminalView start skip runtime=\(runtime.runtimeId) measured=\(hasMeasuredGeometry) started=\(runtimeStarted)")
            return
        }
        runtimeStarted = true
        tabInitLog("TerminalView start runtime=\(runtime.runtimeId)")
        runtime.start()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            requestInputFocus()
        }
    }

    private func requestInputFocus() {
        focusAnchor &+= 1
    }
}
