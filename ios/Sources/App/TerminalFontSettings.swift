import SwiftUI
import UIKit
import CoreText

final class TerminalFontSettings: ObservableObject {
    struct FontOption: Identifiable, Equatable {
        let id: String
        let displayName: String
        let postScriptName: String?
    }

    static let shared = TerminalFontSettings()
    static let appearanceDidChangeNotification = Notification.Name("TerminalFontSettingsAppearanceDidChange")
    static let preferencesDidChangeNotification = Notification.Name("TerminalPreferencesDidChange")

    private static let minPointSizeValue: CGFloat = 2.0
    private static let maxPointSizeValue: CGFloat = 28.0

    private let storageKey = "com.pscal.terminal.fontPointSize"
    private let fontNameKey = "com.pscal.terminal.fontName"
    private let backgroundKey = "com.pscal.terminal.backgroundColor"
    private let foregroundKey = "com.pscal.terminal.foregroundColor"
    private let editorWindowKey = "com.pscal.terminal.nextviWindow"
    private let locationDeviceKey = "com.pscal.terminal.locationDeviceEnabled"
    private let initialTabCountKey = "com.pscal.terminal.initialTabCount"

    static let editorWindowBuildEnabled: Bool = {
#if EDITOR_FLOATING_WINDOW
        return true
#else
        return false
#endif
    }()

    let minimumPointSize: CGFloat = TerminalFontSettings.minPointSizeValue
    let maximumPointSize: CGFloat = TerminalFontSettings.maxPointSizeValue

    static let defaultBackgroundColor = UIColor.black
    static let defaultForegroundColor = UIColor(red: 1.0, green: 0.64, blue: 0.0, alpha: 1.0)

    @Published var pointSize: CGFloat = 14.0
    @Published var backgroundColor: UIColor = TerminalFontSettings.defaultBackgroundColor
    @Published var foregroundColor: UIColor = TerminalFontSettings.defaultForegroundColor

    @Published var pathTruncationEnabled: Bool = false
    @Published private(set) var pathTruncationPath: String = ""
    @Published var locationDeviceEnabled: Bool = false
    @Published var initialTabCount: Int = 1

    @Published private(set) var selectedFontID: String
    @Published private(set) var editorWindowEnabled: Bool

    let fontOptions: [FontOption]

