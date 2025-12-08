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

    /// Compute character width and line height for the given font.
    /// We use "W" as the sample character as it is typically the widest in variable width fonts,
    /// and matches the standard width in monospaced fonts.
    /// We round up the width to the nearest pixel to ensure we never underestimate the space required,
    /// which prevents unexpected line wrapping.
    static func characterMetrics(for font: UIFont) -> CharacterMetrics {
        let sample = "W" as NSString
        let sampleWidth = sample.size(withAttributes: [.font: font]).width
        let charWidth = max(1.0, pixelCeil(sampleWidth))
        let lineHeight = pixelCeil(font.lineHeight)
        return CharacterMetrics(width: charWidth, lineHeight: lineHeight)
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

        // Subtract vertical safe areas for robustness.
        // If the view is within the safe area, these insets are 0.
        // If the view extends into unsafe areas (e.g. full screen), we subtract them
        // to ensure we don't calculate rows that would be obscured.
        availableHeight -= safeAreaInsets.top
        availableHeight -= safeAreaInsets.bottom
        availableHeight = max(0, availableHeight)

        let metrics = characterMetrics(for: font)
        let lineHeight = metrics.lineHeight

        var availableWidth = size.width
        availableWidth -= (safeAreaInsets.left + safeAreaInsets.right)
        availableWidth -= (horizontalPadding * 2)

        // Subtract horizontal safe areas
        availableWidth -= safeAreaInsets.left
        availableWidth -= safeAreaInsets.right
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
