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
        // Align to top so that if we have extra vertical space (e.g. fractional line height)
        // it appears at the bottom.
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

        // GeometryReader reports the size of the view.
        // If the view is respecting safe area (default), this size is the safe area size.
        // In that case, proxy.safeAreaInsets will be zero (relative to the view).
        // If the view was ignoring safe area, proxy.size would be full screen and insets non-zero.
        // In either case, passing both to calculateGrid handles it correctly (size - insets).

        let effectiveHeight = max(1, availableSize.height)
        let sizeForMetrics = CGSize(width: availableSize.width, height: effectiveHeight)

        guard let metrics = TerminalGeometryCalculator.metrics(
            for: sizeForMetrics,
            safeAreaInsets: safeAreaInsets,
            topPadding: Self.topPadding,
            showingStatus: showingStatus,
            font: font
        ) else {
            // If calculation failed (e.g. 0 size), use fallback.
            let fallback = TerminalGeometryCalculator.fallbackMetrics(showingStatus: showingStatus, font: font)
            runtime.updateTerminalSize(columns: fallback.columns, rows: fallback.rows)
            return
        }

        #if DEBUG
        if lastLoggedMetrics != metrics {
            print("[TerminalView] Geometry update: \(availableSize.width)x\(availableSize.height) -> \(metrics.columns) cols x \(metrics.rows) rows")
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