    private init() {
        fontOptions = TerminalFontSettings.buildFontOptions()
        selectedFontID = fontOptions.first?.id ?? "system"

        // Font size from defaults or env
        let stored = UserDefaults.standard.double(forKey: storageKey)
        let initialPointSize: CGFloat
        if stored > 0 {
            initialPointSize = CGFloat(stored)
        } else if let env = ProcessInfo.processInfo.environment["PSCALI_FONT_SIZE"],
                  let parsed = Double(env), parsed > 0 {
            initialPointSize = CGFloat(parsed)
        } else {
            initialPointSize = 14.0
        }

        let clampedSize = TerminalFontSettings.clampSize(initialPointSize)
        pointSize = clampedSize

        // Colors
        backgroundColor = TerminalFontSettings.loadColor(
            key: backgroundKey,
            fallback: TerminalFontSettings.defaultBackgroundColor
        )
        foregroundColor = TerminalFontSettings.loadColor(
            key: foregroundKey,
            fallback: TerminalFontSettings.defaultForegroundColor
        )

        // Font face
        let storedFontID = UserDefaults.standard.string(forKey: fontNameKey)
        let envFontName = ProcessInfo.processInfo.environment["PSCALI_FONT_NAME"]
        selectedFontID = TerminalFontSettings.resolveInitialFontID(
            storedID: storedFontID,
            envName: envFontName,
            options: fontOptions
        )

        // Editor window / external editor
        let storedEditorPref = UserDefaults.standard.object(forKey: editorWindowKey) as? Bool
        let envWindowMode = ProcessInfo.processInfo.environment["PSCALI_EDITOR_WINDOW_MODE"]
        editorWindowEnabled = TerminalFontSettings.editorWindowBuildEnabled
        ? TerminalFontSettings.resolveInitialEditorWindowSetting(stored: storedEditorPref,
                                                                envValue: envWindowMode)
        : false

        // Path truncation
        let storedTruncationPath = UserDefaults.standard.string(forKey: PathTruncationPreferences.pathDefaultsKey)
        let normalizedStored = PathTruncationManager.shared.normalize(storedTruncationPath ?? "")
        if normalizedStored.isEmpty {
            let defaultPath = PathTruncationManager.shared.normalize(
                PathTruncationManager.shared.defaultDocumentsPath()
            )
            pathTruncationPath = defaultPath
        } else {
            pathTruncationPath = normalizedStored
        }

        if UserDefaults.standard.object(forKey: PathTruncationPreferences.enabledDefaultsKey) != nil {
            pathTruncationEnabled = UserDefaults.standard.bool(
                forKey: PathTruncationPreferences.enabledDefaultsKey
            )
        } else {
            pathTruncationEnabled = !pathTruncationPath.isEmpty
        }

        if UserDefaults.standard.object(forKey: locationDeviceKey) != nil {
            locationDeviceEnabled = UserDefaults.standard.bool(forKey: locationDeviceKey)
        } else {
            // Default to enabled so /dev/location is present unless the user opts out.
            locationDeviceEnabled = true
        }
        LocationDeviceProvider.shared.setDeviceEnabled(locationDeviceEnabled)

        if UserDefaults.standard.object(forKey: initialTabCountKey) != nil {
            let storedCount = UserDefaults.standard.integer(forKey: initialTabCountKey)
            initialTabCount = TerminalFontSettings.clampInitialTabCount(storedCount)
        } else {
            initialTabCount = 1
        }

        applyPathTruncationPreferences()
    }

    // MARK: Basic font helpers

    func clamp(_ size: CGFloat) -> CGFloat {
        TerminalFontSettings.clampSize(size)
    }

    private static func clampSize(_ size: CGFloat) -> CGFloat {
        return min(max(size, minPointSizeValue), maxPointSizeValue)
    }

    var currentFont: UIFont {
        font(forPointSize: pointSize)
    }

    private var selectedFontOption: FontOption {
        fontOptions.first(where: { $0.id == selectedFontID }) ?? fontOptions.first!
    }

    func font(forPointSize size: CGFloat) -> UIFont {
        if let name = selectedFontOption.postScriptName,
           !name.hasPrefix("."),
           let custom = UIFont(name: name, size: size) {
            return custom
        }
        return UIFont.monospacedSystemFont(ofSize: size, weight: .regular)
    }

    func font(
        forPointSize size: CGFloat,
        weight: UIFont.Weight,
        italic: Bool = false
    ) -> UIFont {
        let base = font(forPointSize: size)
        var traits = base.fontDescriptor.symbolicTraits

        let wantsBold = weight == .bold
        let hasBold = traits.contains(.traitBold)
        let hasItalic = traits.contains(.traitItalic)

        if wantsBold != hasBold {
            if wantsBold {
                traits.insert(.traitBold)
            } else {
                traits.remove(.traitBold)
            }
        }
        if italic != hasItalic {
            if italic {
                traits.insert(.traitItalic)
            } else {
                traits.remove(.traitItalic)
            }
        }

        if traits != base.fontDescriptor.symbolicTraits,
           let descriptor = base.fontDescriptor.withSymbolicTraits(traits) {
            return UIFont(descriptor: descriptor, size: size)
        }
        return base
    }

    // MARK: Updates

    func updatePointSize(_ newSize: CGFloat) {
        let clamped = clamp(newSize)
        guard clamped != pointSize else { return }
        pointSize = clamped
        UserDefaults.standard.set(Double(clamped), forKey: storageKey)
        notifyChange()
    }

