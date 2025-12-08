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
                    safeAreaInsets: proxy.safeAreaInsets,
                    fontSettings: fontSettings,
                    focusAnchor: $focusAnchor
                )
                .frame(width: proxy.size.width, height: proxy.size.height)
            }

            if showsOverlay {
                overlayButtons
            }
        }
        .sheet(isPresented: $showingSettings) {
            TerminalSettingsView()
        }
        .background(Color(fontSettings.backgroundColor))
    }

    // MARK: Overlay

    @ViewBuilder
    private var overlayButtons: some View {
        HStack(alignment: .center, spacing: 10) {
            Button {
                PscalRuntimeBootstrap.shared.resetTerminalState()
                focusAnchor &+= 1
            } label: {
                Text("R")
                    .font(.system(size: 16, weight: .semibold, design: .rounded))
                    .padding(11)
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
        }
        .padding(.bottom, 16)
        .padding(.trailing, 10)
    }
}

// MARK: - Terminal Content View

struct TerminalContentView: View {
    private static let topPadding: CGFloat = 32.0

    let availableSize: CGSize
    let safeAreaInsets: EdgeInsets

    @ObservedObject private var fontSettings: TerminalFontSettings
    @ObservedObject private var runtime = PscalRuntimeBootstrap.shared
    @Binding private var focusAnchor: Int

    @State private var lastLoggedMetrics: TerminalGeometryCalculator.TerminalGeometryMetrics?

    init(
        availableSize: CGSize,
        safeAreaInsets: EdgeInsets,
        fontSettings: TerminalFontSettings,
        focusAnchor: Binding<Int>
    ) {
        self.availableSize = availableSize
        self.safeAreaInsets = safeAreaInsets
        _fontSettings = ObservedObject(wrappedValue: fontSettings)
        _focusAnchor = focusAnchor
    }

    var body: some View {
        let elvisToken = runtime.elvisRenderToken
        let elvisActive = runtime.isElvisModeActive()
        let elvisVisible = EditorWindowManager.shared.isVisible
        let currentFont = fontSettings.currentFont

        return VStack(spacing: 0) {
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
                mouseEncoding: runtime.mouseEncoding
            )
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Color(fontSettings.backgroundColor))

            Divider()

            if let status = runtime.exitStatus {
                Text("Process exited with status \(status)")
                    .font(.footnote)
                    .foregroundColor(.secondary)
                    .padding(.vertical, 8)
                    .frame(maxWidth: .infinity)
                    .background(Color(.secondarySystemBackground))
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .padding(.top, Self.topPadding)
        .background(Color(fontSettings.backgroundColor))
        .contentShape(Rectangle())
        .onTapGesture {
            focusAnchor &+= 1
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
            updateTerminalGeometry()
            runtime.start()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                focusAnchor &+= 1
            }
        }
        .onChange(of: availableSize) { _ in
            updateTerminalGeometry()
        }
        .onChange(of: fontSettings.pointSize) { _ in
            updateTerminalGeometry()
        }
        .onChange(of: runtime.exitStatus) { _ in
            updateTerminalGeometry()
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
        let showingStatus = runtime.exitStatus != nil
        let font = fontSettings.currentFont

        // Size from GeometryReader is already keyboard-safe (UIKit host enforces it).
        let effectiveHeight = max(1, availableSize.height)
        let sizeForMetrics = CGSize(width: availableSize.width, height: effectiveHeight)

        guard let metrics = TerminalGeometryCalculator.metrics(
            for: sizeForMetrics,
            safeAreaInsets: safeAreaInsets,
            topPadding: Self.topPadding,
            showingStatus: showingStatus,
            font: font
        ) ?? TerminalGeometryCalculator.fallbackMetrics(
            showingStatus: showingStatus,
            font: font
        ) else {
            return
        }

        #if DEBUG
        if lastLoggedMetrics != metrics {
            let grid = TerminalGeometryCalculator.calculateGrid(
                for: sizeForMetrics,
                font: font,
                safeAreaInsets: UIEdgeInsets(
                    top: safeAreaInsets.top,
                    left: safeAreaInsets.leading,
                    bottom: safeAreaInsets.bottom,
                    right: safeAreaInsets.trailing
                ),
                topPadding: Self.topPadding,
                horizontalPadding: TerminalGeometryCalculator.horizontalPadding,
                showingStatus: showingStatus
            )
            let char = TerminalGeometryCalculator.characterMetrics(for: font)
            terminalViewLog(
                String(
                    format: "[TerminalView] available %.1fx%.1f usable %.1fx%.1f -> rows=%d cols=%d char=%.1fx%.1f",
                    availableSize.width,
                    availableSize.height,
                    grid.width,
                    grid.height,
                    metrics.rows,
                    metrics.columns,
                    char.width,
                    char.lineHeight
                )
            )
            lastLoggedMetrics = metrics
        }
        #endif

        runtime.updateTerminalSize(columns: metrics.columns, rows: metrics.rows)
    }
}

// MARK: - Preview

struct TerminalView_Previews: PreviewProvider {
    static var previews: some View {
        TerminalView()
            .previewDevice("iPad Pro (11-inch) (4th generation)")
    }
}
