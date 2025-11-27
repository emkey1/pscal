import SwiftUI
import UIKit
import Combine
import QuartzCore
import CoreText
import Darwin

@_silgen_name("pscalRuntimeDebugLog")
private func c_terminalDebugLog(_ message: UnsafePointer<CChar>) -> Void

private func terminalViewLog(_ message: String) {
    message.withCString { c_terminalDebugLog($0) }
}

final class TerminalFontSettings: ObservableObject {
    struct FontOption: Identifiable, Equatable {
        let id: String
        let displayName: String
        let postScriptName: String?
    }

    static let shared = TerminalFontSettings()
    static let appearanceDidChangeNotification = Notification.Name("TerminalFontSettingsAppearanceDidChange")
    static let preferencesDidChangeNotification = Notification.Name("TerminalPreferencesDidChange")

    private static let minPointSizeValue: CGFloat = 6.0
    private static let maxPointSizeValue: CGFloat = 28.0
    private let storageKey = "com.pscal.terminal.fontPointSize"
    private let fontNameKey = "com.pscal.terminal.fontName"
    private let backgroundKey = "com.pscal.terminal.backgroundColor"
    private let foregroundKey = "com.pscal.terminal.foregroundColor"
    private let elvisWindowKey = "com.pscal.terminal.elvisWindow"
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
        elvisWindowEnabled = TerminalFontSettings.resolveInitialElvisWindowSetting(stored: storedElvisPref,
                                                                                   envValue: envWindowMode)

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
        if weight == .bold {
            traits.insert(.traitBold)
        } else {
            traits.remove(.traitBold)
        }
        if italic {
            traits.insert(.traitItalic)
        } else {
            traits.remove(.traitItalic)
        }
        if let descriptor = base.fontDescriptor.withSymbolicTraits(traits) {
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
        if let stored = stored {
            return stored
        }
        guard let env = envValue?.trimmingCharacters(in: .whitespacesAndNewlines).lowercased(),
              !env.isEmpty else {
            return true
        }
        if ["inline", "main", "primary", "embedded", "0", "false", "no"].contains(env) {
            return false
        }
        if ["window", "external", "secondary", "1", "true", "yes"].contains(env) {
            return true
        }
        return true
    }

    func updateElvisWindowEnabled(_ enabled: Bool) {
        guard enabled != elvisWindowEnabled else { return }
        elvisWindowEnabled = enabled
        UserDefaults.standard.set(enabled, forKey: elvisWindowKey)
        NotificationCenter.default.post(name: TerminalFontSettings.preferencesDidChangeNotification, object: nil)
    }
}

struct TerminalView: View {
    @ObservedObject private var fontSettings = TerminalFontSettings.shared
    @State private var showingSettings = false
    @State private var focusAnchor: Int = 0

    var body: some View {
        KeyboardAwareContainer(
            content: GeometryReader { proxy in
                TerminalContentView(availableSize: proxy.size,
                                    safeAreaInsets: proxy.safeAreaInsets,
                                    fontSettings: fontSettings,
                                    focusAnchor: $focusAnchor)
                    .frame(width: proxy.size.width, height: proxy.size.height)
            }
        )
        .edgesIgnoringSafeArea(.bottom)
        .overlay(alignment: .topTrailing) {
            VStack(alignment: .trailing, spacing: 8) {
                Button(action: {
                    UIPasteboard.general.string = PscalRuntimeBootstrap.shared.currentScreenText()
                    focusAnchor &+= 1
                }) {
                    Image(systemName: "doc.on.doc")
                        .font(.system(size: 16, weight: .semibold))
                        .padding(8)
                        .background(.ultraThinMaterial, in: Circle())
                }
                .accessibilityLabel("Copy Screen")

                Button(action: { showingSettings = true }) {
                    Image(systemName: "textformat.size")
                        .font(.system(size: 16, weight: .semibold))
                        .padding(8)
                        .background(.ultraThinMaterial, in: Circle())
                }
                .accessibilityLabel("Adjust Font Size")
            }
            .padding()
        }
        .sheet(isPresented: $showingSettings) {
            TerminalSettingsView()
        }
        .background(Color(.systemBackground))
    }
}

private struct TerminalContentView: View {
    private static let topPadding: CGFloat = 32.0
    let availableSize: CGSize
    let safeAreaInsets: EdgeInsets
    @ObservedObject private var fontSettings: TerminalFontSettings
    @ObservedObject private var runtime = PscalRuntimeBootstrap.shared
    @Binding private var focusAnchor: Int
    @State private var lastLoggedMetrics: TerminalGeometryMetrics?