    func updateFontSelection(_ identifier: String) {
        guard identifier != selectedFontID,
              fontOptions.contains(where: { $0.id == identifier }) else {
            return
        }
        selectedFontID = identifier
        UserDefaults.standard.set(identifier, forKey: fontNameKey)
        notifyChange()
    }

    func updateBackgroundColor(_ color: UIColor) {
        let normalized = TerminalFontSettings.normalize(color)
        guard normalized != backgroundColor else { return }
        backgroundColor = normalized
        TerminalFontSettings.store(color: normalized, key: backgroundKey)
        notifyChange()
    }

    func updateForegroundColor(_ color: UIColor) {
        let normalized = TerminalFontSettings.normalize(color)
        guard normalized != foregroundColor else { return }
        foregroundColor = normalized
        TerminalFontSettings.store(color: normalized, key: foregroundKey)
        notifyChange()
    }

    func applyTransientAppearance(
        pointSize: CGFloat,
        fontIdentifier: String,
        backgroundColor: UIColor,
        foregroundColor: UIColor
    ) {
        let clampedSize = clamp(pointSize)
        let resolvedFontID: String = {
            if fontOptions.contains(where: { $0.id == fontIdentifier }) {
                return fontIdentifier
            }
            return fontOptions.first?.id ?? "system"
        }()
        let normalizedBackground = TerminalFontSettings.normalize(backgroundColor)
        let normalizedForeground = TerminalFontSettings.normalize(foregroundColor)

        var changed = false
        if self.pointSize != clampedSize {
            self.pointSize = clampedSize
            changed = true
        }
        if self.selectedFontID != resolvedFontID {
            self.selectedFontID = resolvedFontID
            changed = true
        }
        if self.backgroundColor != normalizedBackground {
            self.backgroundColor = normalizedBackground
            changed = true
        }
        if self.foregroundColor != normalizedForeground {
            self.foregroundColor = normalizedForeground
            changed = true
        }

        if changed {
            notifyChange()
        }
    }

    func updateInitialTabCount(_ count: Int) {
        let clamped = TerminalFontSettings.clampInitialTabCount(count)
        guard clamped != initialTabCount else { return }
        initialTabCount = clamped
        UserDefaults.standard.set(clamped, forKey: initialTabCountKey)
        notifyPreferencesChange()
    }

    func updatePathTruncationEnabled(_ enabled: Bool) {
        guard enabled != pathTruncationEnabled else { return }
        pathTruncationEnabled = enabled
        UserDefaults.standard.set(enabled, forKey: PathTruncationPreferences.enabledDefaultsKey)

        if enabled && pathTruncationPath.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            let defaultPath = PathTruncationManager.shared.normalize(
                PathTruncationManager.shared.defaultDocumentsPath()
            )
            pathTruncationPath = defaultPath
            UserDefaults.standard.set(defaultPath, forKey: PathTruncationPreferences.pathDefaultsKey)
        }

