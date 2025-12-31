import SwiftUI
import UIKit
import Combine
import os

// MARK: - Main Terminal View

struct TerminalView: View {
    let showsOverlay: Bool

    @ObservedObject private var fontSettings = TerminalFontSettings.shared
    @State private var showingSettings = false
    @State private var focusAnchor: Int = 0

    init(showsOverlay: Bool = true) {
        self.showsOverlay = showsOverlay
    }

    var body: some View {
        ZStack(alignment: .bottomTrailing) {
            GeometryReader { proxy in
                TerminalContentView(
                    availableSize: proxy.size,
                    fontSettings: fontSettings,
                    focusAnchor: $focusAnchor
                )
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
                PscalRuntimeBootstrap.shared.resetTerminalState()
                requestInputFocus()
                PscalRuntimeBootstrap.shared.send(" ")
                PscalRuntimeBootstrap.shared.send("\u{08}") // backspace
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
                PscalRuntimeBootstrap.shared.forceExshRestart()
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
        NotificationCenter.default.post(name: .terminalInputFocusRequested, object: nil)
    }
}

// MARK: - Terminal Content View

struct TerminalContentView: View {
    static let topPadding: CGFloat = 32.0

    let availableSize: CGSize

    @ObservedObject private var fontSettings: TerminalFontSettings
    @ObservedObject private var runtime = PscalRuntimeBootstrap.shared
    @Binding private var focusAnchor: Int

    @State private var lastLoggedMetrics: TerminalGeometryCalculator.TerminalGeometryMetrics?
    @State private var hasMeasuredGeometry: Bool = false
    @State private var runtimeStarted: Bool = false

    init(
        availableSize: CGSize,
        fontSettings: TerminalFontSettings,
        focusAnchor: Binding<Int>
    ) {
        self.availableSize = availableSize
        _fontSettings = ObservedObject(wrappedValue: fontSettings)
        _focusAnchor = focusAnchor
    }

    var body: some View {
        let elvisToken = runtime.elvisRenderToken
        let elvisActive = runtime.isElvisModeActive()
        let elvisVisible = EditorWindowManager.shared.isVisible
        let currentFont = fontSettings.currentFont

        let externalWindowEnabled = EditorWindowManager.externalWindowEnabled
        let showElvisSnapshot = elvisActive && (!externalWindowEnabled || !elvisVisible)
        let showBlank = elvisActive && elvisVisible && externalWindowEnabled

        return VStack(spacing: 0) {
            ZStack {
                HtermTerminalView(
                    font: currentFont,
                    foregroundColor: fontSettings.foregroundColor,
                    backgroundColor: fontSettings.backgroundColor,
                    onInput: handleInput,
                    onResize: { cols, rows in
                        runtime.updateTerminalSize(columns: cols, rows: rows)
                        hasMeasuredGeometry = true
                        startRuntimeIfNeeded()
                    },
                    onReady: { controller in
                        runtime.attachHtermController(controller)
                    }
                )

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
                        elvisSnapshot: nil,
                        onPaste: handlePaste,
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
        .overlay(alignment: .bottomLeading) {
            if !EditorWindowManager.shared.isVisible {
                TerminalInputBridge(
                    focusAnchor: $focusAnchor,
                    onInput: handleInput,
                    onPaste: handlePaste
                )
                .frame(width: 1, height: 1)
                .allowsHitTesting(false)
            }
        }
        .onAppear {
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

        if lastLoggedMetrics != metrics {
            RuntimeLogger.runtime.append("[TerminalView] Geometry update: \(availableSize.width)x\(availableSize.height) -> \(metrics.columns) cols x \(metrics.rows) rows")
            lastLoggedMetrics = metrics
        }

        runtime.updateTerminalSize(columns: metrics.columns, rows: metrics.rows)
    }

    private func startRuntimeIfNeeded() {
        guard hasMeasuredGeometry, !runtimeStarted else { return }
        runtimeStarted = true
        runtime.start()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            requestInputFocus()
        }
    }

    private func requestInputFocus() {
        focusAnchor &+= 1
        NotificationCenter.default.post(name: .terminalInputFocusRequested, object: nil)
    }
}
