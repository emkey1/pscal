import SwiftUI
import UIKit

struct ShellTerminalView: View {
    @ObservedObject var session: ShellRuntimeSession
    let isActive: Bool
    @ObservedObject private var fontSettings = TerminalFontSettings.shared
    @State private var focusAnchor: Int = 0

    var body: some View {
        GeometryReader { proxy in
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

    var body: some View {
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
                    tabInitLog("ShellTerminalView resize session=\(session.sessionId) cols=\(cols) rows=\(rows)")
                    hasMeasuredGeometry = true
                    session.updateTerminalSize(columns: cols, rows: rows)
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
            if !hasMeasuredGeometry {
                updateTerminalGeometry()
            }
            let started = session.start()
            tabInitLog("ShellTerminalView start session=\(session.sessionId) started=\(started)")
            DispatchQueue.main.async {
                requestInputFocus()
            }
        }
        .onChange(of: availableSize) { _ in
            if !hasMeasuredGeometry { updateTerminalGeometry() }
        }
        .onChange(of: fontSettings.pointSize) { _ in
            hasMeasuredGeometry = false
            updateTerminalGeometry()
        }
    }

    private func handleInput(_ text: String) {
        session.send(text)
    }

    private func handlePaste(_ text: String) {
        session.sendPasted(text)
    }

    private func updateTerminalGeometry() {
        let font = fontSettings.currentFont
        let usableHeight = availableSize.height - Layout.topPadding
        let usableSize = CGSize(width: availableSize.width, height: usableHeight)
        let grid = TerminalGeometryCalculator.calculateCapacity(
            for: usableSize,
            font: font
        )
        let columns = max(10, grid.columns)
        let rows = max(4, grid.rows)
        session.updateTerminalSize(columns: columns, rows: rows)
    }

    private func requestInputFocus() {
        focusAnchor &+= 1
    }
}
