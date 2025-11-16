import SwiftUI
import UIKit
import Combine
import QuartzCore

final class TerminalFontSettings: ObservableObject {
    static let shared = TerminalFontSettings()
    static let appearanceDidChangeNotification = Notification.Name("TerminalFontSettingsAppearanceDidChange")

    private let storageKey = "com.pscal.terminal.fontPointSize"
    private let backgroundKey = "com.pscal.terminal.backgroundColor"
    private let foregroundKey = "com.pscal.terminal.foregroundColor"
    let minimumPointSize: CGFloat = 10.0
    let maximumPointSize: CGFloat = 28.0

    @Published var pointSize: CGFloat = 14.0
    @Published var backgroundColor: UIColor = .systemBackground
    @Published var foregroundColor: UIColor = .label

    private init() {
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

        pointSize = clamp(initialPointSize)
        backgroundColor = loadColor(key: backgroundKey, fallback: .systemBackground)
        foregroundColor = loadColor(key: foregroundKey, fallback: .label)
    }

    func clamp(_ size: CGFloat) -> CGFloat {
        min(max(size, minimumPointSize), maximumPointSize)
    }

    var currentFont: UIFont {
        UIFont.monospacedSystemFont(ofSize: pointSize, weight: .regular)
    }

    func updatePointSize(_ newSize: CGFloat) {
        let clamped = clamp(newSize)
        guard clamped != pointSize else { return }
        pointSize = clamped
        UserDefaults.standard.set(Double(clamped), forKey: storageKey)
        notifyChange()
    }

    func updateBackgroundColor(_ color: UIColor) {
        let normalized = normalize(color)
        guard normalized != backgroundColor else { return }
        backgroundColor = normalized
        store(color: normalized, key: backgroundKey)
        notifyChange()
    }

    func updateForegroundColor(_ color: UIColor) {
        let normalized = normalize(color)
        guard normalized != foregroundColor else { return }
        foregroundColor = normalized
        store(color: normalized, key: foregroundKey)
        notifyChange()
    }

    private func store(color: UIColor, key: String) {
        if let data = try? NSKeyedArchiver.archivedData(withRootObject: color, requiringSecureCoding: false) {
            UserDefaults.standard.set(data, forKey: key)
        }
    }

    private func loadColor(key: String, fallback: UIColor) -> UIColor {
        guard let data = UserDefaults.standard.data(forKey: key),
              let color = try? NSKeyedUnarchiver.unarchiveTopLevelObjectWithData(data) as? UIColor else {
            return fallback
        }
        return normalize(color)
    }

    private func normalize(_ color: UIColor) -> UIColor {
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
        if color.getRed(&r, green: &g, blue: &b, alpha: &a) {
            return UIColor(red: r, green: g, blue: b, alpha: a)
        }
        return color
    }

    private func notifyChange() {
        NotificationCenter.default.post(name: TerminalFontSettings.appearanceDidChangeNotification, object: nil)
    }
}

struct TerminalView: View {
    @ObservedObject private var fontSettings = TerminalFontSettings.shared
    @State private var showingSettings = false

    var body: some View {
        KeyboardAwareContainer(
            content: GeometryReader { proxy in
                TerminalContentView(availableSize: proxy.size,
                                    fontSettings: fontSettings)
                    .frame(width: proxy.size.width, height: proxy.size.height)
            }
        )
        .edgesIgnoringSafeArea(.bottom)
        .overlay(alignment: .topTrailing) {
            Button(action: { showingSettings = true }) {
                Image(systemName: "textformat.size")
                    .font(.system(size: 16, weight: .semibold))
                    .padding(8)
                    .background(.ultraThinMaterial, in: Circle())
            }
            .padding()
            .accessibilityLabel("Adjust Font Size")
        }
        .sheet(isPresented: $showingSettings) {
            TerminalSettingsView()
        }
        .background(Color(.systemBackground))
    }
}

private struct TerminalContentView: View {
    let availableSize: CGSize
    @ObservedObject private var fontSettings: TerminalFontSettings
    @ObservedObject private var runtime = PscalRuntimeBootstrap.shared
    @State private var focusAnchor: Int = 0

    init(availableSize: CGSize, fontSettings: TerminalFontSettings) {
        self.availableSize = availableSize
        _fontSettings = ObservedObject(wrappedValue: fontSettings)
    }

