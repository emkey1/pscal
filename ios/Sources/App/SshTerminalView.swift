import SwiftUI
import UIKit

struct SshTerminalView: View {
    @ObservedObject var session: SshRuntimeSession
    let isActive: Bool
    @ObservedObject private var fontSettings: TerminalTabAppearanceSettings
    @State private var focusAnchor: Int = 0
    @State private var showingSettings = false

    init(
        session: SshRuntimeSession,
        isActive: Bool,
        appearanceSettings: TerminalTabAppearanceSettings
    ) {
        self.session = session
        self.isActive = isActive
        _fontSettings = ObservedObject(wrappedValue: appearanceSettings)
    }

    var body: some View {
        if TerminalDebugFlags.printChanges {
            let _ = Self._printChanges()
            traceViewChanges("SshTerminalView body")
        }
        return GeometryReader { proxy in
            ZStack(alignment: .bottomTrailing) {
                SshTerminalContentView(
                    availableSize: proxy.size,
                    fontSettings: fontSettings,
                    session: session,
                    focusAnchor: $focusAnchor,
                    isActive: isActive
                )
                .id(session.sessionId)
                .frame(width: proxy.size.width, height: proxy.size.height)

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
                .accessibilityLabel("Adjust Font Size")
            }
        }
        .sheet(isPresented: $showingSettings) {
            TerminalSettingsView(appearanceSettings: fontSettings)
        }
        .background(Color(fontSettings.backgroundColor))
        .onChange(of: isActive) { active in
            if active {
                focusAnchor &+= 1
            }
        }
    }
}

private struct SshTerminalContentView: View {
    private enum Layout {
        static let topPadding: CGFloat = 32.0
    }

    let availableSize: CGSize
    @ObservedObject var fontSettings: TerminalTabAppearanceSettings
    @ObservedObject var session: SshRuntimeSession
    @Binding var focusAnchor: Int
    let isActive: Bool

    @State private var hasMeasuredGeometry: Bool = false
    @State private var lastReportedMetrics: TerminalGeometryCalculator.TerminalGeometryMetrics?

    var body: some View {
        if TerminalDebugFlags.printChanges {
            let _ = Self._printChanges()
            traceViewChanges("SshTerminalContentView body")
        }
        let currentFont = fontSettings.currentFont

        return VStack(spacing: 0) {
            HtermTerminalView(
                controller: session.htermController,
                font: currentFont,
                foregroundColor: fontSettings.foregroundColor,
                backgroundColor: fontSettings.backgroundColor,
                focusToken: focusAnchor,
                isActive: isActive,
                onInput: handleInput,
                onPaste: handlePaste,
                onInterrupt: { session.send("\u{03}") },
                onSuspend: { session.send("\u{1A}") },
                onResize: { cols, rows in
                    tabInitLog("SshTerminalView resize session=\(session.sessionId) cols=\(cols) rows=\(rows)")
                    applyTerminalSize(columns: cols, rows: rows)
                },
                onReady: { controller in
                    tabInitLog("SshTerminalView ready session=\(session.sessionId) controller=\(controller.instanceId)")
                    session.attachHtermController(controller)
                },
                onDetach: { controller in
                    tabInitLog("SshTerminalView detach session=\(session.sessionId) controller=\(controller.instanceId)")
                    session.detachHtermController(controller)
                }
            )
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Color(fontSettings.backgroundColor))

            if let status = session.exitStatus {
                Divider()
                Text("SSH exited with status \(status)")
                    .font(.footnote)
                    .foregroundColor(.secondary)
                    .padding(.vertical, 8)
                    .frame(maxWidth: .infinity)
                    .background(Color(.secondarySystemBackground))
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .padding(.top, Layout.topPadding)
        .background(Color(fontSettings.backgroundColor))
        .contentShape(Rectangle())
        .onTapGesture {
            requestInputFocus()
        }
        .onAppear {
            tabInitLog("SshTerminalView appear session=\(session.sessionId) active=\(isActive)")
            updateTerminalGeometry()
            let started = session.start()
            tabInitLog("SshTerminalView start session=\(session.sessionId) started=\(started)")
            DispatchQueue.main.async {
                requestInputFocus()
            }
        }
        .onChange(of: availableSize) { _ in
            updateTerminalGeometry()
        }
        .onChange(of: fontSettings.pointSize) { _ in
            sshResizeLog("[ssh-resize] view font change session=\(session.sessionId) pt=\(fontSettings.pointSize)")
            hasMeasuredGeometry = false
            updateTerminalGeometry()
        }
        .onChange(of: isActive) { active in
            if active {
                updateTerminalGeometry()
            }
        }
    }

    private func handleInput(_ text: String) {
        session.send(text)
    }

    private func handlePaste(_ text: String) {
        session.sendPasted(text)
    }

    private func updateTerminalGeometry() {
        guard isActive else {
            sshResizeLog("[ssh-resize] view geometry skipped session=\(session.sessionId) inactive")
            return
        }
        let font = fontSettings.currentFont
        let usableHeight = availableSize.height - Layout.topPadding
        let usableSize = CGSize(width: availableSize.width, height: usableHeight)
        let grid = TerminalGeometryCalculator.calculateCapacity(
            for: usableSize,
            font: font
        )
        let columns = max(10, grid.columns)
        let rows = max(4, grid.rows)
        sshResizeLog("[ssh-resize] view geometry session=\(session.sessionId) size=\(Int(usableSize.width))x\(Int(usableSize.height)) font=\(font.pointSize) grid=\(columns)x\(rows)")
        applyTerminalSize(columns: columns, rows: rows)
    }

    private func requestInputFocus() {
        focusAnchor &+= 1
    }

    private func applyTerminalSize(columns: Int, rows: Int) {
        if !hasMeasuredGeometry {
            hasMeasuredGeometry = true
        }
        let metrics = TerminalGeometryCalculator.TerminalGeometryMetrics(columns: columns, rows: rows)
        guard lastReportedMetrics != metrics else {
            sshResizeLog("[ssh-resize] view apply skipped session=\(session.sessionId) unchanged=\(metrics.columns)x\(metrics.rows)")
            return
        }
        lastReportedMetrics = metrics
        sshResizeLog("[ssh-resize] view apply session=\(session.sessionId) cols=\(metrics.columns) rows=\(metrics.rows)")
        session.updateTerminalSize(columns: metrics.columns, rows: metrics.rows)
    }
}
