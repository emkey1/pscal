// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceViewController.m at
// 0f75fee1c) and licensed to PSCAL under the Apache License 2.0.

import UIKit

/// An applet with its own keyboard-input view, which takes first responder back whenever
/// its window comes to the front: a tap, window cycling or a Desktop switch.
@MainActor
protocol WorkspaceFocusable: AnyObject {
    func workspaceToolDidBecomeFrontmost()
}

/// An applet whose text can be made bigger or smaller with Cmd+=, Cmd+- and Cmd+0. The base
/// class implements the property, so a subclass declares this and overrides
/// `applyTextScale()` to re-font its own views. The scale is per window, and Workspace saves
/// it in the window's layout.
@MainActor
protocol WorkspaceTextScalable: AnyObject {
    var workspaceTextScale: CGFloat { get set }
}

/// An applet whose content survives a restart, such as a viewer's open file. The state goes
/// inside the saved window layout, so it holds plist types only and stays small.
@MainActor
protocol WorkspaceStatefulTool: AnyObject {
    func workspaceToolStateForSaving() -> [String: Any]?
    func workspaceRestoreToolState(_ state: [String: Any])
}

/// An applet that can be told to open or reveal a path.
@MainActor
protocol WorkspaceFileOpenable: AnyObject {
    func workspaceOpenFile(atPath path: String)
}

/// An applet that keeps some of Workspace's window-level keys for itself while it's focused.
/// A claimed key goes on through the responder chain to the applet's own key commands.
@MainActor
protocol WorkspaceKeyClaiming: AnyObject {
    func workspaceClaimsKey(input: String, keyCode: UIKeyboardHIDUsage, modifiers: UIKeyModifierFlags) -> Bool
}

enum WorkspaceTextScale {
    /// Each step is big enough to see, on small text as on large.
    private static let steps: [CGFloat] = [0.5, 0.75, 0.85, 1, 1.15, 1.25, 1.5, 1.75, 2, 2.5, 3]

    static func clamped(_ scale: CGFloat) -> CGFloat {
        guard scale > 0 else { return 1 }
        return max(steps[0], min(steps[steps.count - 1], scale))
    }

    /// The next step above or below `current`, or back to 1 for a `step` of 0. A value between
    /// two steps moves to the nearer one in the direction asked.
    static func stepped(_ current: CGFloat, step: Int) -> CGFloat {
        let value = clamped(current)
        if step > 0 {
            return steps.first { $0 > value + 0.001 } ?? steps[steps.count - 1]
        }
        if step < 0 {
            return steps.last { $0 < value - 0.001 } ?? steps[0]
        }
        return 1
    }
}