    var body: some View {
        let background = runtime.terminalBackgroundColor
        let elvisToken = runtime.elvisRenderToken
        let elvisActive = runtime.isElvisModeActive()
        let elvisVisible = ElvisWindowManager.shared.isVisible
        return VStack(spacing: 0) {
            TerminalRendererView(text: runtime.screenText,
                                 cursor: runtime.cursorInfo,
                                 backgroundColor: fontSettings.backgroundColor,
                                 foregroundColor: fontSettings.foregroundColor,
                                 isElvisMode: elvisActive,
                                 isElvisWindowVisible: elvisVisible,
                                 elvisRenderToken: elvisToken,
                                 fontPointSize: fontSettings.pointSize)
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
        .padding(.top, 32)
        .background(Color(fontSettings.backgroundColor))
        .contentShape(Rectangle())
        .onTapGesture {
            focusAnchor &+= 1
        }
        .overlay(alignment: .bottomLeading) {
            if !ElvisWindowManager.shared.isVisible {
                TerminalInputBridge(focusAnchor: $focusAnchor, onInput: handleInput)
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
    }

    private func handleInput(_ text: String) {
        runtime.send(text)
    }

    private func updateTerminalGeometry() {
        let showingStatus = runtime.exitStatus != nil
        guard let metrics = TerminalGeometryCalculator.metrics(for: availableSize,
                                                               showingStatus: showingStatus)
                ?? TerminalGeometryCalculator.fallbackMetrics(showingStatus: showingStatus) else {
            return
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

    static func metrics(for size: CGSize, showingStatus: Bool) -> TerminalGeometryMetrics? {
        guard size.width > 0, size.height > 0 else { return nil }

        let font = UIFont.monospacedSystemFont(ofSize: UIFont.preferredFont(forTextStyle: .body).pointSize, weight: .regular)
        let charWidth = max(1.0, ("W" as NSString).size(withAttributes: [.font: font]).width)
        let lineHeight = font.lineHeight + verticalRowPadding

        let usableWidth = max(0, size.width - horizontalPadding)
        var usableHeight = size.height
        if showingStatus {
            usableHeight -= statusOverlayHeight
        }
        usableHeight -= dividerHeight

        let rawColumns = Int(floor(usableWidth / charWidth))
        let rawRows = Int(floor(usableHeight / lineHeight))
        guard rawColumns > 0, rawRows > 0 else { return nil }

        return TerminalGeometryMetrics(
            columns: max(10, rawColumns),
            rows: max(4, rawRows)
        )
    }

    static func fallbackMetrics(showingStatus: Bool) -> TerminalGeometryMetrics? {
        return metrics(for: UIScreen.main.bounds.size, showingStatus: showingStatus)
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
    let fontPointSize: CGFloat

    func makeUIView(context: Context) -> TerminalRendererContainerView {
        let container = TerminalRendererContainerView()
        container.configure(backgroundColor: backgroundColor, foregroundColor: foregroundColor)
        container.applyFont(pointSize: fontPointSize)
        return container
    }

    func updateUIView(_ uiView: TerminalRendererContainerView, context: Context) {
        _ = elvisRenderToken
        uiView.configure(backgroundColor: backgroundColor, foregroundColor: foregroundColor)
        uiView.applyFont(pointSize: fontPointSize)
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

final class TerminalRendererContainerView: UIView {
    private let terminalView = TerminalDisplayTextView()
    private var lastElvisSnapshotText: String?
    private(set) var resolvedFont: UIFont = TerminalFontSettings.shared.currentFont
    private var appearanceObserver: NSObjectProtocol?

    override init(frame: CGRect) {
        super.init(frame: frame)
        clipsToBounds = true
        terminalView.isEditable = false
        terminalView.isSelectable = true
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
        addSubview(terminalView)
        appearanceObserver = NotificationCenter.default.addObserver(forName: TerminalFontSettings.appearanceDidChangeNotification,
                                                                    object: nil,
                                                                    queue: .main) { [weak self] _ in
            guard let self else { return }
            self.applyFont(pointSize: TerminalFontSettings.shared.pointSize)
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
    }

    func configure(backgroundColor: UIColor, foregroundColor: UIColor) {
        terminalView.backgroundColor = backgroundColor
        terminalView.textColor = foregroundColor
        self.backgroundColor = backgroundColor
    }

    func applyFont(pointSize: CGFloat) {
        let clamped = TerminalFontSettings.shared.clamp(pointSize)
        let font = UIFont.monospacedSystemFont(ofSize: clamped, weight: .regular)
        if terminalView.font != font {
            terminalView.font = font
            terminalView.typingAttributes[.font] = font
            resolvedFont = font
        }
    }

    func applyElvisFont(_ font: UIFont) {
        terminalView.adjustsFontForContentSizeCategory = false
        terminalView.font = font
        terminalView.typingAttributes[.font] = font
        resolvedFont = font
    }

    func currentFont() -> UIFont {
        return terminalView.font ?? resolvedFont
    }

    func update(text: NSAttributedString,
                cursor: TerminalCursorInfo?,
                backgroundColor: UIColor,
                isElvisMode: Bool,
                elvisSnapshot: ElvisSnapshot?) {
        if isElvisMode, let snapshot = elvisSnapshot {
            applyElvisSnapshot(snapshot, backgroundColor: backgroundColor)
            terminalView.cursorTextOffset = nil
            terminalView.cursorInfo = nil
            return
        } else {
            lastElvisSnapshotText = nil
            terminalView.attributedText = text
            terminalView.cursorTextOffset = cursor?.textOffset
            terminalView.cursorInfo = cursor
            terminalView.backgroundColor = backgroundColor
            scrollToBottom(textView: terminalView, text: text)
        }
    }

    private func applyElvisSnapshot(_ snapshot: ElvisSnapshot, backgroundColor: UIColor) {
        if lastElvisSnapshotText != snapshot.text {
            lastElvisSnapshotText = snapshot.text
            terminalView.text = snapshot.text
        }
        terminalView.backgroundColor = backgroundColor
        terminalView.textColor = TerminalFontSettings.shared.foregroundColor
    }

    private func scrollToBottom(textView: UITextView, text: NSAttributedString) {
        let length = text.length
        guard length > 0 else { return }
        let bottomRange = NSRange(location: length - 1, length: 1)
        DispatchQueue.main.async {
            let currentLength = textView.attributedText.length
            guard bottomRange.location < currentLength else { return }
            textView.scrollRangeToVisible(bottomRange)
        }
    }
}

final class TerminalDisplayTextView: UITextView {
    var cursorInfo: TerminalCursorInfo? {
        didSet { updateCursorLayer() }
    }
    var cursorTextOffset: Int? {
        didSet { updateCursorLayer() }
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
        isEditable = false
        isScrollEnabled = true
        textContainerInset = .zero
        backgroundColor = .clear
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        updateCursorLayer()
    }

    override var contentOffset: CGPoint {
        didSet { updateCursorLayer() }
    }

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
