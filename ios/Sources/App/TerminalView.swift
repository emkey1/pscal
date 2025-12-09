import SwiftUI
import UIKit
import Combine

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
                    fontSettings: fontSettings
                )
                .frame(width: proxy.size.width, height: proxy.size.height)
            }
        }
        .background(Color(fontSettings.backgroundColor))
    }
}

// MARK: - Terminal Content View

struct TerminalContentView: View {
    let availableSize: CGSize

    @ObservedObject private var fontSettings: TerminalFontSettings
    @ObservedObject private var runtime = PscalRuntimeBootstrap.shared

    init(
        availableSize: CGSize,
        fontSettings: TerminalFontSettings
    ) {
        self.availableSize = availableSize
        _fontSettings = ObservedObject(wrappedValue: fontSettings)
    }

    var body: some View {
        let externalWindowEnabled = EditorWindowManager.externalWindowEnabled
        let elvisVisible = EditorWindowManager.shared.isVisible
        let shouldBlank = runtime.isElvisModeActive() && elvisVisible && externalWindowEnabled

        Group {
            if shouldBlank {
                Color(fontSettings.backgroundColor)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                TerminalTextView(
                    text: runtime.screenText.string,
                    cursor: runtime.cursorInfo,
                    fontSize: $fontSettings.pointSize,
                    terminalColor: $fontSettings.foregroundColor,
                    backgroundColor: $fontSettings.backgroundColor,
                    onInput: { input in
                        runtime.send(input)
                    },
                    onSettingsChanged: { newSize, newColor in
                        fontSettings.updatePointSize(newSize)
                        fontSettings.updateForegroundColor(newColor)
                    }
                )
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color(fontSettings.backgroundColor))
        .onAppear {
            updateTerminalGeometry()
            runtime.start()
        }
        .onChange(of: availableSize) { _ in
            updateTerminalGeometry()
        }
        .onChange(of: fontSettings.pointSize) { _ in
            updateTerminalGeometry()
        }
    }

    // MARK: Geometry

    private func updateTerminalGeometry() {
        let font = fontSettings.currentFont

        // Use full available size
        let usableSize = availableSize

        let grid = TerminalGeometryCalculator.calculateCapacity(
            for: usableSize,
            font: font
        )

        runtime.updateTerminalSize(columns: grid.columns, rows: grid.rows)
    }
}