    init(availableSize: CGSize,
         safeAreaInsets: EdgeInsets,
         fontSettings: TerminalFontSettings,
         focusAnchor: Binding<Int>) {
        self.availableSize = availableSize
        self.safeAreaInsets = safeAreaInsets
        _fontSettings = ObservedObject(wrappedValue: fontSettings)
        _focusAnchor = focusAnchor
    }

    var body: some View {
        let elvisToken = runtime.elvisRenderToken
        let elvisActive = runtime.isElvisModeActive()
        let elvisVisible = ElvisWindowManager.shared.isVisible
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
                                 onPaste: handlePaste)
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
            if !ElvisWindowManager.shared.isVisible {
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
            focusAnchor &+= 1
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
        guard let metrics = TerminalGeometryCalculator.metrics(for: availableSize,
                                                               safeAreaInsets: safeAreaInsets,
                                                               topPadding: Self.topPadding,
                                                               showingStatus: showingStatus,
                                                               font: font)
                ?? TerminalGeometryCalculator.fallbackMetrics(showingStatus: showingStatus, font: font) else {
            return
        }
        if lastLoggedMetrics != metrics {
            let char = TerminalGeometryCalculator.characterMetrics(for: font)
            let usable = TerminalGeometryCalculator.usableDimensions(for: availableSize,
                                                                     safeAreaInsets: safeAreaInsets,
                                                                     topPadding: Self.topPadding,
                                                                     showingStatus: showingStatus)
            terminalViewLog(String(format: "[TerminalView] available %.1fx%.1f safe(top=%.1f bottom=%.1f leading=%.1f trailing=%.1f) usable %.1fx%.1f font=%@ %.2fpt char(%.2f x %.2f) -> rows=%d cols=%d",
                                   availableSize.width,
                                   availableSize.height,
                                   safeAreaInsets.top,
                                   safeAreaInsets.bottom,
                                   safeAreaInsets.leading,
                                   safeAreaInsets.trailing,
                                   usable.width,
                                   usable.height,
                                   font.fontName,
                                   font.pointSize,
                                   char.width,
                                   char.lineHeight,
                                   metrics.rows,
                                   metrics.columns))
            lastLoggedMetrics = metrics
        }
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

enum TerminalGeometryCalculator {
    private static let horizontalPadding: CGFloat = 0.0
    private static let verticalRowPadding: CGFloat = 0.0
    private static let statusOverlayHeight: CGFloat = 32.0
    private static let dividerHeight: CGFloat = 1.0 / UIScreen.main.scale

    static func characterMetrics(for font: UIFont) -> (width: CGFloat, lineHeight: CGFloat) {
        let width = max(1.0, ("W" as NSString).size(withAttributes: [.font: font]).width)
        let height = font.lineHeight + verticalRowPadding
        return (width, height)
    }

    static func usableDimensions(for size: CGSize,
                                 safeAreaInsets: EdgeInsets,
                                 topPadding: CGFloat,
                                 showingStatus: Bool) -> (width: CGFloat, height: CGFloat) {
        let leadingInset = max(0, safeAreaInsets.leading)
        let trailingInset = max(0, safeAreaInsets.trailing)
        let topInset = max(0, safeAreaInsets.top) + max(0, topPadding)
        let bottomInset = max(0, safeAreaInsets.bottom)

        let width = max(0, size.width - horizontalPadding - leadingInset - trailingInset)
        var height = max(0, size.height - topInset - bottomInset)
        if showingStatus {
            height -= statusOverlayHeight
        }
        height -= dividerHeight
        return (width, height)
    }

    static func metrics(for size: CGSize,
                        safeAreaInsets: EdgeInsets,
                        topPadding: CGFloat,
                        showingStatus: Bool,
                        font: UIFont) -> TerminalGeometryMetrics? {
        guard size.width > 0, size.height > 0 else { return nil }

        let char = characterMetrics(for: font)
        let usable = usableDimensions(for: size,
                                      safeAreaInsets: safeAreaInsets,
                                      topPadding: topPadding,
                                      showingStatus: showingStatus)

        let rawColumns = Int(floor(usable.width / char.width))
        let rawRows = Int(floor(usable.height / char.lineHeight))
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
    var onPaste: ((String) -> Void)? = nil

    func makeUIView(context: Context) -> TerminalRendererContainerView {
        let container = TerminalRendererContainerView()
        container.configure(backgroundColor: backgroundColor, foregroundColor: foregroundColor)
        container.applyFont(font: font)
        container.onPaste = onPaste
        return container
    }

    func updateUIView(_ uiView: TerminalRendererContainerView, context: Context) {
        _ = elvisRenderToken
        uiView.configure(backgroundColor: backgroundColor, foregroundColor: foregroundColor)
        uiView.applyFont(font: font)
        uiView.onPaste = onPaste
        let externalWindowEnabled = ElvisWindowManager.externalWindowEnabled
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
        let snapshot = shouldUseSnapshot ? ElvisTerminalBridge.shared.snapshot() : nil
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
    private var lastElvisSnapshotText: String?
    private var lastElvisCursorOffset: Int?
    private(set) var resolvedFont: UIFont = TerminalFontSettings.shared.currentFont
    private var appearanceObserver: NSObjectProtocol?
    private var selectionStartIndex: Int?
    private var selectionEndIndex: Int?
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
        terminalView.isEditable = false
        terminalView.isSelectable = false
        terminalView.isScrollEnabled = true
        terminalView.textContainerInset = .zero
        terminalView.textContainer.lineFragmentPadding = 0
        terminalView.alwaysBounceVertical = true
        terminalView.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        terminalView.adjustsFontForContentSizeCategory = false
        terminalView.font = TerminalFontSettings.shared.currentFont
        terminalView.typingAttributes[.font] = terminalView.font
        terminalView.backgroundColor = TerminalFontSettings.shared.backgroundColor
        terminalView.textColor = TerminalFontSettings.shared.foregroundColor
        terminalView.cursorColor = TerminalFontSettings.shared.foregroundColor
        addSubview(terminalView)

        selectionOverlay.isUserInteractionEnabled = false
        selectionOverlay.backgroundColor = .clear
        selectionOverlay.textView = terminalView
        addSubview(selectionOverlay)
        addGestureRecognizer(longPressRecognizer)

        appearanceObserver = NotificationCenter.default.addObserver(forName: TerminalFontSettings.appearanceDidChangeNotification,
                                                                    object: nil,
                                                                    queue: .main) { [weak self] _ in
            guard let self else { return }
            self.applyFont(font: TerminalFontSettings.shared.currentFont)
            self.configure(backgroundColor: TerminalFontSettings.shared.backgroundColor,
                           foregroundColor: TerminalFontSettings.shared.foregroundColor)
        }
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    deinit {
        if let observer = appearanceObserver {
            NotificationCenter.default.removeObserver(observer)
        }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        terminalView.frame = bounds
        selectionOverlay.frame = bounds
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
        let location = recognizer.location(in: terminalView)
        switch recognizer.state {
        case .began:
            if let index = characterIndex(at: location) {
                selectionStartIndex = index
                selectionEndIndex = index
                selectionOverlay.updateSelection(start: index, end: index)
                terminalView.isScrollEnabled = false
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
            finalizeSelectionCopy()
        default:
            break
        }
    }

    private func finalizeSelectionCopy() {
        guard let start = selectionStartIndex,
              let end = selectionEndIndex,
              let text = terminalView.attributedText else {
            selectionOverlay.clearSelection()
            selectionStartIndex = nil
            selectionEndIndex = nil
            return
        }
        let lower = max(0, min(start, end))
        let upper = min(text.length, max(start, end))
        guard upper > lower else {
            selectionOverlay.clearSelection()
            selectionStartIndex = nil
            selectionEndIndex = nil
            return
        }
        let range = NSRange(location: lower, length: upper - lower)
        let selectedText = text.attributedSubstring(from: range).string
        if !selectedText.isEmpty {
            UIPasteboard.general.string = selectedText
            UIImpactFeedbackGenerator(style: .light).impactOccurred()
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
            self.selectionOverlay.clearSelection()
        }
        selectionStartIndex = nil
        selectionEndIndex = nil
    }

    private func characterIndex(at point: CGPoint) -> Int? {
        guard let text = terminalView.attributedText else { return nil }
        let layout = terminalView.layoutManager
        let container = terminalView.textContainer
        var adjusted = point
        adjusted.x -= terminalView.textContainerInset.left
        adjusted.y -= terminalView.textContainerInset.top
        adjusted.x += terminalView.contentOffset.x
        adjusted.y += terminalView.contentOffset.y
        let glyphIndex = layout.glyphIndex(for: adjusted, in: container)
        let charIndex = layout.characterIndexForGlyph(at: glyphIndex)
        if charIndex >= text.length {
            return text.length
        }
        return charIndex
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
            let displayText = remapFontsIfNeeded(in: text)
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
        if lastElvisSnapshotText != snapshot.text {
            lastElvisSnapshotText = snapshot.text
            terminalView.text = snapshot.text
            lastElvisCursorOffset = nil
            selectionOverlay.clearSelection()
        }
        terminalView.backgroundColor = backgroundColor
        terminalView.textColor = TerminalFontSettings.shared.foregroundColor
        terminalView.cursorColor = TerminalFontSettings.shared.foregroundColor

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
        let length = textView.attributedText.length
        let safeOffset = max(0, min(cursorOffset, length))
        let beginning = textView.beginningOfDocument
        guard let position = textView.position(from: beginning, offset: safeOffset) else {
            let fallbackRange = NSRange(location: max(0, min(safeOffset, max(length - 1, 0))), length: 0)
            textView.scrollRangeToVisible(fallbackRange)
            return
        }
        let caret = textView.caretRect(for: position)
        var newOffset = textView.contentOffset
        let topVisible = newOffset.y + textView.textContainerInset.top
        let bottomVisible = newOffset.y + textView.bounds.height - textView.textContainerInset.bottom
        var needsUpdate = false
        if caret.minY < topVisible {
            newOffset.y = caret.minY - textView.textContainerInset.top - preferBottomInset
            needsUpdate = true
        } else if caret.maxY > (bottomVisible - preferBottomInset) {
            newOffset.y = caret.maxY - textView.bounds.height + textView.textContainerInset.bottom + preferBottomInset
            needsUpdate = true
        }
        let minOffsetY = -textView.contentInset.top
        let maxOffsetY = max(minOffsetY, textView.contentSize.height - textView.bounds.height + textView.contentInset.bottom)
        newOffset.y = min(max(newOffset.y, minOffsetY), maxOffsetY)
        if needsUpdate {
            textView.setContentOffset(newOffset, animated: false)
        }
    }

    private func remapFontsIfNeeded(in text: NSAttributedString) -> NSAttributedString {
        guard text.length > 0 else { return text }
        let mutable = NSMutableAttributedString(attributedString: text)
        let baseFont = currentFont()
        let fullRange = NSRange(location: 0, length: mutable.length)
        mutable.enumerateAttribute(.font, in: fullRange, options: []) { value, range, _ in
            let existing = (value as? UIFont) ?? baseFont
            let traits = existing.fontDescriptor.symbolicTraits
            let weight: UIFont.Weight = traits.contains(.traitBold) ? .bold : .regular
            let italic = traits.contains(.traitItalic)
            let replacement = TerminalFontSettings.shared.font(forPointSize: existing.pointSize,
                                                               weight: weight,
                                                               italic: italic)
            mutable.addAttribute(.font, value: replacement, range: range)
        }
        return mutable
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
        let clampedOffset = max(0, min(offset, attributedText.length))
        let beginning = beginningOfDocument
        guard let position = self.position(from: beginning, offset: clampedOffset) else {
            cursorLayer.opacity = 0
            return
        }
        let caret = caretRect(for: position)
        var rect = caret
        rect.origin.x -= contentOffset.x
        rect.origin.y -= contentOffset.y
        rect.size.width = max(1, rect.size.width)
        rect.size.height = max(rect.size.height, TerminalFontMetrics.lineHeight)
        cursorLayer.frame = rect

        if !blinkAnimationAdded {
            let animation = CABasicAnimation(keyPath: "opacity")
            animation.fromValue = 1
            animation.toValue = 0
            animation.duration = 0.8
            animation.autoreverses = true
            animation.repeatCount = .infinity
            cursorLayer.add(animation, forKey: "blink")
            blinkAnimationAdded = true
        }

        cursorLayer.opacity = 1
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

    func updateSelection(start: Int, end: Int) {
        let lower = min(start, end)
        let upper = max(start, end)
        selectionRange = NSRange(location: lower, length: upper - lower + 1)
        setNeedsDisplay()
    }

    func clearSelection() {
        if selectionRange != nil {
            selectionRange = nil
            setNeedsDisplay()
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
                Section(header: Text("elvis/vi")) {
                    Toggle("elvis/vi",
                           isOn: Binding(get: {
                        settings.elvisWindowEnabled
                    }, set: { newValue in
                        settings.updateElvisWindowEnabled(newValue)
                    }))
                    Text("Show the elvis editor in a floating window.")
                        .font(.footnote)
                        .foregroundColor(.secondary)
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
                normalized.withCString { cString in
                    PSCALRuntimeApplyPathTruncation(cString)
                }
                return
            }
        }
        PSCALRuntimeApplyPathTruncation(nil)
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

struct ElvisFloatingRendererView: View {
    @ObservedObject private var fontSettings = TerminalFontSettings.shared
    @ObservedObject private var runtime = PscalRuntimeBootstrap.shared

    var body: some View {
        let token = runtime.elvisRenderToken
        _ = token
        let snapshot = ElvisTerminalBridge.shared.snapshot()
        return TerminalRendererView(text: NSAttributedString(string: snapshot.text),
                                    cursor: snapshot.cursor,
                                    backgroundColor: fontSettings.backgroundColor,
                                    foregroundColor: fontSettings.foregroundColor,
                                    isElvisMode: true,
                                    isElvisWindowVisible: false,
                                    elvisRenderToken: token,
                                    font: fontSettings.currentFont,
                                    fontPointSize: fontSettings.pointSize)
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
