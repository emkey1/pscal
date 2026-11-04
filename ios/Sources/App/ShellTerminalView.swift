import SwiftUI
import UIKit

struct ShellTerminalView: View {
    @ObservedObject var session: ShellRuntimeSession
    /// On screen, so hterm stays attached and the grid follows the view's size.
    let isVisible: Bool
    /// Holds the keyboard. At most one terminal does.
    let isActive: Bool
    let topPadding: CGFloat
    let showsOverlay: Bool
    let tabId: UInt64
    @ObservedObject private var fontSettings: TerminalTabAppearanceSettings
    @State private var focusAnchor: Int = 0
    @State private var showingSettings = false

    init(
        session: ShellRuntimeSession,
        isVisible: Bool = true,
        isActive: Bool,
        topPadding: CGFloat = 32.0,
        showsOverlay: Bool = true,
        tabId: UInt64,
        appearanceSettings: TerminalTabAppearanceSettings
    ) {
        self.session = session
        self.isVisible = isVisible
        self.isActive = isActive
        self.topPadding = topPadding
        self.showsOverlay = showsOverlay
        self.tabId = tabId
        _fontSettings = ObservedObject(wrappedValue: appearanceSettings)
    }

    var body: some View {
        if TerminalDebugFlags.printChanges {
            let _ = Self._printChanges()
            traceViewChanges("ShellTerminalView body")
        }
        return GeometryReader { proxy in
            ZStack(alignment: .bottomTrailing) {
                ShellTerminalContentView(
                    availableSize: proxy.size,
                    fontSettings: fontSettings,
                    session: session,
                    focusAnchor: $focusAnchor,
                    isVisible: isVisible,
                    isActive: isActive,
                    topPadding: topPadding
                )
                .id(session.sessionId)
                .frame(width: proxy.size.width, height: proxy.size.height)

                if showsOverlay {
                    Button {
                        showingSettings = true
                    } label: {
                        Image(systemName: "textformat.size")
                            .font(.system(size: 16, weight: .semibold))
                            .padding(8)
                            .background(.ultraThinMaterial, in: Circle())
                    }
                    .padding(.bottom, 16)
                    .padding(.trailing, 10)
                    .accessibilityLabel("Terminal Settings")
                    .accessibilityHint("Configure appearance and behavior")
                }
            }
        }
        .sheet(isPresented: $showingSettings) {
            TerminalSettingsView(appearanceSettings: fontSettings, tabId: tabId)
        }
        .background(Color(fontSettings.backgroundColor))
        .onChange(of: isActive) { active in
            if active {
                focusAnchor &+= 1
            }
        }
    }
}

private struct ShellTerminalContentView: View {
    let availableSize: CGSize
    @ObservedObject var fontSettings: TerminalTabAppearanceSettings
    @ObservedObject var session: ShellRuntimeSession
    @Binding var focusAnchor: Int
    let isVisible: Bool
    let isActive: Bool
    let topPadding: CGFloat

    @State private var hasMeasuredGeometry: Bool = false
    @State private var lastReportedMetrics: TerminalGeometryCalculator.TerminalGeometryMetrics?

