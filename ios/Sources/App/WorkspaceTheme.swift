// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceViewController.m at
// 0f75fee1c) and licensed to PSCAL under the Apache License 2.0.

import UIKit

extension Notification.Name {
    static let workspaceThemeDidChange = Notification.Name("WorkspaceThemeDidChange")
    static let workspaceGaugeStyleDidChange = Notification.Name("WorkspaceGaugeStyleDidChange")
}

/// A palette as it's saved: each editable colour as red, green and blue from 0 to 255, in
/// plist types.
typealias WorkspacePalette = [String: [String: Double]]

/// One palette worked out into the colours the interface draws with.
struct WorkspaceThemeColors {
    let backgroundTop: UIColor
    let backgroundBottom: UIColor
    let card: UIColor
    let cardAlt: UIColor
    let primary: UIColor
    let secondary: UIColor
    let accent: UIColor
    let accentAlt: UIColor
    let focusRing: UIColor
    let stroke: UIColor
}

struct WorkspaceThemeRecord {
    let identifier: String
    let title: String
    let palette: WorkspacePalette
    let builtIn: Bool
}

/// Workspace's own colours, separate from each terminal's. The current theme, its density
/// and the gauge style apply to every window on the desktop.
@MainActor
enum WorkspaceTheme {
    static let aurora = "aurora"
    static let solstice = "solstice"
    static let graphite = "graphite"
    /// What a Workspace uses when nobody has chosen. Graphite because Aurora's light
    /// background doesn't carry enough contrast under the Launcher's text, and that is the
    /// first surface a new user meets.
    static let defaultIdentifier = graphite
    static let editableColorKeys = ["backgroundTop", "backgroundBottom", "card", "primary", "secondary", "accent", "accentAlt"]

    private static let themeKey = "com.pscal.workspace.theme"
    private static let customThemesKey = "com.pscal.workspace.customThemes"
    private static let densityKey = "com.pscal.workspace.density"
    private static let gaugeStyleKey = "com.pscal.workspace.gaugeStyle"

    private static var cachedColors: WorkspaceThemeColors?

    static var current: WorkspaceThemeColors {
        if let cachedColors { return cachedColors }
        let colors = colors(from: palette(forIdentifier: currentIdentifier))
        cachedColors = colors
        return colors
    }

    /// The accent as drawn on the desktop rather than on a card: a window's title bar and
    /// border, and the menu button.
    static var accent: UIColor { current.focusRing }

    static var desktopBackground: UIColor { current.backgroundBottom }

    private static func themeDidChange() {
        cachedColors = nil
        NotificationCenter.default.post(name: .workspaceThemeDidChange, object: nil)
    }

    // MARK: - Library

    private static func color(_ red: Double, _ green: Double, _ blue: Double) -> [String: Double] {
        ["red": red, "green": green, "blue": blue]
    }

    static let builtInChoices: [(identifier: String, title: String)] = [
        // The default first, which is the order a picker should offer them in.
        (graphite, "Graphite"),
        (aurora, "Aurora"),
        (solstice, "Solstice"),
    ]

    private static func builtInPalette(_ identifier: String) -> WorkspacePalette {
        switch identifier {
        case solstice:
            return [
                "backgroundTop": color(255, 240, 218),
                "backgroundBottom": color(255, 174, 111),
                "card": color(255, 250, 242),
                "primary": color(71, 39, 24),
                "secondary": color(108, 73, 49),
                "accent": color(170, 70, 30),
                "accentAlt": color(128, 87, 16),
            ]
        case graphite:
            return [
                "backgroundTop": color(15, 20, 32),
                "backgroundBottom": color(26, 34, 50),
                "card": color(44, 55, 76),
                "primary": color(240, 244, 255),
                "secondary": color(182, 194, 218),
                "accent": color(112, 232, 204),
                "accentAlt": color(138, 176, 255),
            ]
        default:
            return [
                "backgroundTop": color(10, 26, 58),
                "backgroundBottom": color(22, 74, 104),
                "card": color(240, 251, 255),
                "primary": color(13, 38, 62),
                "secondary": color(52, 80, 110),
                "accent": color(0, 118, 156),
                "accentAlt": color(20, 128, 120),
            ]
        }
    }