/// A ring or a bar, as the theme's gauge style says, with its reading drawn inside.
@MainActor
final class WorkspaceGaugeView: UIView {
    var progress: Double = 0 {
        didSet {
            progress = max(0, min(1, progress))
            setNeedsDisplay()
        }
    }
    var valueText: String? { didSet { setNeedsDisplay() } }
    var fillColor: UIColor? { didSet { setNeedsDisplay() } }
    var trackColor: UIColor? { didSet { setNeedsDisplay() } }
    var valueColor: UIColor? { didSet { setNeedsDisplay() } }
    var compact = false {
        didSet {
            guard compact != oldValue else { return }
            invalidateIntrinsicContentSize()
            setNeedsDisplay()
        }
    }
    private var observer: NSObjectProtocol?

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .clear
        contentMode = .redraw
        observer = NotificationCenter.default.addObserver(forName: .workspaceGaugeStyleDidChange, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.invalidateIntrinsicContentSize()
                self?.setNeedsDisplay()
            }
        }
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    deinit {
        if let observer {
            NotificationCenter.default.removeObserver(observer)
        }
    }

    override var intrinsicContentSize: CGSize {
        if WorkspaceTheme.usesRingGauges {
            let dimension: CGFloat = compact ? 32 : (WorkspaceMetrics.usesPhoneLayout ? 42 : 54)
            return CGSize(width: UIView.noIntrinsicMetric, height: dimension)
        }
        return CGSize(width: UIView.noIntrinsicMetric, height: compact ? 20 : 28)
    }

    override func draw(_ rect: CGRect) {
        let fill = fillColor ?? .systemTeal
        let track = trackColor ?? UIColor(white: 0.5, alpha: 0.25)
        let value = valueColor ?? fill
        let text = (valueText ?? "") as NSString
        let bounds = self.bounds
        let clamped = CGFloat(max(0, min(1, progress)))

        if WorkspaceTheme.usesRingGauges {
            let dimension = min(bounds.width, bounds.height)
            let lineWidth = max(6, dimension * 0.11)
            let radius = (dimension - lineWidth) / 2 - 1
            guard radius > 1 else { return }
            let center = CGPoint(x: bounds.midX, y: bounds.midY)
            let trackPath = UIBezierPath(arcCenter: center, radius: radius, startAngle: 0, endAngle: 2 * .pi, clockwise: true)
            trackPath.lineWidth = lineWidth
            track.setStroke()
            trackPath.stroke()
            if clamped > 0 {
                let start = -CGFloat.pi / 2
                let path = UIBezierPath(arcCenter: center, radius: radius, startAngle: start, endAngle: start + 2 * .pi * clamped, clockwise: true)
                path.lineWidth = lineWidth
                path.lineCapStyle = .round
                fill.setStroke()
                path.stroke()
            }
            if text.length > 0 {
                let paragraph = NSMutableParagraphStyle()
                paragraph.alignment = .center
                let attributes: [NSAttributedString.Key: Any] = [
                    .font: UIFont.systemFont(ofSize: max(8.5, radius * 0.66), weight: .bold),
                    .foregroundColor: value,
                    .paragraphStyle: paragraph,
                ]
                let size = text.size(withAttributes: attributes)
                text.draw(in: CGRect(x: bounds.minX, y: center.y - size.height / 2, width: bounds.width, height: size.height),
                          withAttributes: attributes)
            }
            return
        }

        let barHeight: CGFloat = compact ? 5 : 7
        let barY = bounds.maxY - barHeight
        if text.length > 0 {
            let attributes: [NSAttributedString.Key: Any] = [
                .font: UIFont.systemFont(ofSize: compact ? 11 : 14, weight: .bold),
                .foregroundColor: value,
            ]
            text.draw(at: CGPoint(x: bounds.minX, y: bounds.minY), withAttributes: attributes)
        }
        track.setFill()
        UIBezierPath(roundedRect: CGRect(x: bounds.minX, y: barY, width: bounds.width, height: barHeight),
                     cornerRadius: barHeight / 2).fill()
        let fillWidth = bounds.width * clamped
        if fillWidth > 0.5 {
            fill.setFill()
            UIBezierPath(roundedRect: CGRect(x: bounds.minX, y: barY, width: max(barHeight, fillWidth), height: barHeight),
                         cornerRadius: barHeight / 2).fill()
        }
    }
}

/// A left-aligned button with a title and a smaller line under it. The kit keeps the two
/// strings, so a new theme or text size can draw them again.
@MainActor
final class WorkspaceTwoLineButton: UIButton {
    fileprivate(set) var titleText = ""
    fileprivate(set) var subtitleText = ""
}

/// Every applet's base: a themed background, the area to build in, the Workspace that holds
/// it, and factories for cards, labels and other views that follow the theme.
///
/// A subclass builds its interface in `viewDidLoad`, calling super first, and overrides
/// `applyTheme()` and `applyTextScale()` for anything the factories don't cover.
@MainActor
class WorkspaceAppletViewController: UIViewController {
    weak var workspaceHost: WorkspaceViewController?
    /// The safe-area-inset area a subclass builds in.
    let toolContentView = UIView()

