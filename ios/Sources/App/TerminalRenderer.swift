import SwiftUI
import UIKit
import QuartzCore
import Combine

struct TerminalRendererView: UIViewRepresentable {
    let text: NSAttributedString
    let cursor: TerminalCursorInfo?
    let backgroundColor: UIColor
    let foregroundColor: UIColor
    let isEditorMode: Bool
    let isEditorWindowVisible: Bool
    let editorRenderToken: UInt64
    let font: UIFont
    let fontPointSize: CGFloat
    let editorSnapshot: EditorSnapshot?
    var onPaste: ((String) -> Void)? = nil
    var onInput: ((String) -> Void)? = nil
    let mouseMode: TerminalBuffer.MouseMode
    let mouseEncoding: TerminalBuffer.MouseEncoding
    var onGeometryChange: ((Int, Int) -> Void)? = nil

    func makeUIView(context: Context) -> TerminalRendererContainerView {
        let container = TerminalRendererContainerView()
        container.configure(backgroundColor: backgroundColor,
                            foregroundColor: foregroundColor)
        container.applyFont(font: font)
        container.onPaste = onPaste
        container.onInput = onInput
        container.updateMouseState(mode: mouseMode, encoding: mouseEncoding)
        container.onGeometryChange = onGeometryChange
        return container
    }

    func updateUIView(_ uiView: TerminalRendererContainerView, context: Context) {
        _ = editorRenderToken

        uiView.configure(backgroundColor: backgroundColor,
                         foregroundColor: foregroundColor)
        uiView.applyFont(font: font)
        uiView.onPaste = onPaste
        uiView.onInput = onInput
        uiView.updateMouseState(mode: mouseMode, encoding: mouseEncoding)
        uiView.onGeometryChange = onGeometryChange

        let externalWindowEnabled = EditorWindowManager.externalWindowEnabled
        let shouldBlankMain = isEditorMode && isEditorWindowVisible && externalWindowEnabled
        if shouldBlankMain {
            uiView.update(text: NSAttributedString(string: ""),
                          cursor: nil,
                          backgroundColor: backgroundColor,
                          isEditorMode: false,
                          editorSnapshot: nil)
            return
        }

        let shouldUseSnapshot = isEditorMode && (!externalWindowEnabled || !isEditorWindowVisible)
        let snapshot = shouldUseSnapshot ? editorSnapshot : nil
        let renderEditor = shouldUseSnapshot && snapshot != nil

        uiView.update(text: text,
                      cursor: cursor,
                      backgroundColor: backgroundColor,
                      isEditorMode: renderEditor,
                      editorSnapshot: snapshot)
    }
}

enum TerminalFontMetrics {
    static var displayFont: UIFont {
        TerminalFontSettings.shared.currentFont
    }

    static var characterWidth: CGFloat {
        TerminalGeometryCalculator.characterMetrics(for: displayFont).width
    }

    static var lineHeight: CGFloat {
        TerminalGeometryCalculator.characterMetrics(for: displayFont).lineHeight
    }
}

// MARK: - TerminalRendererContainerView

final class TerminalRendererContainerView: UIView, UIGestureRecognizerDelegate, UITextViewDelegate {
    private let terminalView = TerminalDisplayTextView()
    private let selectionOverlay = TerminalSelectionOverlay()
    private let selectionMenu = TerminalSelectionMenuView()
    private let commandIndicator = UILabel()
    var onGeometryChange: ((Int, Int) -> Void)?
    var onInput: ((String) -> Void)?

    // Mouse tracking
    private var mouseMode: TerminalBuffer.MouseMode = .none
    private var mouseEncoding: TerminalBuffer.MouseEncoding = .normal
    private var lastMouseLocation: (row: Int, col: Int)?
    private var lastEditorSnapshotText: String?
    private var lastEditorCursorOffset: Int?

    private(set) var resolvedFont: UIFont = TerminalFontSettings.shared.currentFont

    private var appearanceObserver: NSObjectProtocol?
    private var modifierObserver: NSObjectProtocol?

    private var selectionStartIndex: Int?
    private var selectionEndIndex: Int?
    private var selectionAnchorPoint: CGPoint?

    private var scrollbackEnabled = true
    private var autoScrollEnabled = true