    private static func customRecords() -> [[String: Any]] {
        UserDefaults.standard.array(forKey: customThemesKey) as? [[String: Any]] ?? []
    }

    private static func record(from dictionary: [String: Any]) -> WorkspaceThemeRecord? {
        guard let identifier = dictionary["identifier"] as? String,
              let title = dictionary["title"] as? String,
              let palette = dictionary["palette"] as? WorkspacePalette else { return nil }
        return WorkspaceThemeRecord(identifier: identifier, title: title, palette: palette, builtIn: false)
    }

    static func record(forIdentifier identifier: String) -> WorkspaceThemeRecord? {
        if let choice = builtInChoices.first(where: { $0.identifier == identifier }) {
            return WorkspaceThemeRecord(identifier: identifier, title: choice.title,
                                        palette: builtInPalette(identifier), builtIn: true)
        }
        return customRecords().compactMap(record(from:)).first { $0.identifier == identifier }
    }

    /// The built-in themes, then the saved ones.
    static var choices: [WorkspaceThemeRecord] {
        let builtIn = builtInChoices.map {
            WorkspaceThemeRecord(identifier: $0.identifier, title: $0.title,
                                 palette: builtInPalette($0.identifier), builtIn: true)
        }
        return builtIn + customRecords().compactMap(record(from:))
    }

    static func isBuiltIn(_ identifier: String) -> Bool {
        builtInChoices.contains { $0.identifier == identifier }
    }

    static var currentIdentifier: String {
        guard let identifier = UserDefaults.standard.string(forKey: themeKey),
              record(forIdentifier: identifier) != nil else {
            return defaultIdentifier
        }
        return identifier
    }

    static var currentTitle: String {
        record(forIdentifier: currentIdentifier)?.title ?? "Graphite"
    }

    static func setCurrentIdentifier(_ identifier: String) {
        guard record(forIdentifier: identifier) != nil, identifier != currentIdentifier else { return }
        UserDefaults.standard.set(identifier, forKey: themeKey)
        themeDidChange()
    }

    static func palette(forIdentifier identifier: String) -> WorkspacePalette {
        record(forIdentifier: identifier)?.palette ?? builtInPalette(defaultIdentifier)
    }

    static func makeCustomIdentifier() -> String {
        "custom-\(UUID().uuidString.lowercased())"
    }

    static func saveCustomTheme(identifier: String, title: String, palette: WorkspacePalette) {
        guard !identifier.isEmpty, !title.isEmpty else { return }
        var records = customRecords()
        let record: [String: Any] = ["identifier": identifier, "title": title, "palette": palette, "builtIn": false]
        if let index = records.firstIndex(where: { $0["identifier"] as? String == identifier }) {
            records[index] = record
        } else {
            records.append(record)
        }
        UserDefaults.standard.set(records, forKey: customThemesKey)
        themeDidChange()
    }

    static func deleteCustomTheme(identifier: String) {
        guard !identifier.isEmpty else { return }
        var records = customRecords()
        let before = records.count
        records.removeAll { $0["identifier"] as? String == identifier }
        guard records.count != before else { return }
        UserDefaults.standard.set(records, forKey: customThemesKey)
        if UserDefaults.standard.string(forKey: themeKey) == identifier {
            UserDefaults.standard.set(defaultIdentifier, forKey: themeKey)
        }
        themeDidChange()
    }

    // MARK: - Colours from a palette

    static func uiColor(_ descriptor: [String: Double]?) -> UIColor {
        guard let descriptor else { return .black }
        return UIColor(red: (descriptor["red"] ?? 0) / 255,
                       green: (descriptor["green"] ?? 0) / 255,
                       blue: (descriptor["blue"] ?? 0) / 255,
                       alpha: 1)
    }