    var body: some View {
        if TerminalDebugFlags.printChanges {
            let _ = Self._printChanges()
            traceViewChanges("ShellTerminalContentView body")
        }
        let currentFont = fontSettings.currentFont

        return VStack(spacing: 0) {
            HtermTerminalView(
                controller: session.htermController,
                font: currentFont,
                foregroundColor: fontSettings.foregroundColor,
                backgroundColor: fontSettings.backgroundColor,
                focusToken: focusAnchor,
                isVisible: isVisible,
                isActive: isActive,
                onInput: handleInput,
                onPaste: handlePaste,
                onInterrupt: {
                    session.sendInterrupt()
                },
                onSuspend: {
                    session.sendSuspend()
                },
                onResize: { cols, rows in
                    tabInitLog("ShellTerminalView resize session=\(session.sessionId) cols=\(cols) rows=\(rows)")
                    sshResizeLog("[micro-resize] shell view onResize session=\(session.sessionId) cols=\(cols) rows=\(rows)")
                    // Always accept hterm's measured grid. The first SwiftUI
                    // geometry pass can be stale; hterm's startup/resize query
                    // reflects the actual rendered terminal size.
                    applyTerminalSize(columns: cols, rows: rows, source: "hterm")
                },
                onReady: { controller in
                    tabInitLog("ShellTerminalView ready session=\(session.sessionId) controller=\(controller.instanceId)")
                    session.attachHtermController(controller)
                },
                onDetach: { controller in
                    tabInitLog("ShellTerminalView detach session=\(session.sessionId) controller=\(controller.instanceId)")
                    session.detachHtermController(controller)
                }
            )
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Color(fontSettings.backgroundColor))

            if let status = session.exitStatus {
                Divider()
                Text("\(session.sessionDisplayName) exited with status \(status)")
                    .font(.footnote)
                    .foregroundColor(.secondary)
                    .padding(.vertical, 8)
                    .frame(maxWidth: .infinity)
                    .background(Color(.secondarySystemBackground))
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .padding(.top, topPadding)
        .background(Color(fontSettings.backgroundColor))
        .contentShape(Rectangle())
        .onTapGesture {
            requestInputFocus()
        }
        .onAppear {
            tabInitLog("ShellTerminalView appear session=\(session.sessionId) visible=\(isVisible) active=\(isActive)")
            session.setViewVisible(isVisible)
            updateTerminalGeometry()
            let started = session.start()
            tabInitLog("ShellTerminalView start session=\(session.sessionId) started=\(started)")
            if isActive {
                DispatchQueue.main.async {
                    requestInputFocus()
                }
            }
        }
        .onChange(of: availableSize) { _ in
            updateTerminalGeometry()
        }
        .onChange(of: fontSettings.pointSize) { _ in
            hasMeasuredGeometry = false
            updateTerminalGeometry()
        }
        .onChange(of: isVisible) { visible in
            session.setViewVisible(visible)
            if visible {
                updateTerminalGeometry()
            }
        }
        .onDisappear {
            session.setViewVisible(false)
        }
    }

    private func handleInput(_ text: String) {
        session.send(text)
    }

    private func handlePaste(_ text: String) {
        session.sendPasted(text)
    }

    private func updateTerminalGeometry() {
        guard isVisible else {
            return
        }
        // This is only an estimate, for a session starting before the page has measured. After
        // that the page's grid is the terminal's size, and an estimate that differs by a column
        // would be a resize of nothing.
        guard !session.htermController.hasMeasuredGrid else {
            return
        }
        let font = fontSettings.currentFont
        let usableHeight = availableSize.height - topPadding
        // A view with no size yet isn't a terminal of 10 by 4. The session keeps the size it has.
        guard availableSize.width > 0, usableHeight > 0 else {
            return
        }
        let usableSize = CGSize(width: availableSize.width, height: usableHeight)
        let grid = TerminalGeometryCalculator.calculateCapacity(
            for: usableSize,
            font: font
        )
        let columns = max(10, grid.columns)
        let rows = max(4, grid.rows)
        applyTerminalSize(columns: columns, rows: rows, source: "swiftui")
    }

    private func requestInputFocus() {
        focusAnchor &+= 1
    }

    private func applyTerminalSize(columns: Int, rows: Int, source: String) {
        if !hasMeasuredGeometry {
            hasMeasuredGeometry = true
        }
        let metrics = TerminalGeometryCalculator.TerminalGeometryMetrics(columns: columns, rows: rows)
        guard lastReportedMetrics != metrics else { return }
        lastReportedMetrics = metrics
        sshResizeLog("[micro-resize] shell view applyTerminalSize source=\(source) session=\(session.sessionId) cols=\(metrics.columns) rows=\(metrics.rows)")
        session.updateTerminalSize(columns: metrics.columns, rows: metrics.rows)
    }
}