    private var pendingUpdate: (
        text: NSAttributedString,
        cursor: TerminalCursorInfo?,
        backgroundColor: UIColor,
        isEditorMode: Bool,
        editorSnapshot: EditorSnapshot?
    )?

    private var customKeyCommands: [UIKeyCommand] = []
    private var lastReportedGeometry: TerminalGeometryCalculator.TerminalGeometryMetrics?

    private lazy var longPressRecognizer: UILongPressGestureRecognizer = {
        let recognizer = UILongPressGestureRecognizer(
            target: self,
            action: #selector(handleSelectionPress(_:))
        )
        recognizer.minimumPressDuration = 0.25
        recognizer.delegate = self
        return recognizer
    }()
    private lazy var tapRecognizer: UITapGestureRecognizer = {
        let recognizer = UITapGestureRecognizer(
            target: self,
            action: #selector(handleFocusTap(_:))
        )
        recognizer.cancelsTouchesInView = false
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
        terminalView.showsVerticalScrollIndicator = false
        terminalView.showsHorizontalScrollIndicator = false
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
        terminalView.delegate = self

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
        addGestureRecognizer(tapRecognizer)
        addGestureRecognizer(longPressRecognizer)

        selectionMenu.translatesAutoresizingMaskIntoConstraints = false
        selectionMenu.isHidden = true
        selectionMenu.copyHandler = { [weak self] in self?.copySelectionAction() }
        selectionMenu.copyAllHandler = { [weak self] in self?.copyAllAction() }
        selectionMenu.pasteHandler = { [weak self] in self?.pasteSelectionAction() }
        addSubview(selectionMenu)

        appearanceObserver = NotificationCenter.default.addObserver(
            forName: TerminalFontSettings.appearanceDidChangeNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            guard let self else { return }
            self.applyFont(font: TerminalFontSettings.shared.currentFont)
            self.configure(
                backgroundColor: TerminalFontSettings.shared.backgroundColor,
                foregroundColor: TerminalFontSettings.shared.foregroundColor
            )
        }

        modifierObserver = NotificationCenter.default.addObserver(
            forName: .terminalModifierStateChanged,
            object: nil,
            queue: .main
        ) { [weak self] notification in
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
        let increase1 = UIKeyCommand(
            input: "+",
            modifierFlags: [.command],
            action: #selector(handleIncreaseFont)
        )
        let increase2 = UIKeyCommand(
            input: "=",
            modifierFlags: [.command],
            action: #selector(handleIncreaseFont)
        )
        let decrease = UIKeyCommand(
            input: "-",
            modifierFlags: [.command],
            action: #selector(handleDecreaseFont)
        )

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

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil {
            DispatchQueue.main.async { [weak self] in
                _ = self?.becomeFirstResponder()
            }
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

        if bounds.width > 1,
           bounds.height > 1,
           let pending = pendingUpdate {
            pendingUpdate = nil
            update(text: pending.text,
                   cursor: pending.cursor,
                   backgroundColor: pending.backgroundColor,
                   isEditorMode: pending.isEditorMode,
                   editorSnapshot: pending.editorSnapshot)
        }
        reportGeometryIfNeeded()
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
        setNeedsLayout()
    }

    func applyEditorFont(_ font: UIFont) {
        terminalView.adjustsFontForContentSizeCategory = false
        terminalView.font = font
        terminalView.typingAttributes[.font] = font
        resolvedFont = font
        selectionOverlay.clearSelection()
        setNeedsLayout()
    }

    func currentFont() -> UIFont {
        terminalView.font ?? resolvedFont
    }

    @objc
    private func handleFocusTap(_ recognizer: UITapGestureRecognizer) {
        NotificationCenter.default.post(name: .terminalInputFocusRequested, object: nil)
    }

    // MARK: Selection handling

    @objc
    private func handleSelectionPress(_ recognizer: UILongPressGestureRecognizer) {
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

        let location = CGPoint(
            x: point.x + terminalView.contentOffset.x - terminalView.textContainerInset.left,
            y: point.y + terminalView.contentOffset.y - terminalView.textContainerInset.top
        )

        let characterIndex = terminalView.closestPosition(to: location).flatMap { pos in
            terminalView.offset(from: terminalView.beginningOfDocument, to: pos)
        } ?? 0

        let clamped = max(0, min(characterIndex, text.length))
        return clamped
    }

    private func reportGeometryIfNeeded() {
        let size = terminalView.bounds.size
        guard size.width > 0, size.height > 0 else { return }
        let grid = TerminalGeometryCalculator.calculateCapacity(for: size, font: resolvedFont)
        let metrics = TerminalGeometryCalculator.TerminalGeometryMetrics(
            columns: grid.columns,
            rows: grid.rows
        )
        if metrics != lastReportedGeometry {
            lastReportedGeometry = metrics
            onGeometryChange?(metrics.columns, metrics.rows)
        }
    }

    @objc
    private func copySelectionAction() {
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

    @objc
    private func copyAllAction() {
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

    @objc
    private func pasteSelectionAction() {
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
        let targetSize = selectionMenu.systemLayoutSizeFitting(
            UIView.layoutFittingCompressedSize
        )
        let menuSize = CGSize(
            width: max(72, targetSize.width),
            height: max(32, targetSize.height)
        )

        let preferredOrigin = CGPoint(
            x: anchorRect.maxX - menuSize.width,
            y: max(0, anchorRect.minY - menuSize.height - 8)
        )

        let maxX = bounds.width - menuSize.width - 8
        let clampedX = max(8, min(preferredOrigin.x, maxX))

        let maxY = bounds.height - menuSize.height - 8
        let clampedY = max(8, min(preferredOrigin.y, maxY))

        selectionMenu.frame = CGRect(
            origin: CGPoint(x: clampedX, y: clampedY),
            size: menuSize
        )
    }

    // MARK: Update text / cursor

    func update(text: NSAttributedString,
                cursor: TerminalCursorInfo?,
                backgroundColor: UIColor,
                isEditorMode: Bool,
                editorSnapshot: EditorSnapshot?) {
        if !Thread.isMainThread {
            let immutableInput = (text.copy() as? NSAttributedString) ??
                NSAttributedString(attributedString: text)
            DispatchQueue.main.async {
                self.update(text: immutableInput,
                            cursor: cursor,
                            backgroundColor: backgroundColor,
                            isEditorMode: isEditorMode,
                            editorSnapshot: editorSnapshot)
            }
            return
        }

        scrollbackEnabled = !isEditorMode
        if !scrollbackEnabled {
            autoScrollEnabled = true
        }
        terminalView.isScrollEnabled = scrollbackEnabled && (mouseMode == .none)

        if isEditorMode, let snapshot = editorSnapshot {
            applyEditorSnapshot(snapshot, backgroundColor: backgroundColor)
            return
        } else {
            lastEditorSnapshotText = nil
            lastEditorCursorOffset = nil
            selectionOverlay.clearSelection()

            let displayText = remapFontsIfNeeded(in: text)
            let immutableDisplayText = (displayText.copy() as? NSAttributedString) ??
                NSAttributedString(attributedString: displayText)

            if window == nil || bounds.width < 1 || bounds.height < 1 {
                pendingUpdate = (
                    immutableDisplayText,
                    cursor,
                    backgroundColor,
                    isEditorMode,
                    editorSnapshot
                )
                return
            }

            let previousOffset = terminalView.contentOffset
            let shouldAutoScroll = autoScrollEnabled

            terminalView.attributedText = immutableDisplayText
            terminalView.cursorInfo = cursor
            terminalView.backgroundColor = backgroundColor

            if let cursorOffset = cursor?.textOffset {
                terminalView.applyCursor(offset: cursorOffset)
                if shouldAutoScroll {
                    scrollToCursor(textView: terminalView,
                                   cursorOffset: cursorOffset)
                }
            } else if shouldAutoScroll {
                scrollToBottom(textView: terminalView, text: immutableDisplayText)
            }

            if !shouldAutoScroll {
                restoreScrollOffset(previousOffset, textView: terminalView)
            }
        }
    }

    private func applyEditorSnapshot(
        _ snapshot: EditorSnapshot,
        backgroundColor: UIColor
    ) {
        scrollbackEnabled = false
        autoScrollEnabled = true
        lastEditorSnapshotText = snapshot.text

        terminalView.backgroundColor = backgroundColor
        terminalView.textColor = TerminalFontSettings.shared.foregroundColor
        terminalView.cursorColor = TerminalFontSettings.shared.foregroundColor
        let immutableSnapshotText = (snapshot.attributedText.copy() as? NSAttributedString) ??
            NSAttributedString(attributedString: snapshot.attributedText)
        terminalView.attributedText = immutableSnapshotText

        selectionOverlay.clearSelection()

        var resolvedCursor: TerminalCursorInfo?
        var preferredInset: CGFloat = 0

        if let commandCursor = editorCommandLineCursor(from: snapshot) {
            resolvedCursor = commandCursor
            preferredInset = Self.commandLinePadding
        } else if let cursor = snapshot.cursor {
            resolvedCursor = cursor
        }

        terminalView.cursorInfo = resolvedCursor
        terminalView.applyCursor(offset: resolvedCursor?.textOffset)

        if let offset = resolvedCursor?.textOffset {
            if offset != lastEditorCursorOffset {
                if window == nil || bounds.width < 1 || bounds.height < 1 {
                    pendingUpdate = (
                        immutableSnapshotText,
                        resolvedCursor,
                        backgroundColor,
                        true,
                        snapshot
                    )
                    return
                }

                scrollToCursor(textView: terminalView,
                               cursorOffset: offset,
                               cursorRow: resolvedCursor?.row,
                               preferBottomInset: preferredInset)
                lastEditorCursorOffset = offset
            }
        } else {
            terminalView.cursorInfo = nil
            terminalView.applyCursor(offset: nil)
            lastEditorCursorOffset = nil
            scrollToBottom(textView: terminalView, text: terminalView.attributedText)
        }
    }

    func scrollViewDidScroll(_ scrollView: UIScrollView) {
        guard scrollbackEnabled else {
            autoScrollEnabled = true
            return
        }
        if scrollView.isTracking || scrollView.isDragging || scrollView.isDecelerating {
            autoScrollEnabled = isAtBottom(textView: terminalView)
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
        let yOffset = max(
            -textView.contentInset.top,
            contentHeight - boundsHeight + textView.contentInset.bottom
        )

        if yOffset.isFinite {
            textView.setContentOffset(
                CGPoint(x: -textView.contentInset.left, y: yOffset),
                animated: false
            )
        }
    }

    private func restoreScrollOffset(_ offset: CGPoint, textView: UITextView) {
        textView.layoutIfNeeded()
        let contentHeight = textView.contentSize.height
        let boundsHeight = textView.bounds.height
        let maxOffset = max(
            -textView.contentInset.top,
            contentHeight - boundsHeight + textView.contentInset.bottom
        )
        let clampedY = min(max(offset.y, -textView.contentInset.top), maxOffset)
        textView.setContentOffset(CGPoint(x: offset.x, y: clampedY), animated: false)
    }

    private func isAtBottom(textView: UITextView) -> Bool {
        textView.layoutIfNeeded()
        let contentHeight = textView.contentSize.height
        let boundsHeight = textView.bounds.height
        let maxOffset = max(
            -textView.contentInset.top,
            contentHeight - boundsHeight + textView.contentInset.bottom
        )
        let threshold = max(4.0, TerminalFontMetrics.lineHeight)
        return (maxOffset - textView.contentOffset.y) <= threshold
    }

    private func scrollToCursor(
        textView: UITextView,
        cursorOffset: Int,
        cursorRow: Int? = nil,
        preferBottomInset: CGFloat = 0
    ) {
        let length = textView.attributedText.length
        guard length > 0 else { return }

        let safeOffset = max(0, min(cursorOffset, length))

        let adjustedTop = textView.adjustedContentInset.top
        let adjustedBottom = textView.adjustedContentInset.bottom
        let viewportHeight = max(1, textView.bounds.height - adjustedTop - adjustedBottom)
        let visibleTop = textView.contentOffset.y + adjustedTop
        let visibleBottom = visibleTop + viewportHeight

        let inset = max(0, preferBottomInset)
        let targetTop: CGFloat
        let targetBottom: CGFloat
        if let cursorRow {
            let rowHeight = max(1, TerminalFontMetrics.lineHeight)
            let rowY = CGFloat(max(0, cursorRow)) * rowHeight + textView.textContainerInset.top
            targetTop = rowY
            targetBottom = rowY + rowHeight + inset
        } else {
            let range = NSRange(location: safeOffset, length: 0)
            textView.scrollRangeToVisible(range)
            return
        }

        var nextOffsetY = textView.contentOffset.y
        if targetTop < visibleTop {
            nextOffsetY = targetTop - adjustedTop
        } else if targetBottom > visibleBottom {
            nextOffsetY = targetBottom - viewportHeight - adjustedTop
        } else {
            return
        }

        let maxOffsetY = max(
            -adjustedTop,
            textView.contentSize.height - textView.bounds.height + adjustedBottom
        )
        nextOffsetY = min(max(nextOffsetY, -adjustedTop), maxOffsetY)
        if abs(nextOffsetY - textView.contentOffset.y) > 0.5 {
            textView.setContentOffset(
                CGPoint(x: textView.contentOffset.x, y: nextOffsetY),
                animated: false
            )
        }
    }

    private func remapFontsIfNeeded(in text: NSAttributedString) -> NSAttributedString {
        guard text.length > 0 else { return text }

        let baseFont = currentFont()

        var needsRemap = false
        let fullRange = NSRange(location: 0, length: text.length)

        text.enumerateAttribute(.font, in: fullRange, options: []) { value, _, stop in
            if let existing = value as? UIFont {
                if existing.familyName != baseFont.familyName {
                    needsRemap = true
                    stop.pointee = true
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

            let replacement = TerminalFontSettings.shared.font(
                forPointSize: baseFont.pointSize,
                weight: weight,
                italic: italic
            )
            mutable.addAttribute(.font, value: replacement, range: range)
        }
        return mutable
    }

    // MARK: Mouse tracking

    func updateMouseState(
        mode: TerminalBuffer.MouseMode,
        encoding: TerminalBuffer.MouseEncoding
    ) {
        self.mouseMode = mode
        self.mouseEncoding = encoding

        terminalView.isScrollEnabled = scrollbackEnabled && (mode == .none)
    }

    private func editorCommandLineCursor(
        from snapshot: EditorSnapshot
    ) -> TerminalCursorInfo? {
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
            return TerminalCursorInfo(row: rowIndex,
                                      column: columnUTF16,
                                      textOffset: textOffset)
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

        if let last = lastMouseLocation,
           last.row == row,
           last.col == col {
            return
        }
        lastMouseLocation = (row, col)

        // Button 32 (Drag/Motion) + 0 (Left), Action 'M'
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

        let x = point.x - terminalView.textContainerInset.left
        let y = point.y - terminalView.textContainerInset.top

        let col = Int(floor(x / charWidth))
        let row = Int(floor(y / lineHeight))

        return (max(0, col), max(0, row))
    }

    private func sendSGRMouse(button: Int, x: Int, y: Int, pressed: Bool) {
        guard mouseEncoding == .sgr else { return }
        guard let onInput else { return }

        let suffix = pressed ? "M" : "m"
        let sequence = "\u{1B}[<\(button);\(x);\(y)\(suffix)"
        onInput(sequence)
    }

    private static let commandLinePrefixes: Set<Character> = [":", "/", "?"]
    private static let commandLinePadding: CGFloat = 8.0
}

final class TerminalDisplayTextView: UITextView {
    var cursorInfo: TerminalCursorInfo?
    var cursorTextOffset: Int? {
        didSet { updateCursorLayer() }
    }

    var pasteHandler: ((String) -> Void)?

    var cursorColor: UIColor = .systemOrange {
        didSet {
            tintColor = cursorColor
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
    private var pendingCursorUpdate = false
    private var lastAppliedCursorOffset: Int?
    private var legacyTextStorage: NSTextStorage?
    private var legacyLayoutManager: NSLayoutManager?

    override init(frame: CGRect, textContainer: NSTextContainer?) {
        var effectiveContainer = textContainer
        var legacyStorage: NSTextStorage?
        var legacyLayout: NSLayoutManager?
        if effectiveContainer == nil {
            let storage = NSTextStorage()
            let layoutManager = NSLayoutManager()
            let container = NSTextContainer(size: .zero)
            container.widthTracksTextView = true
            container.heightTracksTextView = false
            container.lineFragmentPadding = 0
            storage.addLayoutManager(layoutManager)
            layoutManager.addTextContainer(container)
            effectiveContainer = container
            legacyStorage = storage
            legacyLayout = layoutManager
        }
        super.init(frame: frame, textContainer: effectiveContainer)
        legacyTextStorage = legacyStorage
        legacyLayoutManager = legacyLayout

        if #available(iOS 16.0, *) {
            // Force TextKit 1 for terminal rendering stability under heavy output.
            // Accessing layoutManager triggers fallback when TextKit 2 is active.
            _ = layoutManager
        }

        layer.addSublayer(cursorLayer)

        isEditable = false
        isSelectable = false
        isScrollEnabled = true
        showsVerticalScrollIndicator = false
        showsHorizontalScrollIndicator = false
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

    func applyCursor(offset: Int?) {
        let clampedOffset = offset.map { max(0, min($0, attributedText.length)) }
        if cursorTextOffset != clampedOffset {
            cursorTextOffset = clampedOffset
        }

        guard let clampedOffset else {
            lastAppliedCursorOffset = nil
            return
        }

        guard clampedOffset != lastAppliedCursorOffset else {
            return
        }
        lastAppliedCursorOffset = clampedOffset

        // Selection updates are expensive in TextKit and this view is not editable.
        // Only mirror cursor into selection when actively focused.
        if isFirstResponder {
            selectedRange = NSRange(location: clampedOffset, length: 0)
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

    private func updateCursorLayer() {
        let storageHasPendingEdits = textStorage.editedRange.location != NSNotFound ||
            textStorage.editedMask.rawValue != 0
        if storageHasPendingEdits {
            if !pendingCursorUpdate {
                pendingCursorUpdate = true
                DispatchQueue.main.async { [weak self] in
                    guard let self = self else { return }
                    self.pendingCursorUpdate = false
                    self.updateCursorLayer()
                }
            }
            return
        }

        guard let offset = cursorTextOffset,
              attributedText.length > 0 else {
            cursorLayer.opacity = 0
            return
        }

        let clamped = max(0, min(offset, attributedText.length))
        guard let pos = position(from: beginningOfDocument, offset: clamped) else {
            cursorLayer.opacity = 0
            return
        }

        let caret = caretRect(for: pos)
        var rect = caret
        rect.size.width = max(1, rect.size.width)
        rect.size.height = max(rect.size.height, TerminalFontMetrics.lineHeight)

        CATransaction.begin()
        CATransaction.setDisableActions(true)
        cursorLayer.frame = rect
        CATransaction.commit()

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

// MARK: - Floating Editor Renderer

struct EditorFloatingRendererView: View {
    @ObservedObject private var manager = TerminalTabManager.shared

    var body: some View {
        EditorFloatingRendererContent(runtime: activeRuntime())
    }

    private func activeRuntime() -> PscalRuntimeBootstrap {
        for tab in manager.tabs {
            if case .shell(let runtime) = tab.kind, runtime.editorBridge.isActive {
                return runtime
            }
        }
        if case .shell(let runtime) = manager.selectedTab.kind {
            return runtime
        }
        return PscalRuntimeBootstrap.shared
    }
}

private struct EditorFloatingRendererContent: View {
    @ObservedObject var runtime: PscalRuntimeBootstrap
    @ObservedObject private var fontSettings = TerminalFontSettings.shared

    var body: some View {
        let token = runtime.editorRenderToken
        _ = token
        let snapshot = runtime.editorSnapshot

        return TerminalRendererView(
            text: snapshot.attributedText,
            cursor: snapshot.cursor,
            backgroundColor: fontSettings.backgroundColor,
            foregroundColor: fontSettings.foregroundColor,
            isEditorMode: true,
            isEditorWindowVisible: false,
            editorRenderToken: token,
            font: fontSettings.currentFont,
            fontPointSize: fontSettings.pointSize,
            editorSnapshot: snapshot,
            onInput: runtime.send,
            mouseMode: runtime.mouseMode,
            mouseEncoding: runtime.mouseEncoding
        )
        .background(Color(fontSettings.backgroundColor))
    }
}
