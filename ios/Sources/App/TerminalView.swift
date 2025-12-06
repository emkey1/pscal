import SwiftUI
import UIKit
import Combine
import QuartzCore
import CoreText
import Darwin

@_silgen_name("pscalRuntimeDebugLog")
private func c_terminalDebugLog(_ message: UnsafePointer<CChar>) -> Void

private func withCStringPointer(_ string: String, _ body: (UnsafePointer<CChar>) -> Void) {
    let utf8 = string.utf8CString
    utf8.withUnsafeBufferPointer { buffer in
        if let base = buffer.baseAddress {
            body(base)
        }
    }
}

private func terminalViewLog(_ message: String) {
    withCStringPointer(message) { c_terminalDebugLog($0) }
}

// MARK: - Font Settings (Restored)

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
    private let elvisWindowKey = "com.pscal.terminal.nextviWindow"
    static let elvisWindowBuildEnabled: Bool = {
#if ELVIS_FLOATING_WINDOW
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
    @Published private(set) var selectedFontID: String
    @Published private(set) var elvisWindowEnabled: Bool

    let fontOptions: [FontOption]

    private init() {
        fontOptions = TerminalFontSettings.buildFontOptions()
        selectedFontID = fontOptions.first?.id ?? "system"

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
        backgroundColor = TerminalFontSettings.loadColor(key: backgroundKey, fallback: TerminalFontSettings.defaultBackgroundColor)
        foregroundColor = TerminalFontSettings.loadColor(key: foregroundKey, fallback: TerminalFontSettings.defaultForegroundColor)

        let storedFontID = UserDefaults.standard.string(forKey: fontNameKey)
        let envFontName = ProcessInfo.processInfo.environment["PSCALI_FONT_NAME"]
        selectedFontID = TerminalFontSettings.resolveInitialFontID(storedID: storedFontID,
                                                                   envName: envFontName,
                                                                   options: fontOptions)

        let storedElvisPref = UserDefaults.standard.object(forKey: elvisWindowKey) as? Bool
        let envWindowMode = ProcessInfo.processInfo.environment["PSCALI_ELVIS_WINDOW_MODE"]
        elvisWindowEnabled = TerminalFontSettings.elvisWindowBuildEnabled ?
            TerminalFontSettings.resolveInitialElvisWindowSetting(stored: storedElvisPref,
                                                                  envValue: envWindowMode) : false

        let storedTruncationPath = UserDefaults.standard.string(forKey: PathTruncationPreferences.pathDefaultsKey)
        let normalizedStored = PathTruncationManager.shared.normalize(storedTruncationPath ?? "")
        if normalizedStored.isEmpty {
            let defaultPath = PathTruncationManager.shared.normalize(PathTruncationManager.shared.defaultDocumentsPath())
            pathTruncationPath = defaultPath
        } else {
            pathTruncationPath = normalizedStored
        }
        if UserDefaults.standard.object(forKey: PathTruncationPreferences.enabledDefaultsKey) != nil {
            pathTruncationEnabled = UserDefaults.standard.bool(forKey: PathTruncationPreferences.enabledDefaultsKey)
        } else {
            pathTruncationEnabled = !pathTruncationPath.isEmpty
        }
        applyPathTruncationPreferences()
    }

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

    func font(forPointSize size: CGFloat, weight: UIFont.Weight, italic: Bool = false) -> UIFont {
        let base = font(forPointSize: size)
        var traits = base.fontDescriptor.symbolicTraits
        let wantsBold = weight == .bold
        let hasBold = traits.contains(.traitBold)
        let hasItalic = traits.contains(.traitItalic)
        if wantsBold != hasBold {
            if wantsBold { traits.insert(.traitBold) } else { traits.remove(.traitBold) }
        }
        if italic != hasItalic {
            if italic { traits.insert(.traitItalic) } else { traits.remove(.traitItalic) }
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

    func updatePathTruncationEnabled(_ enabled: Bool) {
        guard enabled != pathTruncationEnabled else { return }
        pathTruncationEnabled = enabled
        UserDefaults.standard.set(enabled, forKey: PathTruncationPreferences.enabledDefaultsKey)
        if enabled && pathTruncationPath.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            let defaultPath = PathTruncationManager.shared.normalize(PathTruncationManager.shared.defaultDocumentsPath())
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

    func useDocumentsPathForTruncation() {
        let defaultPath = PathTruncationManager.shared.defaultDocumentsPath()
        updatePathTruncationPath(defaultPath)
    }

    private func applyPathTruncationPreferences() {
        PathTruncationManager.shared.apply(enabled: pathTruncationEnabled, path: pathTruncationPath)
    }

    private static func store(color: UIColor, key: String) {
        if let data = try? NSKeyedArchiver.archivedData(withRootObject: color, requiringSecureCoding: false) {
            UserDefaults.standard.set(data, forKey: key)
        }
    }

    private static func loadColor(key: String, fallback: UIColor) -> UIColor {
        guard let data = UserDefaults.standard.data(forKey: key),
              let color = try? NSKeyedUnarchiver.unarchivedObject(ofClass: UIColor.self, from: data) else {
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

    private func notifyChange() {
        NotificationCenter.default.post(name: TerminalFontSettings.appearanceDidChangeNotification, object: nil)
    }

    private static func buildFontOptions() -> [FontOption] {
        var options: [FontOption] = [
            FontOption(id: "system", displayName: "System Monospaced", postScriptName: nil)
        ]

        guard let postScriptNames = CTFontManagerCopyAvailablePostScriptNames() as? [String] else {
            return options
        }

        var regularByFamily: [String: FontOption] = [:]
        var fallbackByFamily: [String: FontOption] = [:]

        for name in postScriptNames {
            if name.hasPrefix(".") {
                continue
            }
            let font = CTFontCreateWithName(name as CFString, 0, nil)
            let traits = CTFontGetSymbolicTraits(font)
            if !traits.contains(.monoSpaceTrait) {
                continue
            }
            let familyName = (CTFontCopyName(font, kCTFontFamilyNameKey) as String?) ?? name
            let styleName = (CTFontCopyName(font, kCTFontStyleNameKey) as String?)?.lowercased() ?? ""
            let familyKey = familyName.lowercased()
            let option = FontOption(id: name, displayName: familyName, postScriptName: name)
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

    private static func resolveInitialFontID(storedID: String?,
                                             envName: String?,
                                             options: [FontOption]) -> String {
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

    private static func resolveOption(fromName name: String, options: [FontOption]) -> FontOption? {
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

    private static func resolveInitialElvisWindowSetting(stored: Bool?, envValue: String?) -> Bool {
        if !TerminalFontSettings.elvisWindowBuildEnabled {
            return false;
        }
        if let stored = stored {
            return stored
        }
        guard let env = envValue?.trimmingCharacters(in: .whitespacesAndNewlines).lowercased(),
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

    func updateElvisWindowEnabled(_ enabled: Bool) {
        guard TerminalFontSettings.elvisWindowBuildEnabled else { return }
        guard enabled != elvisWindowEnabled else { return }
        elvisWindowEnabled = enabled
        UserDefaults.standard.set(enabled, forKey: elvisWindowKey)
        NotificationCenter.default.post(name: TerminalFontSettings.preferencesDidChangeNotification, object: nil)
    }
}

// MARK: - Main Terminal View

struct TerminalView: View {
    let showsOverlay: Bool
    @ObservedObject private var fontSettings = TerminalFontSettings.shared
    @State private var showingSettings = false
    @State private var focusAnchor: Int = 0
    
    // FIX: Removed keyboardOverlap state. We trust the parent view's frame now.

    init(showsOverlay: Bool = true) {
        self.showsOverlay = showsOverlay
    }

    var body: some View {
        ZStack(alignment: .bottomTrailing) {
            // FIX: Removed KeyboardAwareContainer.
            // We use GeometryReader directly. The size provided here is ALREADY
            // adjusted for the keyboard by TerminalRootViewController constraints.
            GeometryReader { proxy in
                TerminalContentView(availableSize: proxy.size,
                                    safeAreaInsets: proxy.safeAreaInsets,
                                    keyboardOverlap: 0, // Force 0. Don't subtract twice.
                                    fontSettings: fontSettings,
                                    focusAnchor: $focusAnchor)
                    .frame(width: proxy.size.width, height: proxy.size.height)
            }
            .edgesIgnoringSafeArea(.bottom)

            if showsOverlay {
                HStack(alignment: .center, spacing: 10) {
                    Button(action: {
                        PscalRuntimeBootstrap.shared.resetTerminalState()
                        focusAnchor &+= 1
                    }) {
                        Text("R")
                            .font(.system(size: 16, weight: .semibold, design: .rounded))
                            .padding(11)
                            .background(.ultraThinMaterial, in: Circle())
                    }
                    .accessibilityLabel("Reset Terminal")

                    Button(action: { showingSettings = true }) {
                        Image(systemName: "textformat.size")
                            .font(.system(size: 16, weight: .semibold))
                            .padding(8)
                            .background(.ultraThinMaterial, in: Circle())
                    }
                    .accessibilityLabel("Adjust Font Size")
                }
                .padding(.bottom, {
                    // Anchor buttons relative to the view bottom.
                    // Since the view bottom is now the "Keyboard Top",
                    // we just need a small padding (16) to float above the keys.
                    return 16
                }())
                .padding(.trailing, 10)
            }
        }
        .sheet(isPresented: $showingSettings) {
            TerminalSettingsView()
        }
        .background(Color(.systemBackground))
    }
}

private func currentSafeBottomInset() -> CGFloat {
    #if os(iOS)
    let scene = UIApplication.shared.connectedScenes
        .compactMap { $0 as? UIWindowScene }
        .first
    let window = scene?.windows.first { $0.isKeyWindow }
    return window?.safeAreaInsets.bottom ?? 0
    #else
    return 0
    #endif
}

extension TerminalView {
    fileprivate static func computeKeyboardHeight(from note: Notification) -> CGFloat? {
        guard let frame = (note.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? NSValue)?.cgRectValue else {
            return nil
        }
        guard let window = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .flatMap({ $0.windows })
            .first(where: { $0.isKeyWindow }) else {
            return nil
        }
        let converted = window.convert(frame, from: nil)
        let overlap = max(0, window.bounds.maxY - converted.minY)
        return min(overlap, window.bounds.height)
    }
}

// MARK: - Terminal Content View

private struct TerminalContentView: View {
    private static let topPadding: CGFloat = 32.0
    let availableSize: CGSize
    let safeAreaInsets: EdgeInsets
    @ObservedObject private var fontSettings: TerminalFontSettings
    @ObservedObject private var runtime = PscalRuntimeBootstrap.shared
    @Binding private var focusAnchor: Int
    @State private var lastLoggedMetrics: TerminalGeometryMetrics?
    
    // We keep this parameter signature to match existing inits, but we ignore it or pass 0
    let keyboardOverlap: CGFloat

    init(availableSize: CGSize,
         safeAreaInsets: EdgeInsets,
         keyboardOverlap: CGFloat,
         fontSettings: TerminalFontSettings,
         focusAnchor: Binding<Int>) {
        self.availableSize = availableSize
        self.safeAreaInsets = safeAreaInsets
        self.keyboardOverlap = keyboardOverlap
        _fontSettings = ObservedObject(wrappedValue: fontSettings)
        _focusAnchor = focusAnchor
    }

    var body: some View {
        let elvisToken = runtime.elvisRenderToken
        let elvisActive = runtime.isElvisModeActive()
        let elvisVisible = EditorWindowManager.shared.isVisible
        let currentFont = fontSettings.currentFont
        return VStack(spacing: 0) {
            TerminalRendererView(text: runtime.screenText,
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
                                 mouseEncoding: runtime.mouseEncoding)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Color(.systemBackground))

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
        .padding(.top, Self.topPadding)
        .background(Color(fontSettings.backgroundColor))
        .contentShape(Rectangle())
        .onTapGesture {
            focusAnchor &+= 1
        }
        .overlay(alignment: .bottomLeading) {
            if !EditorWindowManager.shared.isVisible {
                TerminalInputBridge(focusAnchor: $focusAnchor,
                                    onInput: handleInput,
                                    onPaste: handlePaste)
                    .frame(width: 1, height: 1)
                    .allowsHitTesting(false)
            }
        }
        .onAppear {
            updateTerminalGeometry()
            runtime.start()
                    
            // FIX: Delay the initial focus request by 0.5s.
            // This gives the UIWindow time to settle so it accepts the First Responder
            // request (hardware keyboard capture) reliably on cold boot.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                focusAnchor &+= 1
            }
        }
        .onChange(of: availableSize) { _ in
            updateTerminalGeometry()
        }
        .onChange(of: runtime.exitStatus) { _ in
            updateTerminalGeometry()
        }
        .onReceive(NotificationCenter.default.publisher(for: UIContentSizeCategory.didChangeNotification)) { _ in
            updateTerminalGeometry()
        }
        .onChange(of: safeAreaInsets) { _ in
            updateTerminalGeometry()
        }
        .onChange(of: fontSettings.pointSize) { _ in
            updateTerminalGeometry()
        }
        .onReceive(fontSettings.$selectedFontID) { _ in
            updateTerminalGeometry()
        }
    }

    private func handleInput(_ text: String) {
        runtime.send(text)
    }

    private func handlePaste(_ text: String) {
        runtime.sendPasted(text)
    }

    private func updateTerminalGeometry() {
        let showingStatus = runtime.exitStatus != nil
        let font = fontSettings.currentFont
        
        // FIX: Don't subtract keyboardOverlap here. The `availableSize` passed in
        // by GeometryReader is already the correct, keyboard-avoiding size.
        let effectiveHeight = max(1, availableSize.height)
        
        let sizeForMetrics = CGSize(width: availableSize.width, height: effectiveHeight)
        
        guard let candidate = TerminalGeometryCalculator.metrics(for: sizeForMetrics,
                                                                 safeAreaInsets: safeAreaInsets,
                                                                 topPadding: Self.topPadding,
                                                                 showingStatus: showingStatus,
                                                                 font: font)
                ?? TerminalGeometryCalculator.fallbackMetrics(showingStatus: showingStatus, font: font) else {
            return
        }
        let metrics = candidate
        #if DEBUG
        if lastLoggedMetrics != metrics {
            let grid = TerminalGeometryCalculator.calculateGrid(for: sizeForMetrics,
                                                                font: font,
                                                                safeAreaInsets: UIEdgeInsets(top: safeAreaInsets.top,
                                                                                             left: safeAreaInsets.leading,
                                                                                             bottom: safeAreaInsets.bottom,
                                                                                             right: safeAreaInsets.trailing),
                                                                topPadding: Self.topPadding,
                                                                horizontalPadding: TerminalGeometryCalculator.horizontalPadding,
                                                                showingStatus: showingStatus)
            let char = TerminalGeometryCalculator.characterMetrics(for: font)
            terminalViewLog(String(format: "[TerminalView] available %.1fx%.1f usable %.1fx%.1f -> rows=%d cols=%d",
                                   availableSize.width,
                                   availableSize.height,
                                   grid.width,
                                   grid.height,
                                   metrics.rows,
                                   metrics.columns))
            lastLoggedMetrics = metrics
        }
        #endif
        runtime.updateTerminalSize(columns: metrics.columns, rows: metrics.rows)
    }
}

struct TerminalView_Previews: PreviewProvider {
    static var previews: some View {
        TerminalView()
            .previewDevice("iPad Pro (11-inch) (4th generation)")
    }
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

// MARK: - Geometry Calculator (Fixed)

enum TerminalGeometryCalculator {
    static let horizontalPadding: CGFloat = 0.0
    private static let verticalRowPadding: CGFloat = 0.0
    private static let statusOverlayHeight: CGFloat = 32.0
    private struct FontMetricKey: Hashable {
        let name: String
        let pointSize: CGFloat
    }
    private static var metricCache: [FontMetricKey: (CGFloat, CGFloat)] = [:]
    private static let metricLock = NSLock()

    static func characterMetrics(for font: UIFont) -> (width: CGFloat, lineHeight: CGFloat) {
        let key = FontMetricKey(name: font.fontName, pointSize: font.pointSize)
        if let cached = cachedMetrics(for: key) {
            return cached
        }
        let measured = measureMetrics(for: font)
        storeMetrics(measured, for: key)
        return measured
    }

    private static func cachedMetrics(for key: FontMetricKey) -> (CGFloat, CGFloat)? {
        metricLock.lock()
        let value = metricCache[key]
        metricLock.unlock()
        return value
    }

    private static func storeMetrics(_ metrics: (CGFloat, CGFloat), for key: FontMetricKey) {
        metricLock.lock()
        metricCache[key] = metrics
        metricLock.unlock()
    }

    private static func measureMetrics(for font: UIFont) -> (CGFloat, CGFloat) {
        let ctFont: CTFont = font as CTFont
        let character: UniChar = ("M" as NSString).character(at: 0)
        var glyph: CGGlyph = 0
        CTFontGetGlyphsForCharacters(ctFont, [character], &glyph, 1)
        var advance = CGSize.zero
        CTFontGetAdvancesForGlyphs(ctFont, .horizontal, [glyph], &advance, 1)
        let width = max(1.0, advance.width)
        let height = max(font.lineHeight, font.pointSize)
        return (width, height)
    }

    private static func pixelCeil(_ value: CGFloat) -> CGFloat {
        let scale = UIScreen.main.scale
        return ceil(value * scale) / scale
    }

    static func calculateGrid(for size: CGSize,
                              font: UIFont,
                              safeAreaInsets: UIEdgeInsets,
                              topPadding: CGFloat,
                              horizontalPadding: CGFloat,
                              showingStatus: Bool) -> TerminalGridCapacity {
        var availableHeight = size.height
        availableHeight -= topPadding
        if showingStatus {
            availableHeight -= statusOverlayHeight
        }
        
        // --- FIX: Heuristic to avoid double subtraction ---
        let screenHeight = UIScreen.main.bounds.height
        let isKeyboardUp = size.height < (screenHeight * 0.6)
        
        if !isKeyboardUp {
            availableHeight -= safeAreaInsets.bottom
        }
        
        availableHeight = max(0, availableHeight)

        let adjustedLineHeight = pixelCeil(font.lineHeight)

        var availableWidth = size.width
        availableWidth -= (horizontalPadding * 2)
        availableWidth = max(0, availableWidth)

        let sample = "MMMMMMMMMM"
        let sampleWidth = (sample as NSString).size(withAttributes: [.font: font]).width
        let avgCharWidth = max(1.0, sampleWidth / CGFloat(sample.count))

        let rows = Int(floor(availableHeight / adjustedLineHeight))
        let cols = Int(floor(availableWidth / avgCharWidth))

        return TerminalGridCapacity(rows: rows, columns: cols, width: availableWidth, height: availableHeight)
    }

    static func metrics(for size: CGSize,
                        safeAreaInsets: EdgeInsets,
                        topPadding: CGFloat,
                        showingStatus: Bool,
                        font: UIFont) -> TerminalGeometryMetrics? {
        guard size.width > 10, size.height > 10 else { return nil }

        let uiInsets = UIEdgeInsets(top: safeAreaInsets.top,
                                    left: safeAreaInsets.leading,
                                    bottom: safeAreaInsets.bottom,
                                    right: safeAreaInsets.trailing)
        let grid = calculateGrid(for: size,
                                 font: font,
                                 safeAreaInsets: uiInsets,
                                 topPadding: topPadding,
                                 horizontalPadding: horizontalPadding,
                                 showingStatus: showingStatus)

        let rawColumns = grid.columns
        let rawRows = grid.rows
        
        guard rawColumns > 0, rawRows > 0 else { return nil }

        return TerminalGeometryMetrics(
            columns: max(10, rawColumns),
            rows: max(4, rawRows)
        )
    }

    static func fallbackMetrics(showingStatus: Bool, font: UIFont) -> TerminalGeometryMetrics? {
        return metrics(for: UIScreen.main.bounds.size,
                       safeAreaInsets: EdgeInsets(),
                       topPadding: 0,
                       showingStatus: showingStatus,
                       font: font)
    }
}

// MARK: - Renderer Views

struct TerminalRendererView: UIViewRepresentable {
    let text: NSAttributedString
    let cursor: TerminalCursorInfo?
    let backgroundColor: UIColor
    let foregroundColor: UIColor
    let isElvisMode: Bool
    let isElvisWindowVisible: Bool
    let elvisRenderToken: UInt64
    let font: UIFont
    let fontPointSize: CGFloat
    let elvisSnapshot: ElvisSnapshot?
    var onPaste: ((String) -> Void)? = nil
    let mouseMode: TerminalBuffer.MouseMode
    let mouseEncoding: TerminalBuffer.MouseEncoding

    func makeUIView(context: Context) -> TerminalRendererContainerView {
        let container = TerminalRendererContainerView()
        container.configure(backgroundColor: backgroundColor, foregroundColor: foregroundColor)
        container.applyFont(font: font)
        container.onPaste = onPaste
        container.updateMouseState(mode: mouseMode, encoding: mouseEncoding)
        return container
    }

    func updateUIView(_ uiView: TerminalRendererContainerView, context: Context) {
        _ = elvisRenderToken
        uiView.configure(backgroundColor: backgroundColor, foregroundColor: foregroundColor)
        uiView.applyFont(font: font)
        uiView.onPaste = onPaste
        uiView.updateMouseState(mode: mouseMode, encoding: mouseEncoding)
        let externalWindowEnabled = EditorWindowManager.externalWindowEnabled
        let shouldBlankMain = isElvisMode && isElvisWindowVisible && externalWindowEnabled
        if shouldBlankMain {
            uiView.update(text: NSAttributedString(string: ""),
                          cursor: nil,
                          backgroundColor: backgroundColor,
                          isElvisMode: false,
                          elvisSnapshot: nil)
            return
        }
        let shouldUseSnapshot = isElvisMode && (!externalWindowEnabled || !isElvisWindowVisible)
        let snapshot: ElvisSnapshot?
        if shouldUseSnapshot {
            snapshot = elvisSnapshot ?? EditorTerminalBridge.shared.snapshot()
        } else {
            snapshot = nil
        }
        uiView.update(text: text,
                      cursor: cursor,
                      backgroundColor: backgroundColor,
                      isElvisMode: shouldUseSnapshot,
                      elvisSnapshot: snapshot)
    }
}

private enum TerminalFontMetrics {
    static var displayFont: UIFont {
        TerminalFontSettings.shared.currentFont
    }

    static var characterWidth: CGFloat {
        let font = displayFont
        return max(1, ("W" as NSString).size(withAttributes: [.font: font]).width)
    }

    static var lineHeight: CGFloat {
        displayFont.lineHeight
    }
}

final class TerminalRendererContainerView: UIView, UIGestureRecognizerDelegate {
    private let terminalView = TerminalDisplayTextView()
    private let selectionOverlay = TerminalSelectionOverlay()
    private let selectionMenu = TerminalSelectionMenuView()
    private let commandIndicator = UILabel()
 
    // --- MOUSE TRACKING STATE ---
    private var mouseMode: TerminalBuffer.MouseMode = .none
    private var mouseEncoding: TerminalBuffer.MouseEncoding = .normal
    private var lastMouseLocation: (row: Int, col: Int)?
    private var lastElvisSnapshotText: String?
    private var lastElvisCursorOffset: Int?
    private(set) var resolvedFont: UIFont = TerminalFontSettings.shared.currentFont
    private var appearanceObserver: NSObjectProtocol?
    private var modifierObserver: NSObjectProtocol?
    private var selectionStartIndex: Int?
    private var selectionEndIndex: Int?
    private var selectionAnchorPoint: CGPoint?
    private var pendingUpdate: (text: NSAttributedString,
                                cursor: TerminalCursorInfo?,
                                backgroundColor: UIColor,
                                isElvisMode: Bool,
                                elvisSnapshot: ElvisSnapshot?)?
    private var customKeyCommands: [UIKeyCommand] = []
    private lazy var longPressRecognizer: UILongPressGestureRecognizer = {
        let recognizer = UILongPressGestureRecognizer(target: self, action: #selector(handleSelectionPress(_:)))
        recognizer.minimumPressDuration = 0.25
        recognizer.delegate = self
        return recognizer
    }()
    var onPaste: ((String) -> Void)? {
        didSet {
            terminalView.pasteHandler = onPaste
        }
    }

    override init(frame: CGRect) {
        super.init(frame: frame)
        clipsToBounds = true
        translatesAutoresizingMaskIntoConstraints = false
        terminalView.isEditable = false
        terminalView.isSelectable = false
        terminalView.isScrollEnabled = true
        terminalView.textContainerInset = .zero
        terminalView.textContainer.lineFragmentPadding = 0
        terminalView.contentInset = .zero
        terminalView.alwaysBounceVertical = true
        terminalView.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        terminalView.adjustsFontForContentSizeCategory = false
        terminalView.font = TerminalFontSettings.shared.currentFont
        terminalView.typingAttributes[.font] = terminalView.font
        terminalView.backgroundColor = TerminalFontSettings.shared.backgroundColor
        terminalView.textColor = TerminalFontSettings.shared.foregroundColor
        terminalView.cursorColor = TerminalFontSettings.shared.foregroundColor
        addSubview(terminalView)
        configureKeyCommands()

        commandIndicator.text = "⌘"
        commandIndicator.font = UIFont.systemFont(ofSize: 14, weight: .bold)
        commandIndicator.textColor = .white
        commandIndicator.backgroundColor = UIColor.black.withAlphaComponent(0.5)
        commandIndicator.textAlignment = .center
        commandIndicator.layer.cornerRadius = 4
        commandIndicator.layer.masksToBounds = true
        commandIndicator.isHidden = true
        commandIndicator.translatesAutoresizingMaskIntoConstraints = false
        addSubview(commandIndicator)
        NSLayoutConstraint.activate([
            commandIndicator.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -8),
            commandIndicator.topAnchor.constraint(equalTo: topAnchor, constant: 8),
            commandIndicator.widthAnchor.constraint(equalToConstant: 22),
            commandIndicator.heightAnchor.constraint(equalToConstant: 22)
        ])

        selectionOverlay.isUserInteractionEnabled = false
        selectionOverlay.backgroundColor = .clear
        selectionOverlay.textView = terminalView
        addSubview(selectionOverlay)
        addGestureRecognizer(longPressRecognizer)
        selectionMenu.translatesAutoresizingMaskIntoConstraints = false
        selectionMenu.isHidden = true
        selectionMenu.copyHandler = { [weak self] in self?.copySelectionAction() }
        selectionMenu.copyAllHandler = { [weak self] in self?.copyAllAction() }
        selectionMenu.pasteHandler = { [weak self] in self?.pasteSelectionAction() }
        addSubview(selectionMenu)
        appearanceObserver = NotificationCenter.default.addObserver(forName: TerminalFontSettings.appearanceDidChangeNotification,
                                                                    object: nil,
                                                                    queue: .main) { [weak self] _ in
            guard let self else { return }
            self.applyFont(font: TerminalFontSettings.shared.currentFont)
            self.configure(backgroundColor: TerminalFontSettings.shared.backgroundColor,
                           foregroundColor: TerminalFontSettings.shared.foregroundColor)
        }
        modifierObserver = NotificationCenter.default.addObserver(forName: .terminalModifierStateChanged,
                                                                  object: nil,
                                                                  queue: .main) { [weak self] notification in
            guard let commandDown = notification.userInfo?["command"] as? Bool else { return }
            self?.commandIndicator.isHidden = !commandDown
        }
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override var canBecomeFirstResponder: Bool { true }
    override var keyCommands: [UIKeyCommand]? { customKeyCommands }

    private func configureKeyCommands() {
        let increase1 = UIKeyCommand(input: "+", modifierFlags: [.command], action: #selector(handleIncreaseFont))
        let increase2 = UIKeyCommand(input: "=", modifierFlags: [.command], action: #selector(handleIncreaseFont))
        let decrease = UIKeyCommand(input: "-", modifierFlags: [.command], action: #selector(handleDecreaseFont))
        increase1.wantsPriorityOverSystemBehavior = true
        increase2.wantsPriorityOverSystemBehavior = true
        decrease.wantsPriorityOverSystemBehavior = true
        customKeyCommands = [increase1, increase2, decrease]
    }

    @objc
    private func handleIncreaseFont() {
        let current = TerminalFontSettings.shared.pointSize
        TerminalFontSettings.shared.updatePointSize(current + 1.0)
    }

    @objc
    private func handleDecreaseFont() {
        let current = TerminalFontSettings.shared.pointSize
        TerminalFontSettings.shared.updatePointSize(current - 1.0)
    }

    private func updateCommandIndicator(from event: UIPressesEvent) {
        let commandDown = event.modifierFlags.contains(.command)
        commandIndicator.isHidden = !commandDown
    }

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        if let event = event {
            updateCommandIndicator(from: event)
        }
        super.pressesBegan(presses, with: event)
    }

    override func pressesChanged(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        if let event = event {
            updateCommandIndicator(from: event)
        }
        super.pressesChanged(presses, with: event)
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        if let event = event {
            updateCommandIndicator(from: event)
        } else {
            commandIndicator.isHidden = true
        }
        super.pressesEnded(presses, with: event)
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        commandIndicator.isHidden = true
        super.pressesCancelled(presses, with: event)
    }

    override func becomeFirstResponder() -> Bool {
        return super.becomeFirstResponder()
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil {
            DispatchQueue.main.async { [weak self] in _ = self?.becomeFirstResponder() }
        }
    }

    deinit {
        if let observer = appearanceObserver {
            NotificationCenter.default.removeObserver(observer)
        }
        if let observer = modifierObserver {
            NotificationCenter.default.removeObserver(observer)
        }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        terminalView.frame = bounds
        selectionOverlay.frame = bounds
        if bounds.width > 1, bounds.height > 1, let pending = pendingUpdate {
            pendingUpdate = nil
            update(text: pending.text,
                   cursor: pending.cursor,
                   backgroundColor: pending.backgroundColor,
                   isElvisMode: pending.isElvisMode,
                   elvisSnapshot: pending.elvisSnapshot)
        }
    }

    func configure(backgroundColor: UIColor, foregroundColor: UIColor) {
        terminalView.backgroundColor = backgroundColor
        terminalView.textColor = foregroundColor
        terminalView.cursorColor = foregroundColor
        self.backgroundColor = backgroundColor
    }

    func applyFont(font: UIFont) {
        if let current = terminalView.font,
           current.fontName == font.fontName,
           abs(current.pointSize - font.pointSize) < 0.01 {
            return
        }
        terminalView.font = font
        terminalView.typingAttributes[.font] = font
        resolvedFont = font
    }

    func applyElvisFont(_ font: UIFont) {
        terminalView.adjustsFontForContentSizeCategory = false
        terminalView.font = font
        terminalView.typingAttributes[.font] = font
        resolvedFont = font
        selectionOverlay.clearSelection()
    }

    func currentFont() -> UIFont {
        return terminalView.font ?? resolvedFont
    }

    @objc private func handleSelectionPress(_ recognizer: UILongPressGestureRecognizer) {
        if mouseMode != .none { return }
        
        let location = recognizer.location(in: terminalView)
        let anchor = recognizer.location(in: self)
        switch recognizer.state {
        case .began:
            if let index = characterIndex(at: location) {
                selectionStartIndex = index
                selectionEndIndex = index
                selectionOverlay.updateSelection(start: index, end: index)
                terminalView.isScrollEnabled = false
                selectionAnchorPoint = anchor
            }
        case .changed:
            guard let start = selectionStartIndex else { break }
            if let index = characterIndex(at: location) {
                if selectionEndIndex != index {
                    selectionEndIndex = index
                    selectionOverlay.updateSelection(start: start, end: index)
                }
            }
        case .ended, .cancelled, .failed:
            terminalView.isScrollEnabled = true
            selectionAnchorPoint = anchor
            showSelectionMenu()
        default:
            break
        }
    }

    private func showSelectionMenu() {
        guard selectionOverlay.hasSelection else {
            hideSelectionMenu()
            return
        }
        selectionMenu.isPasteEnabled = UIPasteboard.general.hasStrings
        positionSelectionMenu()
        selectionMenu.isHidden = false
        bringSubviewToFront(selectionMenu)
    }

    private func hideSelectionMenu() {
        selectionMenu.isHidden = true
        selectionAnchorPoint = nil
    }

    private func characterIndex(at point: CGPoint) -> Int? {
        guard let text = terminalView.attributedText else { return nil }
        let location = CGPoint(x: point.x + terminalView.contentOffset.x - terminalView.textContainerInset.left,
                               y: point.y + terminalView.contentOffset.y - terminalView.textContainerInset.top)
        let characterIndex = terminalView.closestPosition(to: location).flatMap { pos in
            terminalView.offset(from: terminalView.beginningOfDocument, to: pos)
        } ?? 0
        let clamped = max(0, min(characterIndex, text.length))
        return clamped
    }

    @objc private func copySelectionAction() {
        guard let text = terminalView.attributedText,
              let range = selectionOverlay.currentSelectionRange() else {
            selectionOverlay.clearSelection()
            selectionStartIndex = nil
            selectionEndIndex = nil
            selectionAnchorPoint = nil
            return
        }
        let clamped = NSIntersectionRange(range, NSRange(location: 0, length: text.length))
        if clamped.length > 0 {
            let selectedText = text.attributedSubstring(from: clamped).string
            UIPasteboard.general.string = selectedText
            UIImpactFeedbackGenerator(style: .light).impactOccurred()
        }
        selectionOverlay.clearSelection()
        selectionStartIndex = nil
        selectionEndIndex = nil
        selectionAnchorPoint = nil
        hideSelectionMenu()
    }

    @objc private func copyAllAction() {
        if let text = terminalView.attributedText, text.length > 0 {
            UIPasteboard.general.string = text.string
            UIImpactFeedbackGenerator(style: .light).impactOccurred()
        }
        selectionOverlay.clearSelection()
        selectionStartIndex = nil
        selectionEndIndex = nil
        selectionAnchorPoint = nil
        hideSelectionMenu()
    }

    @objc private func pasteSelectionAction() {
        guard let onPaste = onPaste,
              let text = UIPasteboard.general.string,
              !text.isEmpty else {
            hideSelectionMenu()
            return
        }
        onPaste(text)
        hideSelectionMenu()
    }

    private func positionSelectionMenu() {
        guard let anchorRect = selectionOverlay.selectionBoundingRect(in: self) else {
            selectionMenu.isHidden = true
            return
        }
        selectionMenu.layoutIfNeeded()
        let targetSize = selectionMenu.systemLayoutSizeFitting(UIView.layoutFittingCompressedSize)
        let menuSize = CGSize(width: max(72, targetSize.width), height: max(32, targetSize.height))
        let preferredOrigin = CGPoint(x: anchorRect.maxX - menuSize.width,
                                      y: max(0, anchorRect.minY - menuSize.height - 8))
        let maxX = bounds.width - menuSize.width - 8
        let clampedX = max(8, min(preferredOrigin.x, maxX))
        let maxY = bounds.height - menuSize.height - 8
        let clampedY = max(8, min(preferredOrigin.y, maxY))
        selectionMenu.frame = CGRect(origin: CGPoint(x: clampedX, y: clampedY), size: menuSize)
    }

    func update(text: NSAttributedString,
                cursor: TerminalCursorInfo?,
                backgroundColor: UIColor,
                isElvisMode: Bool,
                elvisSnapshot: ElvisSnapshot?) {
        if !Thread.isMainThread {
            DispatchQueue.main.async {
                self.update(text: text,
                            cursor: cursor,
                            backgroundColor: backgroundColor,
                            isElvisMode: isElvisMode,
                            elvisSnapshot: elvisSnapshot)
            }
            return
        }
        if isElvisMode, let snapshot = elvisSnapshot {
            applyElvisSnapshot(snapshot, backgroundColor: backgroundColor)
            return
        } else {
            lastElvisSnapshotText = nil
            lastElvisCursorOffset = nil
            selectionOverlay.clearSelection()
            let displayText = remapFontsIfNeeded(in: text)
            if window == nil || bounds.width < 1 || bounds.height < 1 {
                pendingUpdate = (text, cursor, backgroundColor, isElvisMode, elvisSnapshot)
                return
            }
            terminalView.attributedText = displayText
            terminalView.cursorTextOffset = cursor?.textOffset
            terminalView.cursorInfo = cursor
            terminalView.backgroundColor = backgroundColor
            if let cursorOffset = cursor?.textOffset {
                scrollToCursor(textView: terminalView, cursorOffset: cursorOffset)
            } else {
                scrollToBottom(textView: terminalView, text: displayText)
            }
        }
    }

    private func applyElvisSnapshot(_ snapshot: ElvisSnapshot, backgroundColor: UIColor) {
        lastElvisSnapshotText = snapshot.text
        terminalView.backgroundColor = backgroundColor
        terminalView.textColor = TerminalFontSettings.shared.foregroundColor
        terminalView.cursorColor = TerminalFontSettings.shared.foregroundColor
        terminalView.attributedText = snapshot.attributedText
        lastElvisCursorOffset = nil
        selectionOverlay.clearSelection()

        var resolvedCursor: TerminalCursorInfo?
        var preferredInset: CGFloat = 0
        if let commandCursor = elvisCommandLineCursor(from: snapshot) {
            resolvedCursor = commandCursor
            preferredInset = Self.commandLinePadding
        } else if let cursor = snapshot.cursor {
            resolvedCursor = cursor
        }

        terminalView.cursorTextOffset = resolvedCursor?.textOffset
        terminalView.cursorInfo = resolvedCursor

        if let offset = resolvedCursor?.textOffset {
            if offset != lastElvisCursorOffset {
                if window == nil || bounds.width < 1 || bounds.height < 1 {
                    pendingUpdate = (snapshot.attributedText, resolvedCursor, backgroundColor, true, snapshot)
                    return
                }
                scrollToCursor(textView: terminalView,
                               cursorOffset: offset,
                               preferBottomInset: preferredInset)
                lastElvisCursorOffset = offset
            }
        } else {
            terminalView.cursorTextOffset = nil
            terminalView.cursorInfo = nil
            lastElvisCursorOffset = nil
            scrollToBottom(textView: terminalView, text: terminalView.attributedText)
        }
    }

    private func scrollToBottom(textView: UITextView, text: NSAttributedString) {
        textView.layoutIfNeeded()
        let currentLength = textView.attributedText.length
        if currentLength > 0 {
            let bottomRange = NSRange(location: max(0, currentLength - 1), length: 1)
            textView.scrollRangeToVisible(bottomRange)
        }
        let contentHeight = textView.contentSize.height
        let boundsHeight = textView.bounds.height
        let yOffset = max(-textView.contentInset.top, contentHeight - boundsHeight + textView.contentInset.bottom)
        if yOffset.isFinite {
            textView.setContentOffset(CGPoint(x: -textView.contentInset.left, y: yOffset), animated: false)
        }
    }

    private func scrollToCursor(textView: UITextView,
                                cursorOffset: Int,
                                preferBottomInset: CGFloat = 0) {
        textView.layoutIfNeeded()
        let storageLength = textView.attributedText.length
        if storageLength > 0 {
            textView.layoutManager.ensureLayout(forCharacterRange: NSRange(location: 0, length: storageLength))
            textView.layoutManager.ensureLayout(for: textView.textContainer)
        }
        let length = textView.attributedText.length
        let safeOffset = max(0, min(cursorOffset, length))
        let range = NSRange(location: safeOffset, length: 0)
        textView.scrollRangeToVisible(range)
    }

    private func remapFontsIfNeeded(in text: NSAttributedString) -> NSAttributedString {
        guard text.length > 0 else { return text }
        let baseFont = currentFont()

        var needsRemap = false
        let fullRange = NSRange(location: 0, length: text.length)
        text.enumerateAttribute(.font, in: fullRange, options: []) { value, _, stop in
            if let existing = value as? UIFont {
                if existing.familyName != baseFont.familyName {
                    needsRemap = true;
                    stop.pointee = true;
                }
            }
        }
        if !needsRemap {
            return text
        }

        let mutable = NSMutableAttributedString(attributedString: text)
        mutable.enumerateAttribute(.font, in: fullRange, options: []) { value, range, _ in
            let existing = (value as? UIFont) ?? baseFont
            let traits = existing.fontDescriptor.symbolicTraits
            let weight: UIFont.Weight = traits.contains(.traitBold) ? .bold : .regular
            let italic = traits.contains(.traitItalic)
            let replacement = TerminalFontSettings.shared.font(forPointSize: baseFont.pointSize,
                                                               weight: weight,
                                                               italic: italic)
            mutable.addAttribute(.font, value: replacement, range: range)
        }
        return mutable
    }
    
    func updateMouseState(mode: TerminalBuffer.MouseMode, encoding: TerminalBuffer.MouseEncoding) {
        self.mouseMode = mode
        self.mouseEncoding = encoding
        
        // If mouse is active, we might want to disable native scrolling so Drag works for Vim.
        // If mouse is .none, native scrolling (history) is allowed.
        terminalView.isScrollEnabled = (mode == .none)
    }

    private func elvisCommandLineCursor(from snapshot: ElvisSnapshot) -> TerminalCursorInfo? {
        let lines = snapshot.text.components(separatedBy: "\n")
        guard !lines.isEmpty else {
            return nil
        }
        var offsets: [Int] = []
        offsets.reserveCapacity(lines.count)
        var runningOffset = 0
        for (index, line) in lines.enumerated() {
            offsets.append(runningOffset)
            runningOffset += (line as NSString).length
            if index < lines.count - 1 {
                runningOffset += 1
            }
        }
        let totalLength = (snapshot.text as NSString).length
        for rowIndex in stride(from: lines.count - 1, through: 0, by: -1) {
            let line = lines[rowIndex]
            let trimmedLeading = line.drop(while: { $0 == " " })
            if trimmedLeading.isEmpty {
                continue
            }
            guard let first = trimmedLeading.first,
                  Self.commandLinePrefixes.contains(first) else {
                if line.trimmingCharacters(in: .whitespaces).isEmpty {
                    continue
                }
                break
            }
            let lastNonSpaceIndex = line.lastIndex(where: { $0 != " " }) ?? line.startIndex
            let visibleSubstring = line[line.startIndex...lastNonSpaceIndex]
            let columnUTF16 = String(visibleSubstring).utf16.count
            let textOffset = min(offsets[rowIndex] + columnUTF16, totalLength)
            return TerminalCursorInfo(row: rowIndex, column: columnUTF16, textOffset: textOffset)
        }
        return nil
    }
    
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard mouseMode != .none, let touch = touches.first else {
            super.touchesBegan(touches, with: event)
            return
        }
        let (col, row) = bufferCoordinate(from: touch.location(in: terminalView))
        lastMouseLocation = (row, col)
            
        // Button 0 (Left), Action 'M' (Press)
        sendSGRMouse(button: 0, x: col + 1, y: row + 1, pressed: true)
    }
    
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard mouseMode == .drag, let touch = touches.first else {
            super.touchesMoved(touches, with: event)
            return
        }
        let (col, row) = bufferCoordinate(from: touch.location(in: terminalView))
            
        // Filter noise: only send if cell changed
        if let last = lastMouseLocation, last.row == row, last.col == col {
            return
        }
        lastMouseLocation = (row, col)
            
        // Button 32 (Drag/Motion) + 0 (Left), Action 'M' (Press/Move)
        sendSGRMouse(button: 32, x: col + 1, y: row + 1, pressed: true)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard mouseMode != .none, let touch = touches.first else {
            super.touchesEnded(touches, with: event)
            return
        }
        let (col, row) = bufferCoordinate(from: touch.location(in: terminalView))
        lastMouseLocation = nil
            
        // Button 0 (Left), Action 'm' (Release)
        sendSGRMouse(button: 0, x: col + 1, y: row + 1, pressed: false)
    }

    private func bufferCoordinate(from point: CGPoint) -> (col: Int, row: Int) {
        let charWidth = TerminalFontMetrics.characterWidth
        let lineHeight = TerminalFontMetrics.lineHeight
            
        // Adjust for insets if any
        let x = point.x - terminalView.textContainerInset.left
        let y = point.y - terminalView.textContainerInset.top
            
        let col = Int(floor(x / charWidth))
        let row = Int(floor(y / lineHeight))
            
        return (max(0, col), max(0, row))
    }

    private func sendSGRMouse(button: Int, x: Int, y: Int, pressed: Bool) {
        guard mouseEncoding == .sgr else { return }
            
        // SGR Format: CSI < button ; x ; y M (or m)
        // M = Press/Move, m = Release
        let suffix = pressed ? "M" : "m"
        let sequence = "\u{1B}[<\(button);\(x);\(y)\(suffix)"
            
        PscalRuntimeBootstrap.shared.send(sequence)
    }

    private static let commandLinePrefixes: Set<Character> = [":", "/", "?"]
    private static let commandLinePadding: CGFloat = 8.0
}

final class TerminalDisplayTextView: UITextView {
    var cursorInfo: TerminalCursorInfo? {
        didSet { updateCursorLayer() }
    }
    var cursorTextOffset: Int? {
        didSet { updateCursorLayer() }
    }
    var pasteHandler: ((String) -> Void)?
    var cursorColor: UIColor = .systemOrange {
        didSet {
            cursorLayer.backgroundColor = cursorColor.cgColor
        }
    }

    private let cursorLayer: CALayer = {
        let layer = CALayer()
        layer.backgroundColor = UIColor.systemOrange.cgColor
        layer.opacity = 0
        return layer
    }()

    private var blinkAnimationAdded = false

    override init(frame: CGRect, textContainer: NSTextContainer?) {
        super.init(frame: frame, textContainer: textContainer)
        layer.addSublayer(cursorLayer)
        cursorLayer.backgroundColor = cursorColor.cgColor
        isEditable = false
        isSelectable = false
        isScrollEnabled = true
        textContainerInset = .zero
        backgroundColor = .clear
        isUserInteractionEnabled = true
        if #available(iOS 11.0, *) {
            self.textDragInteraction?.isEnabled = false
        }
        pruneGestures()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        updateCursorLayer()
    }

    override var canBecomeFirstResponder: Bool { false }

    override var contentOffset: CGPoint {
        didSet { updateCursorLayer() }
    }

    override func canPerformAction(_ action: Selector, withSender sender: Any?) -> Bool {
        if action == #selector(paste(_:)) {
            return pasteHandler != nil && UIPasteboard.general.hasStrings
        }
        return super.canPerformAction(action, withSender: sender)
    }

    override func paste(_ sender: Any?) {
        guard let handler = pasteHandler,
              let text = UIPasteboard.general.string,
              !text.isEmpty else { return }
        handler(text)
    }

    override func copy(_ sender: Any?) { }

    private func updateCursorLayer() {
        guard let offset = cursorTextOffset else {
            cursorLayer.opacity = 0
            return
        }

        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            guard let currentOffset = self.cursorTextOffset, currentOffset == offset else { return }
            
            guard self.attributedText.length > 0 else {
                self.cursorLayer.opacity = 0
                return
            }

            let clamped = max(0, min(offset, self.attributedText.length))
            guard let pos = self.position(from: self.beginningOfDocument, offset: clamped) else {
                return
            }

            let caret = self.caretRect(for: pos)
            var rect = caret
            rect.origin.x -= self.contentOffset.x
            rect.origin.y -= self.contentOffset.y
            rect.size.width = max(1, rect.size.width)
            rect.size.height = max(rect.size.height, TerminalFontMetrics.lineHeight)

            CATransaction.begin()
            CATransaction.setDisableActions(true)
            self.cursorLayer.frame = rect
            CATransaction.commit()

            if !self.blinkAnimationAdded {
                let animation = CABasicAnimation(keyPath: "opacity")
                animation.fromValue = 1
                animation.toValue = 0
                animation.duration = 0.8
                animation.autoreverses = true
                animation.repeatCount = .infinity
                self.cursorLayer.add(animation, forKey: "blink")
                self.blinkAnimationAdded = true
            }

            self.cursorLayer.opacity = 1
        }
    }

    private func pruneGestures() {
        gestureRecognizers?.forEach { recognizer in
            if recognizer is UIPanGestureRecognizer {
                recognizer.isEnabled = true
            } else {
                recognizer.isEnabled = false
            }
        }
    }
}

final class TerminalSelectionOverlay: UIView {
    weak var textView: UITextView?
    private var selectionRange: NSRange?
    var hasSelection: Bool {
        guard let range = selectionRange else { return false }
        return range.length > 0
    }

    func currentSelectionRange() -> NSRange? {
        return selectionRange
    }

    func selectionBoundingRect(in view: UIView) -> CGRect? {
        guard let textView else { return nil }
        return selectionBoundingRect(for: selectionRange).flatMap { convert($0, from: textView) }
    }

    func updateSelection(start: Int, end: Int) {
        let lower = min(start, end)
        let upper = max(start, end)
        let newRange = NSRange(location: lower, length: upper - lower + 1)
        let oldRect = selectionBoundingRect(for: selectionRange)
        let newRect = selectionBoundingRect(for: newRange)
        selectionRange = newRange
        if let dirty = union(rectA: oldRect, rectB: newRect) {
            setNeedsDisplay(dirty.insetBy(dx: -2, dy: -2))
        } else {
            setNeedsDisplay()
        }
    }

    func clearSelection() {
        if let range = selectionRange {
            selectionRange = nil
            if let rect = selectionBoundingRect(for: range) {
                setNeedsDisplay(rect.insetBy(dx: -2, dy: -2))
            } else {
                setNeedsDisplay()
            }
        }
    }

    override func draw(_ rect: CGRect) {
        guard let textView = textView,
              let layoutManager = textView.layoutManager as NSLayoutManager?,
              let textContainer = textView.textContainer as NSTextContainer?,
              let selection = selectionRange,
              selection.length > 0 else {
            return
        }

        let selectionColor = UIColor.systemBlue.withAlphaComponent(0.25)
        let inset = textView.textContainerInset
        let context = UIGraphicsGetCurrentContext()
        context?.setFillColor(selectionColor.cgColor)

        var actualCharRange = NSRange()
        let glyphRange = layoutManager.glyphRange(forCharacterRange: selection, actualCharacterRange: &actualCharRange)

        layoutManager.enumerateLineFragments(forGlyphRange: glyphRange) { (_, usedRect, _, glyphRangeForLine, _) in
            let intersection = NSIntersectionRange(glyphRangeForLine, glyphRange)
            if intersection.length == 0 {
                return
            }
            let highlightRect = layoutManager.boundingRect(forGlyphRange: intersection, in: textContainer)
            var drawRect = highlightRect
            drawRect.origin.x += inset.left - textView.contentOffset.x
            drawRect.origin.y += inset.top - textView.contentOffset.y
            drawRect = drawRect.integral.insetBy(dx: -1, dy: -1)
            context?.fill(drawRect)
        }
    }

    private func selectionBoundingRect(for range: NSRange?) -> CGRect? {
        guard let range = range,
              let textView = textView,
              let layoutManager = textView.layoutManager as NSLayoutManager?,
              let textContainer = textView.textContainer as NSTextContainer?,
              range.length > 0 else {
            return nil
        }
        let inset = textView.textContainerInset
        let glyphRange = layoutManager.glyphRange(forCharacterRange: range, actualCharacterRange: nil)
        var rect = layoutManager.boundingRect(forGlyphRange: glyphRange, in: textContainer)
        rect.origin.x += inset.left - textView.contentOffset.x
        rect.origin.y += inset.top - textView.contentOffset.y
        return rect
    }

    private func union(rectA: CGRect?, rectB: CGRect?) -> CGRect? {
        switch (rectA, rectB) {
        case let (.some(a), .some(b)):
            return a.union(b)
        case let (.some(a), .none):
            return a
        case let (.none, .some(b)):
            return b
        default:
            return nil
        }
    }
}

final class TerminalSelectionMenuView: UIView {
    var copyHandler: (() -> Void)?
    var copyAllHandler: (() -> Void)?
    var pasteHandler: (() -> Void)?
    var isPasteEnabled: Bool = true {
        didSet {
            pasteButton.isEnabled = isPasteEnabled
            pasteButton.alpha = isPasteEnabled ? 1.0 : 0.5
        }
    }

    private let stack = UIStackView()
    private let copyButton: UIButton = {
        let button = UIButton(type: .system)
        button.setTitle("Copy", for: .normal)
        button.titleLabel?.font = UIFont.systemFont(ofSize: 14, weight: .semibold)
        return button
    }()
    private let copyAllButton: UIButton = {
        let button = UIButton(type: .system)
        button.setTitle("All", for: .normal)
        button.titleLabel?.font = UIFont.systemFont(ofSize: 14, weight: .semibold)
        return button
    }()
    private let pasteButton: UIButton = {
        let button = UIButton(type: .system)
        button.setTitle("Paste", for: .normal)
        button.titleLabel?.font = UIFont.systemFont(ofSize: 14, weight: .semibold)
        return button
    }()

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = UIColor.secondarySystemBackground.withAlphaComponent(0.9)
        layer.cornerRadius = 8
        layer.masksToBounds = true
        stack.axis = .horizontal
        stack.spacing = 4
        stack.alignment = .fill
        stack.distribution = .fillEqually
        addSubview(stack)
        stack.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 4),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -4),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: 4),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -4)
        ])
        copyButton.addTarget(self, action: #selector(didTapCopy), for: .touchUpInside)
        copyAllButton.addTarget(self, action: #selector(didTapCopyAll), for: .touchUpInside)
        pasteButton.addTarget(self, action: #selector(didTapPaste), for: .touchUpInside)
        stack.addArrangedSubview(copyButton)
        stack.addArrangedSubview(copyAllButton)
        stack.addArrangedSubview(pasteButton)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func sizeToFit() {
        super.sizeToFit()
        layoutIfNeeded()
    }

    @objc private func didTapCopy() {
        copyHandler?()
    }

    @objc private func didTapCopyAll() {
        copyAllHandler?()
    }

    @objc private func didTapPaste() {
        if isPasteEnabled {
            pasteHandler?()
        }
    }
}

struct TerminalSettingsView: View {
    @ObservedObject private var settings = TerminalFontSettings.shared
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        content
    }

    @ViewBuilder
    private var content: some View {
        NavigationView {
            Form {
                Section(header: Text("Font")) {
                    Picker("Font",
                           selection: Binding(get: {
                        settings.selectedFontID
                    }, set: { newValue in
                        settings.updateFontSelection(newValue)
                    })) {
                        ForEach(settings.fontOptions) { option in
                            Text(option.displayName).tag(option.id)
                        }
                    }
                    let previewFont = settings.font(forPointSize: 16)
                    Text("Sample AaBb123")
                        .font(Font(previewFont))
                        .foregroundColor(Color(settings.foregroundColor))
                }
                Section(header: Text("Font Size")) {
                    Slider(value: Binding(get: {
                        Double(settings.pointSize)
                    }, set: { newValue in
                        settings.updatePointSize(CGFloat(newValue))
                    }), in: Double(settings.minimumPointSize)...Double(settings.maximumPointSize), step: 1)
                    HStack {
                        Text("Current")
                        Spacer()
                        Text("\(Int(settings.pointSize)) pt")
                            .font(.system(.body, design: .monospaced))
                    }
                }
                Section(header: Text("Colors")) {
                    ColorPicker("Background",
                                selection: Binding(get: {
                        Color(settings.backgroundColor)
                    }, set: { newValue in
                        settings.updateBackgroundColor(UIColor(swiftUIColor: newValue))
                    }))
                    ColorPicker("Foreground",
                                selection: Binding(get: {
                        Color(settings.foregroundColor)
                    }, set: { newValue in
                        settings.updateForegroundColor(UIColor(swiftUIColor: newValue))
                    }))
                }
                Section(header: Text("Filesystem Paths")) {
                    Toggle("Present sandbox as /",
                           isOn: Binding(get: {
                        settings.pathTruncationEnabled
                    }, set: { newValue in
                        settings.updatePathTruncationEnabled(newValue)
                    }))
                    Text("Requires restarting the PSCAL runtime to take effect.")
                        .font(.footnote)
                        .foregroundColor(.secondary)
                    TextField("PATH_TRUNCATE",
                              text: Binding(get: {
                        settings.pathTruncationPath
                    }, set: { newValue in
                        settings.updatePathTruncationPath(newValue)
                    }))
                    .textInputAutocapitalization(.none)
                    .autocorrectionDisabled(true)
                    .font(.system(.body, design: .monospaced))
                    .disabled(!settings.pathTruncationEnabled)
                    Button("Use Documents root") {
                        settings.useDocumentsPathForTruncation()
                    }
                    .disabled(!settings.pathTruncationEnabled)
                    Text(settings.pathTruncationEnabled ?
                         "Absolute paths map to \(settings.pathTruncationPath.isEmpty ? "Documents" : settings.pathTruncationPath)." :
                            "PATH_TRUNCATE is disabled; absolute paths reflect the full sandbox path.")
                    .font(.footnote)
                    .foregroundColor(.secondary)
                }
            }
            .navigationTitle("Terminal Settings")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .applyDetents()
    }
}

struct PathTruncationPreferences {
    static let enabledDefaultsKey = "com.pscal.terminal.pathTruncateEnabled"
    static let pathDefaultsKey = "com.pscal.terminal.pathTruncateValue"
}

final class PathTruncationManager {
    static let shared = PathTruncationManager()

    private init() {}

    func normalize(_ path: String) -> String {
        let trimmed = path.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty {
            return ""
        }
        let absolute = trimmed.hasPrefix("/") ? trimmed : "/" + trimmed
        let resolved = URL(fileURLWithPath: absolute).resolvingSymlinksInPath().path
        var normalized = resolved
        if normalized.isEmpty {
            return ""
        }
        if normalized == "/" {
            return normalized
        }
        while normalized.count > 1 && normalized.hasSuffix("/") {
            normalized.removeLast()
        }
        return normalized
    }

    func defaultDocumentsPath() -> String {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
        return normalize(docs?.path ?? "")
    }

    func apply(enabled: Bool, path: String) {
        if enabled {
            let normalized = normalize(path)
            if !normalized.isEmpty {
                withCStringPointer(normalized) { PSCALRuntimeApplyPathTruncation($0) }
                GPSDeviceProvider.shared.updateTargetRoot(normalized)
                let tmpURL = URL(fileURLWithPath: normalized, isDirectory: true)
                    .appendingPathComponent("tmp", isDirectory: true)
                try? FileManager.default.createDirectory(at: tmpURL,
                                                         withIntermediateDirectories: true,
                                                         attributes: nil)
                return
            }
        }
        PSCALRuntimeApplyPathTruncation(nil)
        GPSDeviceProvider.shared.updateTargetRoot(nil)
    }

    func applyStoredPreference() {
        let defaults = UserDefaults.standard
        let storedPath = defaults.string(forKey: PathTruncationPreferences.pathDefaultsKey)
        let normalizedPath: String
        if let storedPath, !storedPath.isEmpty {
            normalizedPath = normalize(storedPath)
        } else {
            normalizedPath = normalize(defaultDocumentsPath())
        }
        let enabled: Bool
        if defaults.object(forKey: PathTruncationPreferences.enabledDefaultsKey) != nil {
            enabled = defaults.bool(forKey: PathTruncationPreferences.enabledDefaultsKey)
        } else {
            enabled = false
        }
        apply(enabled: enabled, path: normalizedPath)
    }
}

struct EditorFloatingRendererView: View {
    @ObservedObject private var fontSettings = TerminalFontSettings.shared
    @ObservedObject private var runtime = PscalRuntimeBootstrap.shared

    var body: some View {
        let token = runtime.elvisRenderToken
        _ = token
        let snapshot = EditorTerminalBridge.shared.snapshot()
        
        return TerminalRendererView(text: snapshot.attributedText,
                                    cursor: snapshot.cursor,
                                    backgroundColor: fontSettings.backgroundColor,
                                    foregroundColor: fontSettings.foregroundColor,
                                    isElvisMode: true,
                                    isElvisWindowVisible: false,
                                    elvisRenderToken: token,
                                    font: fontSettings.currentFont,
                                    fontPointSize: fontSettings.pointSize,
                                    elvisSnapshot: snapshot,
                                    // ADDED: Pass mouse state from runtime
                                    mouseMode: runtime.mouseMode,
                                    mouseEncoding: runtime.mouseEncoding)
            .background(Color(fontSettings.backgroundColor))
    }
}

@available(iOS 16.0, *)
private struct DetentModifier: ViewModifier {
    func body(content: Content) -> some View {
        content.presentationDetents([.medium, .large])
    }
}

private struct LegacyDetentModifier: ViewModifier {
    func body(content: Content) -> some View {
        content
    }
}

private extension View {
    func applyDetents() -> some View {
        if #available(iOS 16.0, *) {
            return AnyView(self.modifier(DetentModifier()))
        } else {
            return AnyView(self.modifier(LegacyDetentModifier()))
        }
    }
}

private extension UIColor {
    convenience init(swiftUIColor: Color) {
        if let cgColor = swiftUIColor.cgColor {
            self.init(cgColor: cgColor)
        } else {
            self.init(red: 1, green: 1, blue: 1, alpha: 1)
        }
    }
}
