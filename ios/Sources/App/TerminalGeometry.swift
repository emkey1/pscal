import UIKit
import SwiftUI

/// Responsible for calculating the terminal grid dimensions (rows and columns)
/// based on the available visual size and font metrics.
enum TerminalGeometryCalculator {

    // MARK: - Constants

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

    /// Calculates the grid capacity given the exact usable size.
    /// The caller is responsible for subtracting any padding, safe areas, or overlays
    /// before calling this function.
    ///
    /// - Parameters:
    ///   - size: The usable size for the terminal text area.
    ///   - font: The font used for rendering.
    static func calculateCapacity(
        for size: CGSize,
        font: UIFont
    ) -> TerminalGridCapacity {

        let availableWidth = max(0, size.width)
        let availableHeight = max(0, size.height)

        // Metrics
        let metrics = characterMetrics(for: font)
        let lineHeight = metrics.lineHeight
        let charWidth = metrics.width

        // Grid dimensions
        // We enforce a minimum of 1x1 to prevent crashes.
        let columns = max(1, Int(availableWidth / charWidth))
        let rows = max(1, Int(availableHeight / lineHeight))

        return TerminalGridCapacity(
            rows: rows,
            columns: columns,
            width: availableWidth,
            height: availableHeight
        )
    }

    static func fallbackMetrics(showingStatus: Bool, font: UIFont) -> TerminalGeometryMetrics {
        return TerminalGeometryMetrics(columns: 80, rows: showingStatus ? 23 : 24)
    }
}