        applyPathTruncationPreferences()
    }

    func updatePathTruncationPath(_ newPath: String) {
        let normalized = PathTruncationManager.shared.normalize(newPath)
        guard normalized != pathTruncationPath else { return }
        pathTruncationPath = normalized
        UserDefaults.standard.set(normalized, forKey: PathTruncationPreferences.pathDefaultsKey)
        if pathTruncationEnabled {
            applyPathTruncationPreferences()
        }
    }

    func updateLocationDeviceEnabled(_ enabled: Bool) {
        guard enabled != locationDeviceEnabled else { return }
        locationDeviceEnabled = enabled
        UserDefaults.standard.set(enabled, forKey: locationDeviceKey)
        LocationDeviceProvider.shared.setDeviceEnabled(enabled)
    }

    func useDocumentsPathForTruncation() {
        let defaultPath = PathTruncationManager.shared.defaultDocumentsPath()
        updatePathTruncationPath(defaultPath)
    }

    private func applyPathTruncationPreferences() {
        PathTruncationManager.shared.apply(enabled: pathTruncationEnabled,
                                           path: pathTruncationPath)
    }

    // MARK: Color persistence

    private static func store(color: UIColor, key: String) {
        if let data = try? NSKeyedArchiver.archivedData(
            withRootObject: color,
            requiringSecureCoding: false
        ) {
            UserDefaults.standard.set(data, forKey: key)
        }
    }

    private static func loadColor(key: String, fallback: UIColor) -> UIColor {
        guard let data = UserDefaults.standard.data(forKey: key),
              let color = try? NSKeyedUnarchiver.unarchivedObject(
                ofClass: UIColor.self,
                from: data
              ) else {
            return fallback
        }
        return normalize(color)
    }

    private static func normalize(_ color: UIColor) -> UIColor {
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
        if color.getRed(&r, green: &g, blue: &b, alpha: &a) {
            return UIColor(red: r, green: g, blue: b, alpha: a)
        }
        return color
    }

    // MARK: Notify

    private func notifyChange() {
        NotificationCenter.default.post(
            name: TerminalFontSettings.appearanceDidChangeNotification,
            object: nil
        )
    }

    private func notifyPreferencesChange() {
        NotificationCenter.default.post(
            name: TerminalFontSettings.preferencesDidChangeNotification,
            object: nil
        )
    }

    static func clampInitialTabCount(_ value: Int) -> Int {
        return min(max(value, 1), 5)
    }

    // MARK: Font option discovery

    private static func sanitizedFontName(_ rawName: String) -> String? {
        let trimmed = rawName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }
        if trimmed.hasPrefix(".") {
            return nil
        }
        let lower = trimmed.lowercased()
        if lower.hasPrefix("sfcompact") || lower.hasPrefix("sfns") {
            // Avoid private system UI fonts that trigger CoreText warnings.
            return nil
        }
        return trimmed
    }

    private static func buildFontOptions() -> [FontOption] {
        var options: [FontOption] = [
            FontOption(id: "system",
                       displayName: "System Monospaced",
                       postScriptName: nil)
        ]

        guard let postScriptNames = CTFontManagerCopyAvailablePostScriptNames() as? [String] else {
            return options
        }

        var regularByFamily: [String: FontOption] = [:]
        var fallbackByFamily: [String: FontOption] = [:]

        for rawName in postScriptNames {
            guard let name = sanitizedFontName(rawName) else { continue }

            let font = CTFontCreateWithName(name as CFString, 0, nil)
            let traits = CTFontGetSymbolicTraits(font)
            if !traits.contains(.monoSpaceTrait) {
                continue
            }

            let familyNameRaw = (CTFontCopyName(font, kCTFontFamilyNameKey) as String?) ?? name
            let familyName = familyNameRaw.trimmingCharacters(in: .whitespacesAndNewlines)
            let styleName = (CTFontCopyName(font, kCTFontStyleNameKey) as String?)?.lowercased() ?? ""
            let familyKey = familyName.lowercased()

            let option = FontOption(id: name,
                                    displayName: familyName.isEmpty ? name : familyName,
                                    postScriptName: name)

            let isRegular = styleName.isEmpty ||
            styleName.contains("regular") ||
            styleName.contains("roman") ||
            styleName.contains("plain") ||
            styleName.contains("normal")

            if isRegular {
                regularByFamily[familyKey] = option
            } else if fallbackByFamily[familyKey] == nil {
                fallbackByFamily[familyKey] = option
            }
        }

        let allFamilies = Set(regularByFamily.keys).union(fallbackByFamily.keys).sorted()
        for key in allFamilies {
            if let option = regularByFamily[key] ?? fallbackByFamily[key] {
                if !options.contains(where: { $0.id == option.id }) {
                    options.append(option)
                }
            }
        }

        return options
    }

    private static func resolveInitialFontID(
        storedID: String?,
        envName: String?,
        options: [FontOption]
    ) -> String {
        if let storedID,
           options.contains(where: { $0.id == storedID }) {
            return storedID
        }
        if let envName,
           let match = resolveOption(fromName: envName, options: options) {
            return match.id
        }
        return options.first?.id ?? "system"
    }

    private static func resolveOption(
        fromName name: String,
        options: [FontOption]
    ) -> FontOption? {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }

        return options.first(where: { option in
            if option.id.caseInsensitiveCompare(trimmed) == .orderedSame {
                return true
            }
            if let postScript = option.postScriptName,
               postScript.caseInsensitiveCompare(trimmed) == .orderedSame {
                return true
            }
            return option.displayName.caseInsensitiveCompare(trimmed) == .orderedSame
        })
    }

    private static func resolveInitialEditorWindowSetting(
        stored: Bool?,
        envValue: String?
    ) -> Bool {
        if !TerminalFontSettings.editorWindowBuildEnabled {
            return false
        }

        if let stored = stored {
            return stored
        }

        guard let env = envValue?
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .lowercased(),
              !env.isEmpty else {
            return false
        }

        if ["inline", "main", "primary", "embedded", "0", "false", "no"].contains(env) {
            return false
        }
        if ["window", "external", "secondary", "1", "true", "yes"].contains(env) {
            return true
        }
        return false
    }

    func updateEditorWindowEnabled(_ enabled: Bool) {
        guard TerminalFontSettings.editorWindowBuildEnabled else { return }
        guard enabled != editorWindowEnabled else { return }
        editorWindowEnabled = enabled
        UserDefaults.standard.set(enabled, forKey: editorWindowKey)
        NotificationCenter.default.post(name: TerminalFontSettings.preferencesDidChangeNotification,
                                        object: nil)
    }
}

