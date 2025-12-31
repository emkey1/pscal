import SwiftUI
import UIKit

struct SshTerminalView: View {
    @ObservedObject var session: SshRuntimeSession
    @ObservedObject private var fontSettings = TerminalFontSettings.shared
    @State private var focusAnchor: Int = 0

    var body: some View {
        GeometryReader { proxy in
            SshTerminalContentView(
                availableSize: proxy.size,
                fontSettings: fontSettings,
                session: session,
                focusAnchor: $focusAnchor
            )
            .frame(width: proxy.size.width, height: proxy.size.height)
        }
        .background(Color(fontSettings.backgroundColor))
    }
}

private struct SshTerminalContentView: View {
    private enum Layout {
        static let topPadding: CGFloat = 32.0
    }

    let availableSize: CGSize
    @ObservedObject var fontSettings: TerminalFontSettings
    @ObservedObject var session: SshRuntimeSession
    @Binding var focusAnchor: Int

    @State private var hasMeasuredGeometry: Bool = false

    var body: some View {
        let currentFont = fontSettings.currentFont

        return VStack(spacing: 0) {
            HtermTerminalView(
                font: currentFont,
                foregroundColor: fontSettings.foregroundColor,
                backgroundColor: fontSettings.backgroundColor,
                onInput: handleInput,
                onResize: { cols, rows in
                    hasMeasuredGeometry = true
                    session.updateTerminalSize(columns: cols, rows: rows)
                },
                onReady: { controller in
                    session.attachHtermController(controller)
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
        .overlay(alignment: .bottomLeading) {
            TerminalInputBridge(
                focusAnchor: $focusAnchor,
                onInput: handleInput,
                onPaste: handlePaste,
                onInterrupt: { session.send("\u{03}") },
                onSuspend: { session.send("\u{1A}") }
            )
            .frame(width: 1, height: 1)
            .allowsHitTesting(false)
        }
        .onAppear {
            if !hasMeasuredGeometry {
                updateTerminalGeometry()
            }
            session.start()
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
        NotificationCenter.default.post(name: .terminalInputFocusRequested, object: nil)
    }
}
