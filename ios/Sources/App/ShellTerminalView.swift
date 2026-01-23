import SwiftUI
import UIKit

struct ShellTerminalView: View {
    @ObservedObject var session: ShellRuntimeSession
    let isActive: Bool
    @ObservedObject private var fontSettings = TerminalFontSettings.shared
    @State private var focusAnchor: Int = 0

    var body: some View {
        if TerminalDebugFlags.printChanges {
            let _ = Self._printChanges()
            traceViewChanges("ShellTerminalView body")
        }
        return GeometryReader { proxy in
            ShellTerminalContentView(
                availableSize: proxy.size,
                fontSettings: fontSettings,
                session: session,
                focusAnchor: $focusAnchor,
                isActive: isActive
            )
            .id(session.sessionId)
            .frame(width: proxy.size.width, height: proxy.size.height)
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
    private enum Layout {
        static let topPadding: CGFloat = 32.0
    }

    let availableSize: CGSize
    @ObservedObject var fontSettings: TerminalFontSettings
    @ObservedObject var session: ShellRuntimeSession
    @Binding var focusAnchor: Int
    let isActive: Bool

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
                    tabInitLog("ShellTerminalView resize session=\(session.sessionId) cols=\(cols) rows=\(rows)")
                    applyTerminalSize(columns: cols, rows: rows)
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
                Text("Shell exited with status \(status)")
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
            tabInitLog("ShellTerminalView appear session=\(session.sessionId) active=\(isActive)")
            updateTerminalGeometry()
            let started = session.start()
            tabInitLog("ShellTerminalView start session=\(session.sessionId) started=\(started)")
            DispatchQueue.main.async {
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
        guard lastReportedMetrics != metrics else { return }
        lastReportedMetrics = metrics
        session.updateTerminalSize(columns: metrics.columns, rows: metrics.rows)
    }
}