@MainActor
final class TerminalTabAppearanceSettings: ObservableObject {
    private static let minPointSizeValue: CGFloat = 2.0
    private static let maxPointSizeValue: CGFloat = 28.0

    let profileID: String
    let minimumPointSize: CGFloat = TerminalTabAppearanceSettings.minPointSizeValue
    let maximumPointSize: CGFloat = TerminalTabAppearanceSettings.maxPointSizeValue
    let fontOptions: [TerminalFontSettings.FontOption]

    @Published var pointSize: CGFloat
    @Published private(set) var selectedFontID: String
    @Published var backgroundColor: UIColor
    @Published var foregroundColor: UIColor

    private let pointSizeKey: String
    private let fontNameKey: String
    private let backgroundKey: String
    private let foregroundKey: String

    init(profileID: String, defaults: TerminalFontSettings = .shared) {
        self.profileID = profileID
        self.fontOptions = defaults.fontOptions

        let suffix = Self.sanitizedProfileID(profileID)
        self.pointSizeKey = "com.pscal.terminal.tabAppearance.\(suffix).fontPointSize"
        self.fontNameKey = "com.pscal.terminal.tabAppearance.\(suffix).fontName"
        self.backgroundKey = "com.pscal.terminal.tabAppearance.\(suffix).backgroundColor"
        self.foregroundKey = "com.pscal.terminal.tabAppearance.\(suffix).foregroundColor"

        let storedSize = UserDefaults.standard.double(forKey: pointSizeKey)
        if storedSize > 0 {
            self.pointSize = Self.clampSize(CGFloat(storedSize))
        } else {
            self.pointSize = Self.clampSize(defaults.pointSize)
        }

        if let storedFontID = UserDefaults.standard.string(forKey: fontNameKey),
           fontOptions.contains(where: { $0.id == storedFontID }) {
            self.selectedFontID = storedFontID
        } else if fontOptions.contains(where: { $0.id == defaults.selectedFontID }) {
            self.selectedFontID = defaults.selectedFontID
        } else {
            self.selectedFontID = fontOptions.first?.id ?? "system"
        }

        self.backgroundColor = Self.loadColor(key: backgroundKey, fallback: defaults.backgroundColor)
        self.foregroundColor = Self.loadColor(key: foregroundKey, fallback: defaults.foregroundColor)
    }

    func clamp(_ size: CGFloat) -> CGFloat {
        Self.clampSize(size)
    }