    private static func rgba(_ color: UIColor) -> (red: CGFloat, green: CGFloat, blue: CGFloat, alpha: CGFloat)? {
        var red: CGFloat = 0, green: CGFloat = 0, blue: CGFloat = 0, alpha: CGFloat = 0
        if color.getRed(&red, green: &green, blue: &blue, alpha: &alpha) {
            return (red, green, blue, alpha)
        }
        var white: CGFloat = 0
        if color.getWhite(&white, alpha: &alpha) {
            return (white, white, white, alpha)
        }
        return nil
    }

    private static func luminance(_ color: UIColor) -> CGFloat {
        guard let parts = rgba(color) else { return 0 }
        func linear(_ value: CGFloat) -> CGFloat {
            value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4)
        }
        return 0.2126 * linear(parts.red) + 0.7152 * linear(parts.green) + 0.0722 * linear(parts.blue)
    }

    static func contrastRatio(_ first: UIColor, _ second: UIColor) -> CGFloat {
        let a = luminance(first), b = luminance(second)
        return (max(a, b) + 0.05) / (min(a, b) + 0.05)
    }

    private static func blend(_ foreground: UIColor, over background: UIColor) -> UIColor {
        guard let fore = rgba(foreground) else { return foreground }
        guard let back = rgba(background) else { return foreground }
        let alpha = fore.alpha + back.alpha * (1 - fore.alpha)
        guard alpha > 0.0001 else { return .clear }
        func mix(_ f: CGFloat, _ b: CGFloat) -> CGFloat {
            (f * fore.alpha + b * back.alpha * (1 - fore.alpha)) / alpha
        }
        return UIColor(red: mix(fore.red, back.red), green: mix(fore.green, back.green), blue: mix(fore.blue, back.blue), alpha: 1)
    }

    private static func average(_ first: UIColor, _ second: UIColor) -> UIColor {
        guard let a = rgba(first) else { return second }
        guard let b = rgba(second) else { return first }
        return UIColor(red: (a.red + b.red) / 2, green: (a.green + b.green) / 2, blue: (a.blue + b.blue) / 2, alpha: 1)
    }

    /// Walks `color` towards `target` until it clears the contrast bar against `background`.
    private static func walk(_ color: UIColor, towards target: UIColor, background: UIColor, minimumContrast: CGFloat) -> UIColor {
        guard let from = rgba(color), let to = rgba(target) else { return target }
        for step in 1...24 {
            let amount = CGFloat(step) / 24
            let candidate = UIColor(red: from.red + (to.red - from.red) * amount,
                                    green: from.green + (to.green - from.green) * amount,
                                    blue: from.blue + (to.blue - from.blue) * amount,
                                    alpha: 1)
            if contrastRatio(candidate, background) >= minimumContrast {
                return candidate
            }
        }
        return target
    }

    static func adjusted(_ color: UIColor, against background: UIColor, minimumContrast: CGFloat) -> UIColor {
        if contrastRatio(color, background) >= minimumContrast { return color }
        let target: UIColor = contrastRatio(.black, background) >= contrastRatio(.white, background) ? .black : .white
        return walk(color, towards: target, background: background, minimumContrast: minimumContrast)
    }

    /// The focus ring is drawn on the desktop, not on a card, so it must contrast with the
    /// background: the card-tuned accent vanishes on a background of the same hue. 3:1 is
    /// the WCAG minimum for a component's boundary, and an accent that clears it is kept.
    private static func focusRingColor(accent: UIColor, background: UIColor) -> UIColor {
        let minimumContrast: CGFloat = 3
        if contrastRatio(accent, background) >= minimumContrast { return accent }
        var target: UIColor = luminance(background) < 0.45 ? .white : .black
        if contrastRatio(target, background) < minimumContrast {
            target = contrastRatio(.black, background) >= contrastRatio(.white, background) ? .black : .white
        }
        return walk(accent, towards: target, background: background, minimumContrast: minimumContrast)
    }

    /// The colour to draw on a filled surface, such as a focused title bar or a selected
    /// row. Measured against the fill rather than assumed from the palette: white is 5.16:1
    /// on Aurora's teal and 1.49:1 on Graphite's mint, and the palette editor can build a
    /// fourth palette that breaks any assumption. The light pole wins where it clears the
    /// bar; the dark pole is near-black so it reads as ink and not as a hole.
    static func onSurface(_ fill: UIColor, minimumContrast: CGFloat = 4.5) -> UIColor {
        let light = UIColor.white
        let dark = UIColor(red: 0.05, green: 0.06, blue: 0.09, alpha: 1)
        let lightRatio = contrastRatio(light, fill)
        if lightRatio >= minimumContrast { return light }
        let darkRatio = contrastRatio(dark, fill)
        if darkRatio >= minimumContrast { return dark }
        return lightRatio >= darkRatio ? light : dark
    }

    static func ink(on fill: UIColor) -> UIColor {
        onSurface(fill)
    }

    /// A softer companion to `onSurface`, for secondary text on the same fill, but only as
    /// soft as the fill allows. A dimmer label that can't be read is not a hierarchy.
    static func mutedOnSurface(_ ink: UIColor, fill: UIColor, minimumContrast: CGFloat = 4.5) -> UIColor {
        let muted = blend(ink.withAlphaComponent(0.82), over: fill)
        return contrastRatio(muted, fill) >= minimumContrast ? muted : ink
    }

    /// What a translucent wash paints over `base`. That composite is what text on it has to
    /// contrast with.
    static func wash(_ color: UIColor, alpha: CGFloat, over base: UIColor) -> UIColor {
        blend(color.withAlphaComponent(alpha), over: base)
    }

    static func colors(from palette: WorkspacePalette) -> WorkspaceThemeColors {
        let backgroundTop = uiColor(palette["backgroundTop"])
        let backgroundBottom = uiColor(palette["backgroundBottom"])
        let cardBase = uiColor(palette["card"])
        let backdrop = average(backgroundTop, backgroundBottom)
        let card = blend(cardBase.withAlphaComponent(0.97), over: backdrop)
        let cardAlt = blend(cardBase.withAlphaComponent(0.94), over: backdrop)
        let surface = average(card, cardAlt)
        let accentBase = uiColor(palette["accent"])
        // Anchored to the background, where it's actually drawn, not to the card.
        let focusRing = focusRingColor(accent: accentBase, background: backgroundBottom)
        return WorkspaceThemeColors(
            backgroundTop: backgroundTop,
            backgroundBottom: backgroundBottom,
            card: card,
            cardAlt: cardAlt,
            primary: adjusted(uiColor(palette["primary"]), against: surface, minimumContrast: 7),
            secondary: adjusted(uiColor(palette["secondary"]), against: surface, minimumContrast: 4.9),
            accent: adjusted(accentBase, against: surface, minimumContrast: 4.8),
            accentAlt: adjusted(uiColor(palette["accentAlt"]), against: surface, minimumContrast: 4.8),
            focusRing: focusRing,
            stroke: focusRing.withAlphaComponent(0.28))
    }

    // MARK: - Density

    static var density: CGFloat {
        get {
            guard let value = UserDefaults.standard.object(forKey: densityKey) as? NSNumber else { return 0 }
            return max(0, min(1, CGFloat(value.doubleValue)))
        }
        set {
            UserDefaults.standard.set(Double(max(0, min(1, newValue))), forKey: densityKey)
            themeDidChange()
        }
    }

    static var densityTitle: String {
        switch density {
        case ..<0.18: return "Ultra Compact"
        case ..<0.42: return "Compact"
        case ..<0.72: return "Balanced"
        default: return "Comfortable"
        }
    }

    static func densityValue(_ compact: CGFloat, _ roomy: CGFloat) -> CGFloat {
        compact + (roomy - compact) * density
    }

    static func fontSize(_ style: UIFont.TextStyle) -> CGFloat {
        var size: CGFloat
        switch style {
        case .caption2: size = densityValue(8, 11)
        case .caption1: size = densityValue(9, 12)
        case .footnote: size = densityValue(10, 13)
        case .subheadline: size = densityValue(11, 15)
        case .headline: size = densityValue(12, 17)
        case .body: size = densityValue(12, 16)
        case .title3: size = densityValue(15, 21)
        case .title2: size = densityValue(17, 25)
        case .largeTitle: size = densityValue(24, 34)
        default: size = densityValue(12, 16)
        }
        if WorkspaceMetrics.usesPhoneLayout {
            size *= 0.92
        }
        return size
    }

    /// The monospaced detail text views, before any per-window text scale.
    static var textViewFontSize: CGFloat {
        let phone = WorkspaceMetrics.usesPhoneLayout
        return densityValue(phone ? 8.5 : 9.5, phone ? 11 : 12)
    }

    // MARK: - Gauges

    /// Workspace-wide gauge style for the status applets. Rings unless bars were chosen.
    static var usesRingGauges: Bool {
        get { UserDefaults.standard.string(forKey: gaugeStyleKey) != "bar" }
        set {
            UserDefaults.standard.set(newValue ? "ring" : "bar", forKey: gaugeStyleKey)
            NotificationCenter.default.post(name: .workspaceGaugeStyleDidChange, object: nil)
        }
    }

    // MARK: - Artwork

    /// A theme's picture, from a thumbnail to a whole wallpaper.
    static func artworkImage(identifier: String, palette: WorkspacePalette, size: CGSize, wallpaper: Bool) -> UIImage? {
        guard size.width >= 1, size.height >= 1 else { return nil }
        let theme = colors(from: palette)
        let format = UIGraphicsImageRendererFormat.default()
        format.opaque = true
        let renderer = UIGraphicsImageRenderer(size: size, format: format)
        return renderer.image { rendererContext in
            let context = rendererContext.cgContext
            let width = size.width, height = size.height

            let card = theme.card.withAlphaComponent(wallpaper ? 0.26 : 0.18)
            let accent = theme.accent.withAlphaComponent(wallpaper ? 0.46 : 0.28)
            let accentAlt = theme.accentAlt.withAlphaComponent(wallpaper ? 0.34 : 0.24)
            let secondary = theme.secondary.withAlphaComponent(wallpaper ? 0.22 : 0.18)

            let colorSpace = CGColorSpaceCreateDeviceRGB()
            if let gradient = CGGradient(colorsSpace: colorSpace,
                                         colors: [theme.backgroundTop.cgColor, theme.backgroundBottom.cgColor] as CFArray,
                                         locations: nil) {
                context.drawLinearGradient(gradient,
                                           start: CGPoint(x: width / 2, y: 0),
                                           end: CGPoint(x: width / 2, y: height),
                                           options: [])
            }

            accent.setFill()
            UIBezierPath(ovalIn: CGRect(x: width * 0.58, y: -height * 0.1, width: width * 0.44, height: height * 0.46)).fill()
            accentAlt.setFill()
            UIBezierPath(ovalIn: CGRect(x: -width * 0.16, y: height * 0.52, width: width * 0.46, height: height * 0.38)).fill()

            switch identifier {
            case solstice:
                theme.card.withAlphaComponent(wallpaper ? 0.82 : 0.9).setFill()
                UIBezierPath(ovalIn: CGRect(x: width * 0.62, y: height * 0.12, width: width * 0.18, height: width * 0.18)).fill()
                let bands: [(CGFloat, UIColor)] = [
                    (0.58, theme.accent.withAlphaComponent(0.34)),
                    (0.68, theme.accentAlt.withAlphaComponent(0.28)),
                    (0.79, theme.card.withAlphaComponent(0.22)),
                ]
                for (origin, color) in bands {
                    let y = origin * height
                    let band = UIBezierPath()
                    band.move(to: CGPoint(x: -width * 0.1, y: y))
                    band.addCurve(to: CGPoint(x: width * 1.1, y: y - height * 0.04),
                                  controlPoint1: CGPoint(x: width * 0.26, y: y - height * 0.08),
                                  controlPoint2: CGPoint(x: width * 0.72, y: y + height * 0.02))
                    band.addLine(to: CGPoint(x: width * 1.1, y: height * 1.1))
                    band.addLine(to: CGPoint(x: -width * 0.1, y: height * 1.1))
                    band.close()
                    color.setFill()
                    band.fill()
                }
            case graphite:
                context.saveGState()
                context.setLineWidth(max(1, min(width, height) * 0.006))
                context.setStrokeColor(secondary.withAlphaComponent(0.55).cgColor)
                let step = max(26, min(width, height) * 0.12)
                var x = -height
                while x <= width + height {
                    context.move(to: CGPoint(x: x, y: 0))
                    context.addLine(to: CGPoint(x: x + height * 0.4, y: height))
                    x += step
                }
                context.strokePath()
                context.restoreGState()
                theme.card.withAlphaComponent(wallpaper ? 0.18 : 0.28).setFill()
                UIBezierPath(roundedRect: CGRect(x: width * 0.1, y: height * 0.16, width: width * 0.48, height: height * 0.26),
                             cornerRadius: min(width, height) * 0.06).fill()
            default:
                let ribbonA = UIBezierPath()
                ribbonA.move(to: CGPoint(x: -width * 0.1, y: height * 0.28))
                ribbonA.addCurve(to: CGPoint(x: width * 1.05, y: height * 0.12),
                                 controlPoint1: CGPoint(x: width * 0.22, y: height * 0.54),
                                 controlPoint2: CGPoint(x: width * 0.7, y: -height * 0.02))
                ribbonA.addLine(to: CGPoint(x: width * 1.05, y: height * 0.3))
                ribbonA.addCurve(to: CGPoint(x: -width * 0.1, y: height * 0.46),
                                 controlPoint1: CGPoint(x: width * 0.74, y: height * 0.5),
                                 controlPoint2: CGPoint(x: width * 0.2, y: height * 0.18))
                ribbonA.close()
                accent.setFill()
                ribbonA.fill()

                let ribbonB = UIBezierPath()
                ribbonB.move(to: CGPoint(x: -width * 0.1, y: height * 0.5))
                ribbonB.addCurve(to: CGPoint(x: width * 1.05, y: height * 0.38),
                                 controlPoint1: CGPoint(x: width * 0.28, y: height * 0.68),
                                 controlPoint2: CGPoint(x: width * 0.76, y: height * 0.18))
                ribbonB.addLine(to: CGPoint(x: width * 1.05, y: height * 0.55))
                ribbonB.addCurve(to: CGPoint(x: -width * 0.1, y: height * 0.66),
                                 controlPoint1: CGPoint(x: width * 0.76, y: height * 0.72),
                                 controlPoint2: CGPoint(x: width * 0.28, y: height * 0.42))
                ribbonB.close()
                accentAlt.setFill()
                ribbonB.fill()
            }

            card.setFill()
            let panels = [
                CGRect(x: width * 0.08, y: height * 0.12, width: width * 0.18, height: height * 0.11),
                CGRect(x: width * 0.32, y: height * 0.22, width: width * 0.22, height: height * 0.12),
                CGRect(x: width * 0.62, y: height * 0.62, width: width * 0.22, height: height * 0.12),
            ]
            for panel in panels {
                UIBezierPath(roundedRect: panel, cornerRadius: min(width, height) * 0.05).fill()
            }
        }
    }
}