    private let backgroundGradient = CAGradientLayer()
    // Weak, so views dropped by a rebuild of a list don't pile up here.
    private let trackedCards = NSHashTable<UIView>.weakObjects()
    private let trackedPrimaryLabels = NSHashTable<UILabel>.weakObjects()
    private let trackedSecondaryLabels = NSHashTable<UILabel>.weakObjects()
    private let trackedAccentLabels = NSHashTable<UILabel>.weakObjects()
    private let trackedTextViews = NSHashTable<UITextView>.weakObjects()
    private let trackedIcons = NSHashTable<UIImageView>.weakObjects()
    private let trackedGauges = NSHashTable<WorkspaceGaugeView>.weakObjects()
    private let trackedTwoLineButtons = NSHashTable<WorkspaceTwoLineButton>.weakObjects()
    private let trackedTileCaptions = NSHashTable<UILabel>.weakObjects()
    private var scaleBaseFonts = NSMapTable<UILabel, UIFont>.weakToStrongObjects()
    private var scaleAppliedFonts = NSMapTable<UILabel, UIFont>.weakToStrongObjects()
    private var themeObserver: NSObjectProtocol?
    private var storedTextScale: CGFloat = 1
    private var autosizedWindowSize: CGSize?

    var theme: WorkspaceThemeColors { WorkspaceTheme.current }