    var currentFont: UIFont {
        font(forPointSize: pointSize)
    }

    func font(forPointSize size: CGFloat) -> UIFont {
        let selectedOption = fontOptions.first(where: { $0.id == selectedFontID }) ?? fontOptions.first
        if let name = selectedOption?.postScriptName,
           !name.hasPrefix("."),
           let custom = UIFont(name: name, size: size) {
            return custom
        }
        return UIFont.monospacedSystemFont(ofSize: size, weight: .regular)
    }

    func font(
        forPointSize size: CGFloat,
        weight: UIFont.Weight,
        italic: Bool = false
    ) -> UIFont {
        let base = font(forPointSize: size)
        var traits = base.fontDescriptor.symbolicTraits

        let wantsBold = weight == .bold
        let hasBold = traits.contains(.traitBold)
        let hasItalic = traits.contains(.traitItalic)

        if wantsBold != hasBold {
            if wantsBold {
                traits.insert(.traitBold)
            } else {
                traits.remove(.traitBold)
            }
        }
        if italic != hasItalic {
            if italic {
                traits.insert(.traitItalic)
            } else {
                traits.remove(.traitItalic)
            }
        }

        if traits != base.fontDescriptor.symbolicTraits,
           let descriptor = base.fontDescriptor.withSymbolicTraits(traits) {
            return UIFont(descriptor: descriptor, size: size)
        }
        return base
    }

    func updatePointSize(_ newSize: CGFloat) {
        let clamped = clamp(newSize)
        guard clamped != pointSize else { return }
        pointSize = clamped
        UserDefaults.standard.set(Double(clamped), forKey: pointSizeKey)
        notifyChange()
    }

    func updateFontSelection(_ identifier: String) {
        guard identifier != selectedFontID,
              fontOptions.contains(where: { $0.id == identifier }) else {
            return
        }
        selectedFontID = identifier
        UserDefaults.standard.set(identifier, forKey: fontNameKey)
        notifyChange()
    }

    func updateBackgroundColor(_ color: UIColor) {
        let normalized = Self.normalize(color)
        guard normalized != backgroundColor else { return }
        backgroundColor = normalized
        Self.store(color: normalized, key: backgroundKey)
        notifyChange()
    }

    func updateForegroundColor(_ color: UIColor) {
        let normalized = Self.normalize(color)
        guard normalized != foregroundColor else { return }
        foregroundColor = normalized
        Self.store(color: normalized, key: foregroundKey)
        notifyChange()
    }

    private static func clampSize(_ size: CGFloat) -> CGFloat {
        min(max(size, minPointSizeValue), maxPointSizeValue)
    }

    private static func sanitizedProfileID(_ value: String) -> String {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "._-"))
        let scalars = value.unicodeScalars.map { allowed.contains($0) ? Character($0) : "_" }
        return String(scalars)
    }

    private static func store(color: UIColor, key: String) {
        if let data = try? NSKeyedArchiver.archivedData(
            withRootObject: color,
            requiringSecureCoding: false
        ) {
            UserDefaults.standard.set(data, forKey: key)
        }
    }

    private static func loadColor(key: String, fallback: UIColor) -> UIColor {
        guard let data = UserDefaults.standard.data(forKey: key),
              let color = try? NSKeyedUnarchiver.unarchivedObject(
                ofClass: UIColor.self,
                from: data
              ) else {
            return normalize(fallback)
        }
        return normalize(color)
    }

    private static func normalize(_ color: UIColor) -> UIColor {
        var r: CGFloat = 0
        var g: CGFloat = 0
        var b: CGFloat = 0
        var a: CGFloat = 0
        if color.getRed(&r, green: &g, blue: &b, alpha: &a) {
            return UIColor(red: r, green: g, blue: b, alpha: a)
        }
        return color
    }

    private func notifyChange() {
        NotificationCenter.default.post(
            name: TerminalFontSettings.appearanceDidChangeNotification,
            object: nil
        )
    }
}
