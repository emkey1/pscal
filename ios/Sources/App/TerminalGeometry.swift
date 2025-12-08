import UIKit
import SwiftUI

enum TerminalGeometryCalculator {
    /// Horizontal padding inside the terminal when calculating columns.
    static let horizontalPadding: CGFloat = 0

    /// Extra vertical space reserved for the "process exited" status bar.
    static let statusOverlayHeight: CGFloat = 28.0

    struct CharacterMetrics {
        let width: CGFloat
        let lineHeight: CGFloat
    }

    struct TerminalGeometryMetrics: Equatable {
        let columns: Int
        let rows: Int
    }

    struct TerminalGridCapacity {
        let rows: Int
        let columns: Int
        let width: CGFloat
        let height: CGFloat
    }

    /// Round up to the nearest device pixel.
    static func pixelCeil(_ value: CGFloat) -> CGFloat {
        let scale = UIScreen.main.scale
        return ceil(value * scale) / scale
    }

    /// Compute average character width and line height for the given font.
    static func characterMetrics(for font: UIFont) -> CharacterMetrics {
        let sample = "MMMMMMMMMM" as NSString
        let sampleWidth = sample.size(withAttributes: [.font: font]).width
        let avgCharWidth = max(1.0, sampleWidth / 10.0)
        let lineHeight = pixelCeil(font.lineHeight)
        return CharacterMetrics(width: avgCharWidth, lineHeight: lineHeight)
    }

    /// Core grid calculation based on a concrete view size.
    static func calculateGrid(
        for size: CGSize,
        font: UIFont,
        safeAreaInsets: UIEdgeInsets,
        topPadding: CGFloat,
        horizontalPadding: CGFloat,
        showingStatus: Bool
    ) -> TerminalGridCapacity {
        // UIKit already constrains the host view to sit above the keyboard via
        // keyboardLayoutGuide, so `size.height` is the actual usable height.
        var availableHeight = size.height
        availableHeight -= (safeAreaInsets.top + safeAreaInsets.bottom)
        availableHeight -= topPadding
        if showingStatus {
            availableHeight -= statusOverlayHeight
        }
        availableHeight = max(0, availableHeight)

        let metrics = characterMetrics(for: font)
        let lineHeight = metrics.lineHeight

        var availableWidth = size.width
        availableWidth -= (safeAreaInsets.left + safeAreaInsets.right)
        availableWidth -= (horizontalPadding * 2)
        availableWidth = max(0, availableWidth)

        let charWidth = metrics.width

        let maxColumns = max(10, min(Int(availableWidth / charWidth), 2000))
        let maxRows    = max(4,  min(Int(availableHeight / lineHeight), 2000))

        return TerminalGridCapacity(
            rows: maxRows,
            columns: maxColumns,
            width: availableWidth,
            height: availableHeight
        )
    }

    /// Main entry used by `updateTerminalGeometry()`.
    static func metrics(
        for size: CGSize,
        safeAreaInsets: EdgeInsets,
        topPadding: CGFloat,
        showingStatus: Bool,
        font: UIFont
    ) -> TerminalGeometryMetrics? {
        let uiInsets = UIEdgeInsets(
            top: safeAreaInsets.top,
            left: safeAreaInsets.leading,
            bottom: safeAreaInsets.bottom,
            right: safeAreaInsets.trailing
        )

        let grid = calculateGrid(
            for: size,
            font: font,
            safeAreaInsets: uiInsets,
            topPadding: topPadding,
            horizontalPadding: horizontalPadding,
            showingStatus: showingStatus
        )

        guard grid.rows > 0, grid.columns > 0 else {
            return nil
        }

        return TerminalGeometryMetrics(columns: grid.columns, rows: grid.rows)
    }

    /// Fallback when we don't have a usable size yet.
    static func fallbackMetrics(
        showingStatus: Bool,
        font: UIFont
    ) -> TerminalGeometryMetrics? {
        // Simple conservative default.
        return TerminalGeometryMetrics(columns: 80, rows: showingStatus ? 23 : 24)
    }
}