    deinit {
        if let themeObserver {
            NotificationCenter.default.removeObserver(themeObserver)
        }
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.clipsToBounds = true
        view.backgroundColor = .black
        backgroundGradient.startPoint = CGPoint(x: 0.12, y: 0)
        backgroundGradient.endPoint = CGPoint(x: 0.88, y: 1)
        view.layer.insertSublayer(backgroundGradient, at: 0)

        toolContentView.translatesAutoresizingMaskIntoConstraints = false
        toolContentView.backgroundColor = .clear
        view.addSubview(toolContentView)
        NSLayoutConstraint.activate([
            toolContentView.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            toolContentView.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor),
            toolContentView.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor),
            toolContentView.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor),
        ])

        themeObserver = NotificationCenter.default.addObserver(forName: .workspaceThemeDidChange, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.themeDidChange()
            }
        }
        applyTheme()
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        // A subclass builds its views after this class's viewDidLoad already applied the
        // theme, so apply it again now they exist. Without this a label keeps UILabel's
        // default colour until the theme next changes, which is invisible on a dark theme.
        applyTheme()
        // The same for a text size set before the subclass built its views.
        if abs(workspaceTextScale - 1) > 0.001 {
            applyTextScale()
        }
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        backgroundGradient.frame = view.bounds
        let width = toolContentView.bounds.width
        let compact = width > 0 && width < 330
        for gauge in trackedGauges.allObjects {
            gauge.compact = compact
        }
    }

    /// The theme changed. An applet with a list built from the theme rebuilds it here.
    func themeDidChange() {
        applyTheme()
    }

    /// Remembers the window's size as one this applet gave it.
    func noteAutosizedWindow() {
        autosizedWindowSize = workspaceHost?.window(holding: self)?.frame.size
    }

    /// Follows a text size change while the window still has the size the applet gave it. A
    /// size the person chose stays.
    func growWindowForTextScale(from previous: CGSize, to next: CGSize) {
        guard let host = workspaceHost, let window = host.window(holding: self), !window.zoomedToFullscreen else { return }
        let expected = autosizedWindowSize ?? previous
        guard abs(window.frame.width - expected.width) < 1, abs(window.frame.height - expected.height) < 1 else { return }
        host.resizeWindow(window, to: next)
        noteAutosizedWindow()
    }

    // MARK: - Text scale

    var workspaceTextScale: CGFloat {
        get { storedTextScale }
        set {
            let clamped = WorkspaceTextScale.clamped(newValue)
            guard abs(clamped - storedTextScale) >= 0.001 else { return }
            storedTextScale = clamped
            if isViewLoaded {
                applyTextScale()
            }
        }
    }

    /// `size` at this window's scale, rounded to half a point.
    func scaledFontSize(_ size: CGFloat) -> CGFloat {
        (size * workspaceTextScale * 2).rounded() / 2
    }

    /// The same, but untouched at 1: the density and phone factors give sizes in between,
    /// so the rounding alone would move text nobody had scaled.
    func toolScaledFontSize(_ size: CGFloat) -> CGFloat {
        abs(workspaceTextScale - 1) < 0.001 ? size : scaledFontSize(size)
    }

    func applyTextScale() {
        applyTrackedTextViewFonts()
        for button in trackedTwoLineButtons.allObjects {
            drawTwoLineButton(button)
        }
        trackedTileCaptions.allObjects.forEach(scaleLabel)
    }

    /// Scales a label from the font it was given, which is remembered, so calling this again
    /// never compounds. A font set on the label since the last call becomes the new base.
    func scaleLabel(_ label: UILabel) {
        guard let current = label.font else { return }
        var base = scaleBaseFonts.object(forKey: label)
        let applied = scaleAppliedFonts.object(forKey: label)
        if base == nil || applied == nil || current != applied {
            base = current
        }
        guard let base else { return }
        let scaled = base.withSize(scaledFontSize(base.pointSize))
        label.font = scaled
        scaleBaseFonts.setObject(base, forKey: label)
        scaleAppliedFonts.setObject(scaled, forKey: label)
    }

    private func applyTrackedTextViewFonts() {
        let size = scaledFontSize(WorkspaceTheme.textViewFontSize)
        for textView in trackedTextViews.allObjects {
            textView.font = .monospacedSystemFont(ofSize: size, weight: .regular)
        }
    }

    // MARK: - Factories

    private func themeLabel(style: UIFont.TextStyle, monospaced: Bool, tracker: NSHashTable<UILabel>) -> UILabel {
        let label = UILabel()
        label.translatesAutoresizingMaskIntoConstraints = false
        label.numberOfLines = 0
        let size = WorkspaceTheme.fontSize(style)
        label.font = monospaced ? .monospacedSystemFont(ofSize: size, weight: .medium) : .systemFont(ofSize: size)
        tracker.add(label)
        return label
    }

    func themePrimaryLabel(style: UIFont.TextStyle, monospaced: Bool = false) -> UILabel {
        themeLabel(style: style, monospaced: monospaced, tracker: trackedPrimaryLabels)
    }

    func themeSecondaryLabel(style: UIFont.TextStyle, monospaced: Bool = false) -> UILabel {
        themeLabel(style: style, monospaced: monospaced, tracker: trackedSecondaryLabels)
    }

    func themeAccentLabel(style: UIFont.TextStyle, monospaced: Bool = false) -> UILabel {
        themeLabel(style: style, monospaced: monospaced, tracker: trackedAccentLabels)
    }

    func themeCardView() -> UIView {
        let card = UIView()
        card.translatesAutoresizingMaskIntoConstraints = false
        card.layer.borderWidth = 1
        card.layer.shadowColor = UIColor.black.cgColor
        card.layer.shadowOpacity = 0.16
        trackedCards.add(card)
        styleCard(card)
        return card
    }

    private func styleCard(_ card: UIView) {
        let phone = WorkspaceMetrics.usesPhoneLayout
        card.layer.cornerRadius = WorkspaceTheme.densityValue(phone ? 10 : 14, phone ? 16 : 22)
        card.layer.shadowRadius = WorkspaceTheme.densityValue(phone ? 7 : 10, phone ? 12 : 18)
        card.layer.shadowOffset = CGSize(width: 0, height: WorkspaceTheme.densityValue(phone ? 4 : 6, phone ? 7 : 10))
    }

    func themeTextView() -> UITextView {
        let textView = UITextView()
        textView.translatesAutoresizingMaskIntoConstraints = false
        textView.isEditable = false
        textView.alwaysBounceVertical = true
        textView.backgroundColor = .clear
        trackedTextViews.add(textView)
        applyTrackedTextViewFonts()
        return textView
    }

    func themeAccentIcon(named symbol: String, pointSize: CGFloat) -> UIImageView {
        let icon = UIImageView()
        icon.translatesAutoresizingMaskIntoConstraints = false
        icon.contentMode = .scaleAspectFit
        icon.tintColor = theme.accent
        let configuration = UIImage.SymbolConfiguration(pointSize: pointSize, weight: .semibold)
        icon.image = UIImage(systemName: symbol, withConfiguration: configuration)?.withRenderingMode(.alwaysTemplate)
        icon.setContentHuggingPriority(.required, for: .horizontal)
        icon.setContentCompressionResistancePriority(.required, for: .horizontal)
        trackedIcons.add(icon)
        return icon
    }

    /// An icon and a caption, with a right-aligned label after them if `trailing` is set.
    func tileHeaderRow(icon symbol: String, caption: String, trailing: Bool) -> (row: UIStackView, trailing: UILabel?) {
        let header = UIStackView()
        header.axis = .horizontal
        header.spacing = 6
        header.alignment = .center
        let icon = themeAccentIcon(named: symbol, pointSize: 13)
        icon.widthAnchor.constraint(equalToConstant: 16).isActive = true
        icon.heightAnchor.constraint(equalToConstant: 16).isActive = true
        let captionLabel = themeSecondaryLabel(style: .caption1)
        captionLabel.text = caption
        captionLabel.font = .systemFont(ofSize: 11, weight: .semibold)
        trackedTileCaptions.add(captionLabel)
        header.addArrangedSubview(icon)
        header.addArrangedSubview(captionLabel)
        guard trailing else { return (header, nil) }
        let trailingLabel = themeSecondaryLabel(style: .caption1)
        trailingLabel.font = .systemFont(ofSize: 12, weight: .regular)
        trailingLabel.textAlignment = .right
        captionLabel.setContentHuggingPriority(.defaultLow, for: .horizontal)
        header.addArrangedSubview(trailingLabel)
        return (header, trailingLabel)
    }

    /// A card with a caption, a right-aligned subtitle and a gauge under them.
    func gaugeTile(icon symbol: String, caption: String) -> (card: UIView, subtitle: UILabel, gauge: WorkspaceGaugeView) {
        let card = themeCardView()
        let outer = UIStackView()
        outer.translatesAutoresizingMaskIntoConstraints = false
        outer.axis = .vertical
        outer.spacing = 5
        card.addSubview(outer)

        let header = tileHeaderRow(icon: symbol, caption: caption, trailing: true)
        let gauge = WorkspaceGaugeView(frame: .zero)
        gauge.translatesAutoresizingMaskIntoConstraints = false
        trackedGauges.add(gauge)
        outer.addArrangedSubview(header.row)
        outer.addArrangedSubview(gauge)

        let phone = WorkspaceMetrics.usesPhoneLayout
        let insetY: CGFloat = phone ? 5 : 7
        NSLayoutConstraint.activate([
            card.heightAnchor.constraint(greaterThanOrEqualToConstant: phone ? 50 : 64),
            outer.topAnchor.constraint(equalTo: card.topAnchor, constant: insetY),
            outer.leadingAnchor.constraint(equalTo: card.leadingAnchor, constant: 12),
            outer.trailingAnchor.constraint(equalTo: card.trailingAnchor, constant: -12),
            outer.bottomAnchor.constraint(equalTo: card.bottomAnchor, constant: -insetY),
        ])
        return (card, header.trailing ?? UILabel(), gauge)
    }

    /// An icon, a title and a right-aligned value on one line.
    func iconRow(icon symbol: String, title: String) -> (row: UIStackView, title: UILabel, value: UILabel) {
        let row = UIStackView()
        row.translatesAutoresizingMaskIntoConstraints = false
        row.axis = .horizontal
        row.spacing = 10
        row.alignment = .center

        let icon = themeAccentIcon(named: symbol, pointSize: 12)
        icon.widthAnchor.constraint(equalToConstant: 18).isActive = true
        icon.heightAnchor.constraint(equalToConstant: 16).isActive = true

        let titleLabel = themePrimaryLabel(style: .subheadline)
        titleLabel.text = title
        titleLabel.font = .systemFont(ofSize: 12, weight: .regular)

        let value = themeSecondaryLabel(style: .subheadline)
        value.font = .systemFont(ofSize: 12, weight: .semibold)
        value.textAlignment = .right
        value.numberOfLines = 1
        value.adjustsFontSizeToFitWidth = true
        value.minimumScaleFactor = 0.6

        row.addArrangedSubview(icon)
        row.addArrangedSubview(titleLabel)
        row.addArrangedSubview(value)
        titleLabel.setContentHuggingPriority(.defaultLow, for: .horizontal)
        titleLabel.setContentCompressionResistancePriority(.required, for: .horizontal)
        value.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        return (row, titleLabel, value)
    }

    func rowsCard(rows: [UIView]) -> UIView {
        let card = themeCardView()
        let stack = UIStackView()
        stack.translatesAutoresizingMaskIntoConstraints = false
        stack.axis = .vertical
        stack.spacing = 2
        rows.forEach(stack.addArrangedSubview)
        card.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: card.topAnchor, constant: 8),
            stack.leadingAnchor.constraint(equalTo: card.leadingAnchor, constant: 12),
            stack.trailingAnchor.constraint(equalTo: card.trailingAnchor, constant: -12),
            stack.bottomAnchor.constraint(equalTo: card.bottomAnchor, constant: -8),
        ])
        return card
    }

    /// A card holding a vertical stack, inset by `inset`.
    func stackCard(spacing: CGFloat, inset: CGFloat) -> (card: UIView, stack: UIStackView) {
        let card = themeCardView()
        let stack = UIStackView()
        stack.translatesAutoresizingMaskIntoConstraints = false
        stack.axis = .vertical
        stack.spacing = spacing
        card.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: card.topAnchor, constant: inset),
            stack.leadingAnchor.constraint(equalTo: card.leadingAnchor, constant: inset),
            stack.trailingAnchor.constraint(equalTo: card.trailingAnchor, constant: -inset),
            stack.bottomAnchor.constraint(equalTo: card.bottomAnchor, constant: -inset),
        ])
        return (card, stack)
    }

    /// A vertical stack in a scroll view filling `toolContentView`, as wide as the window.
    func installScrollingStack(spacing: CGFloat, inset: CGFloat) -> (scroll: UIScrollView, stack: UIStackView) {
        let scrollView = UIScrollView()
        scrollView.translatesAutoresizingMaskIntoConstraints = false
        scrollView.alwaysBounceVertical = true
        toolContentView.addSubview(scrollView)
        let stack = UIStackView()
        stack.translatesAutoresizingMaskIntoConstraints = false
        stack.axis = .vertical
        stack.spacing = spacing
        scrollView.addSubview(stack)
        NSLayoutConstraint.activate([
            scrollView.topAnchor.constraint(equalTo: toolContentView.topAnchor),
            scrollView.leadingAnchor.constraint(equalTo: toolContentView.leadingAnchor),
            scrollView.trailingAnchor.constraint(equalTo: toolContentView.trailingAnchor),
            scrollView.bottomAnchor.constraint(equalTo: toolContentView.bottomAnchor),
            stack.topAnchor.constraint(equalTo: scrollView.contentLayoutGuide.topAnchor, constant: inset),
            stack.leadingAnchor.constraint(equalTo: scrollView.frameLayoutGuide.leadingAnchor, constant: inset),
            stack.trailingAnchor.constraint(equalTo: scrollView.frameLayoutGuide.trailingAnchor, constant: -inset),
            stack.bottomAnchor.constraint(equalTo: scrollView.contentLayoutGuide.bottomAnchor, constant: -inset),
        ])
        return (scrollView, stack)
    }

    func clearArrangedSubviews(of stack: UIStackView) {
        for view in stack.arrangedSubviews {
            stack.removeArrangedSubview(view)
            view.removeFromSuperview()
        }
    }

    /// A button with a title over a smaller line, in the theme's colours and the window's
    /// text size.
    func twoLineButton(title: String, subtitle: String, action: Selector) -> WorkspaceTwoLineButton {
        let button = WorkspaceTwoLineButton(type: .system)
        button.translatesAutoresizingMaskIntoConstraints = false
        button.contentHorizontalAlignment = .left
        button.titleLabel?.numberOfLines = 0
        button.layer.cornerRadius = 12
        button.layer.borderWidth = 1
        button.contentEdgeInsets = UIEdgeInsets(top: 7, left: 10, bottom: 7, right: 10)
        button.addTarget(self, action: action, for: .touchUpInside)
        trackedTwoLineButtons.add(button)
        setTwoLineButton(button, title: title, subtitle: subtitle)
        return button
    }

    func setTwoLineButton(_ button: WorkspaceTwoLineButton, title: String, subtitle: String) {
        button.titleText = title
        button.subtitleText = subtitle
        drawTwoLineButton(button)
    }

    private func drawTwoLineButton(_ button: WorkspaceTwoLineButton) {
        let colors = theme
        button.backgroundColor = colors.cardAlt.withAlphaComponent(0.94)
        button.layer.borderColor = colors.stroke.cgColor
        let paragraph = NSMutableParagraphStyle()
        paragraph.alignment = .left
        let titleFont = UIFont.systemFont(ofSize: toolScaledFontSize(WorkspaceTheme.fontSize(.subheadline)), weight: .semibold)
        let subtitleFont = UIFont.systemFont(ofSize: toolScaledFontSize(WorkspaceTheme.fontSize(.caption1)), weight: .medium)
        let text = NSMutableAttributedString(string: button.titleText, attributes: [
            .font: titleFont, .foregroundColor: colors.primary, .paragraphStyle: paragraph,
        ])
        text.append(NSAttributedString(string: "\n" + button.subtitleText, attributes: [
            .font: subtitleFont, .foregroundColor: colors.secondary, .paragraphStyle: paragraph,
        ]))
        button.setAttributedTitle(text, for: .normal)
    }

    // MARK: - Theme

    /// Recolours everything the factories made. A subclass overrides this, calling super
    /// first, for anything else.
    func applyTheme() {
        let colors = theme
        backgroundGradient.colors = [colors.backgroundTop.cgColor, colors.backgroundBottom.cgColor]
        for card in trackedCards.allObjects {
            styleCard(card)
            card.backgroundColor = colors.card.withAlphaComponent(0.95)
            card.layer.borderColor = colors.stroke.cgColor
        }
        trackedPrimaryLabels.allObjects.forEach { $0.textColor = colors.primary }
        trackedSecondaryLabels.allObjects.forEach { $0.textColor = colors.secondary }
        trackedAccentLabels.allObjects.forEach { $0.textColor = colors.accent }
        applyTrackedTextViewFonts()
        for textView in trackedTextViews.allObjects {
            textView.textColor = colors.primary
            textView.tintColor = colors.accent
        }
        trackedIcons.allObjects.forEach { $0.tintColor = colors.accent }
        let gaugeTrack = colors.secondary.withAlphaComponent(0.22)
        for gauge in trackedGauges.allObjects {
            gauge.fillColor = colors.accent
            gauge.trackColor = gaugeTrack
            gauge.valueColor = colors.primary
        }
        trackedTwoLineButtons.allObjects.forEach(drawTwoLineButton)
    }
}
