import UIKit
import SwiftUI

/// Responsible for calculating the terminal grid dimensions (rows and columns)
/// based on the available visual size, font metrics, and safe area insets.
enum TerminalGeometryCalculator {

    // MARK: - Constants

    /// Horizontal padding inside the terminal (left and right).
    static let horizontalPadding: CGFloat = 0

    /// Vertical space reserved for the "process exited" status bar.
    static let statusOverlayHeight: CGFloat = 28.0

    // MARK: - Types

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
        let usableWidth: CGFloat
        let usableHeight: CGFloat
    }

    // MARK: - Calculations

    /// Round up to the nearest device pixel to align with the screen grid.
    static func pixelCeil(_ value: CGFloat) -> CGFloat {
        let scale = UIScreen.main.scale
        return ceil(value * scale) / scale
    }

    /// Computes character metrics for the given font.
    /// Uses 'W' as the reference character for width.
    static func characterMetrics(for font: UIFont) -> CharacterMetrics {
        // We use "W" as the sample character as it is typically the widest in variable width fonts,
        // and matches the standard width in monospaced fonts.
        let sample = "W" as NSString
        let sampleWidth = sample.size(withAttributes: [.font: font]).width

        // Round up to ensure we don't underestimate width (prevents wrapping).
        // Ensure at least 1px width.
        let charWidth = max(1.0, pixelCeil(sampleWidth))
        let lineHeight = pixelCeil(font.lineHeight)

        return CharacterMetrics(width: charWidth, lineHeight: lineHeight)
    }

    /// Calculates the grid capacity given the bounds and constraints.
    ///
    /// - Parameters:
    ///   - size: The total size of the view (from GeometryReader).
    ///   - font: The font used for rendering.
    ///   - safeAreaInsets: The safe area insets to respect (subtract from size).
    ///   - topPadding: Additional top padding.
    ///   - horizontalPadding: Additional horizontal padding (applied to both sides).
    ///   - showingStatus: Whether the status overlay is visible.
    static func calculateGrid(
        for size: CGSize,
        font: UIFont,
        safeAreaInsets: UIEdgeInsets,
        topPadding: CGFloat,
        horizontalPadding: CGFloat = TerminalGeometryCalculator.horizontalPadding,
        showingStatus: Bool
    ) -> TerminalGridCapacity {

        // Vertical calculation
        var availableHeight = size.height

        // 1. Subtract safe areas (top/bottom)
        availableHeight -= (safeAreaInsets.top + safeAreaInsets.bottom)

        // 2. Subtract custom UI padding
        availableHeight -= topPadding

        // 3. Subtract status overlay if present
        if showingStatus {
            availableHeight -= statusOverlayHeight
        }

        // Clamp to 0
        availableHeight = max(0, availableHeight)

        // Horizontal calculation
        var availableWidth = size.width

        // 1. Subtract safe areas (left/right)
        availableWidth -= (safeAreaInsets.left + safeAreaInsets.right)

        // 2. Subtract custom UI padding (left/right)
        availableWidth -= (horizontalPadding * 2)

        // Clamp to 0
        availableWidth = max(0, availableWidth)

        // Metrics
        let metrics = characterMetrics(for: font)
        let lineHeight = metrics.lineHeight
        let charWidth = metrics.width

        // Grid dimensions
        // We enforce a minimum of 1x1 to prevent crashes, but usually terminal logic handles small sizes.
        // We cap at 2000 to prevent crazy allocations if size reports huge.
        let columns = max(1, min(Int(availableWidth / charWidth), 2000))
        let rows = max(1, min(Int(availableHeight / lineHeight), 2000))

        return TerminalGridCapacity(
            rows: rows,
            columns: columns,
            width: availableWidth,   // This is technically "usable width"
            height: availableHeight, // "usable height"
            usableWidth: availableWidth,
            usableHeight: availableHeight
        )
    }

    /// Helper to get metrics directly.
    static func metrics(
        for size: CGSize,
        safeAreaInsets: EdgeInsets, // SwiftUI EdgeInsets
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
            showingStatus: showingStatus
        )

        // If the grid is too small to be useful (0 rows or columns), return nil to fallback
        if grid.rows < 1 || grid.columns < 1 {
             return nil
        }

        return TerminalGeometryMetrics(columns: grid.columns, rows: grid.rows)
    }

    static func fallbackMetrics(showingStatus: Bool, font: UIFont) -> TerminalGeometryMetrics {
        return TerminalGeometryMetrics(columns: 80, rows: showingStatus ? 23 : 24)
    }
}
