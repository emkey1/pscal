import SwiftUI
import UIKit

@_silgen_name("pscalRuntimeRequestSigint")
func pscalRuntimeRequestSigint()
@_silgen_name("pscalRuntimeRequestSigtstp")
func pscalRuntimeRequestSigtstp()

extension Notification.Name {
    static let terminalModifierStateChanged = Notification.Name("terminalModifierStateChanged")
    static let terminalInputFocusRequested = Notification.Name("terminalInputFocusRequested")
}

@MainActor
struct TerminalInputBridge: UIViewRepresentable {
    @Binding var focusAnchor: Int
    var onInput: (String) -> Void
    var onPaste: ((String) -> Void)?
    var onCopy: (() -> Void)?
    var onInterrupt: (() -> Void)? = nil
    var onSuspend: (() -> Void)? = nil
    var onNewTab: (() -> Void)? = nil
    var onCloseTab: (() -> Void)? = nil

    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    func makeUIView(context: Context) -> TerminalKeyInputView {
        let view = TerminalKeyInputView()
        view.backgroundColor = .clear
        view.textColor = .clear
        view.tintColor = .clear
        view.isScrollEnabled = false
        view.text = ""
        view.autocorrectionType = .no
        view.autocapitalizationType = .none
        view.spellCheckingType = .no
        view.smartQuotesType = .no
        view.smartInsertDeleteType = .no
        view.smartDashesType = .no
        view.keyboardType = .asciiCapable
        if #available(iOS 17.0, *) {
            view.inlinePredictionType = .no
        }
        view.keyboardAppearance = .dark
        view.onInput = onInput
        view.onPaste = onPaste
        view.onCopy = onCopy
        view.onInterrupt = onInterrupt
        view.onSuspend = onSuspend
        view.onNewTab = onNewTab
        view.onCloseTab = onCloseTab
        context.coordinator.view = view

        // Remove default copy/paste/undo bar
        view.inputAssistantItem.leadingBarButtonGroups = []
        view.inputAssistantItem.trailingBarButtonGroups = []

        DispatchQueue.main.async {
            view.becomeFirstResponder()
        }
        return view
    }

    func updateUIView(_ uiView: TerminalKeyInputView, context: Context) {
        let needsInvalidation =
            ((uiView.onCopy == nil) != (onCopy == nil)) ||
            ((uiView.onNewTab == nil) != (onNewTab == nil)) ||
            ((uiView.onCloseTab == nil) != (onCloseTab == nil))

        uiView.onInput = onInput
        uiView.onPaste = onPaste
        uiView.onCopy = onCopy
        uiView.onInterrupt = onInterrupt
        uiView.onSuspend = onSuspend
        uiView.onNewTab = onNewTab
        uiView.onCloseTab = onCloseTab
        if needsInvalidation {
            uiView.invalidateKeyCommandsCache()
        }

        if context.coordinator.focusAnchor != focusAnchor {
            context.coordinator.focusAnchor = focusAnchor
            DispatchQueue.main.async {
                uiView.becomeFirstResponder()
            }
        }
    }

    @MainActor
    final class Coordinator {
        weak var view: TerminalKeyInputView?
        var focusAnchor: Int = 0
    }
}

@MainActor
final class TerminalKeyInputView: UITextView {
    var onInput: ((String) -> Void)?
    var onPaste: ((String) -> Void)?
    var onCopy: (() -> Void)?
    var onInterrupt: (() -> Void)?
    var onSuspend: (() -> Void)?
    var onNewTab: (() -> Void)?
    var onCloseTab: (() -> Void)?
    var onFocusChange: ((Bool) -> Void)?
    var inputEnabled: Bool = true {
        didSet {
            if !inputEnabled && isFirstResponder {
                _ = resignFirstResponder()
            }
        }
    }
    var applicationCursorEnabled: Bool = false
    private var hardwareKeyboardConnected: Bool = false
    private var softKeyboardVisible: Bool = false
    private var keyboardObservers: [NSObjectProtocol] = []
    private let hardwareKeyboardHeightEpsilon: CGFloat = 80.0
    private var cachedKeyCommands: [UIKeyCommand]?
    private var suppressNextPlainDotInsert: Bool = false

    private func isHardwareKeyboard(_ notification: Notification) -> Bool {
        guard let userInfo = notification.userInfo,
              let frameValue = userInfo[UIResponder.keyboardFrameEndUserInfoKey] as? NSValue,
              let window = window else {
            return false
        }
        let frameInWindow = window.convert(frameValue.cgRectValue, from: nil)
        return frameInWindow.height < hardwareKeyboardHeightEpsilon
    }
    
    override init(frame: CGRect, textContainer: NSTextContainer?) {
        self.accessoryBar = UIInputView(frame: .zero, inputViewStyle: .keyboard)
        super.init(frame: frame, textContainer: textContainer)
        configureAccessoryBar()
        installKeyboardObservers()
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        self.accessoryBar = UIInputView(frame: .zero, inputViewStyle: .keyboard)
        super.init(coder: coder)
        configureAccessoryBar()
        installKeyboardObservers()
    }

    deinit {
        keyboardObservers.forEach { NotificationCenter.default.removeObserver($0) }
    }

    private var controlLatch: Bool = false
    private weak var ctrlButton: UIButton?

    // MARK: - FIXED ACCESSORY BAR
    private let accessoryBar: UIInputView

    override var inputAccessoryView: UIView? {
        get {
            (isFirstResponder && softKeyboardVisible && !hardwareKeyboardConnected) ? accessoryBar : nil
        }
        set { /* ignore external setters */ }
    }


    private func configureAccessoryBar() {
        accessoryBar.allowsSelfSizing = true
        accessoryBar.translatesAutoresizingMaskIntoConstraints = false

        let stack = UIStackView()
        stack.axis = .horizontal
        stack.alignment = .fill
        stack.distribution = .fillEqually
        stack.spacing = 8
        stack.translatesAutoresizingMaskIntoConstraints = false

        func makeButton(_ title: String, action: Selector) -> UIButton {
            let button = UIButton(type: .system)
            var config = UIButton.Configuration.filled()
            config.cornerStyle = .medium
            config.baseBackgroundColor = UIColor.secondarySystemBackground.withAlphaComponent(0.8)
            config.baseForegroundColor = .label
            config.title = title
            let isPhone = UIDevice.current.userInterfaceIdiom == .phone
            config.contentInsets = NSDirectionalEdgeInsets(
                top:   isPhone ? 3 : 6,
                leading: 4,
                bottom: isPhone ? 3 : 6,
                trailing: 4
            )
            button.configuration = config
            button.titleLabel?.font = UIFontMetrics.default.scaledFont(for: .systemFont(ofSize: 15, weight: .semibold))
            button.addTarget(self, action: action, for: .touchUpInside)
            return button
        }

        let esc = makeButton("\u{238B}", action: #selector(handleEscAction))
        let ctrl = makeButton("^", action: #selector(handleCtrlToggle))
        ctrlButton = ctrl
        let up = makeButton("↑", action: #selector(handleUpAction))
        let down = makeButton("↓", action: #selector(handleDownAction))
        let left = makeButton("←", action: #selector(handleLeftAction))
        let right = makeButton("→", action: #selector(handleRightAction))
        let fslash = makeButton("/", action: #selector(handleFSlashAction))
        let dot = makeButton(".", action: #selector(handleDotAction))
        let minus = makeButton("-", action: #selector(handleMinusAction))
        let pipe = makeButton("|", action: #selector(handlePipeAction))

        if UIDevice.current.userInterfaceIdiom == .phone {
            // Replaced "Tab" with the Unicode symbol \u{21E5}
            let tab = makeButton("\u{21E5}", action: #selector(handleTab))
            [esc, ctrl, tab, dot, fslash, pipe, minus, up, down, left, right].forEach(stack.addArrangedSubview)
        } else {
            [esc, ctrl, dot, fslash, pipe, minus, up, down, left, right].forEach(stack.addArrangedSubview)
        }

        accessoryBar.addSubview(stack)
        let minHeight: CGFloat = UIDevice.current.userInterfaceIdiom == .phone ? 30 : 45
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: accessoryBar.safeAreaLayoutGuide.leadingAnchor, constant: 8),
            stack.trailingAnchor.constraint(equalTo: accessoryBar.safeAreaLayoutGuide.trailingAnchor, constant: -8),
            stack.topAnchor.constraint(equalTo: accessoryBar.topAnchor, constant: 4),
            stack.bottomAnchor.constraint(equalTo: accessoryBar.bottomAnchor, constant: -4),
            accessoryBar.heightAnchor.constraint(greaterThanOrEqualToConstant: minHeight)
        ])
    }

    override var canBecomeFirstResponder: Bool {
        inputEnabled && super.canBecomeFirstResponder
    }

    override func becomeFirstResponder() -> Bool {
        guard inputEnabled else { return false }
        let became = super.becomeFirstResponder()
        if became {
            onFocusChange?(true)
        }
        return became
    }

    override func resignFirstResponder() -> Bool {
        let resigned = super.resignFirstResponder()
        if resigned {
            onFocusChange?(false)
        }
        return resigned
    }

    override var keyCommands: [UIKeyCommand]? {
        if let cache = cachedKeyCommands {
            return cache
        }
        var commands: [UIKeyCommand] = []
        let increase1 = UIKeyCommand(input: "+", modifierFlags: [.command], action: #selector(handleIncreaseFont))
        let increase2 = UIKeyCommand(input: "=", modifierFlags: [.command], action: #selector(handleIncreaseFont))
        let decrease = UIKeyCommand(input: "-", modifierFlags: [.command], action: #selector(handleDecreaseFont))
        if #available(iOS 15.0, *) {
            increase1.wantsPriorityOverSystemBehavior = true
            increase2.wantsPriorityOverSystemBehavior = true
            decrease.wantsPriorityOverSystemBehavior = true
        }
        commands.append(contentsOf: [increase1, increase2, decrease])
        commands.append(UIKeyCommand(input: "v",
                                     modifierFlags: [.command],
                                     action: #selector(handlePasteCommand)))
        if onCopy != nil {
            let copy = UIKeyCommand(input: "c",
                                    modifierFlags: [.command],
                                    action: #selector(handleCopyCommand(_:)))
            let cut = UIKeyCommand(input: "x",
                                   modifierFlags: [.command],
                                   action: #selector(handleCopyCommand(_:)))
            if #available(iOS 15.0, *) {
                copy.wantsPriorityOverSystemBehavior = true
                cut.wantsPriorityOverSystemBehavior = true
            }
            commands.append(contentsOf: [copy, cut])
        }
        if onNewTab != nil {
            let newTab = UIKeyCommand(input: "t",
                                      modifierFlags: [.command],
                                      action: #selector(handleNewTabCommand))
            if #available(iOS 15.0, *) {
                newTab.wantsPriorityOverSystemBehavior = true
            }
            commands.append(newTab)
        }
        if onCloseTab != nil {
            let closeTab = UIKeyCommand(input: "w",
                                        modifierFlags: [.command],
                                        action: #selector(handleCloseTabCommand))
            if #available(iOS 15.0, *) {
                closeTab.wantsPriorityOverSystemBehavior = true
            }
            commands.append(closeTab)
        }
        cachedKeyCommands = commands
        return commands
    }
    
    func invalidateKeyCommandsCache() {
        cachedKeyCommands = nil
    }

    override func insertText(_ text: String) {
        var normalizedText = text
        // Keep modal editor command input stable on iOS keyboards that can
        // autocorrect "..." to a single ellipsis codepoint.
        if normalizedText.contains("\u{2026}") {
            normalizedText = normalizedText.replacingOccurrences(of: "\u{2026}", with: ".")
        }
        if let translated = translatedUIKitInputToken(normalizedText) {
            onInput?(translated)
            return
        }
        if normalizedText.hasPrefix("UIKeyInput") {
            return
        }

        if normalizedText == "." {
            if suppressNextPlainDotInsert {
                suppressNextPlainDotInsert = false
                return
            }
        }

        if controlLatch, let scalar = normalizedText.unicodeScalars.first {
            controlLatch = false
            let value = scalar.value
            if scalar == "c" {
                triggerInterrupt()
                return
            }
            if scalar == "z" {
                triggerSuspend()
                return
            }
            if value >= 0x40, value <= 0x7F,
               let ctrlScalar = UnicodeScalar(value & 0x1F) {
                onInput?(String(ctrlScalar))
                return
            }
        }
        let normalized = normalizedText.replacingOccurrences(of: "\n", with: "\r")
        onInput?(normalized)
    }

    override func deleteBackward() {
        onInput?("\u{7F}")
    }

    override func paste(_ sender: Any?) {
        guard let text = UIPasteboard.general.string, !text.isEmpty else { return }
        onPaste?(text)
    }

    override func copy(_ sender: Any?) {
        if let onCopy {
            onCopy()
            return
        }
        super.copy(sender)
    }

    override func cut(_ sender: Any?) {
        if let onCopy {
            onCopy()
            return
        }
        super.cut(sender)
    }

    override func canPerformAction(_ action: Selector, withSender sender: Any?) -> Bool {
        if action == #selector(paste(_:)) {
            return UIPasteboard.general.hasStrings
        }
        if action == #selector(copy(_:)) || action == #selector(cut(_:)) {
            return onCopy != nil
        }
        return super.canPerformAction(action, withSender: sender)
    }

    override func caretRect(for position: UITextPosition) -> CGRect { .zero }

    override func selectionRects(for range: UITextRange) -> [UITextSelectionRect] { [] }

    override var selectedTextRange: UITextRange? {
        get { nil }
        set { }
    }

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        if !hardwareKeyboardConnected {
            hardwareKeyboardConnected = true
            if softKeyboardVisible {
                reloadInputViews()
            }
        }
        if let event {
            NotificationCenter.default.post(name: .terminalModifierStateChanged,
                                            object: nil,
                                            userInfo: ["command": event.modifierFlags.contains(.command)])
        }
        // Keep UIKit out of hardware key dispatch to avoid key-event crashes
        // observed after pager/editor transitions. We route unknown keys through
        // insertText so normal text input still works.
        for press in presses {
            if handle(press: press) {
                continue
            }
            guard let key = press.key else { continue }
            if key.keyCode == .keyboardDeleteOrBackspace {
                deleteBackward()
                continue
            }
            let chars = key.characters
            if !chars.isEmpty {
                insertText(chars)
            }
        }
    }

    override func pressesChanged(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        if let event {
            NotificationCenter.default.post(name: .terminalModifierStateChanged,
                                            object: nil,
                                            userInfo: ["command": event.modifierFlags.contains(.command)])
        }
        // Key-down is sufficient for terminal input.
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        NotificationCenter.default.post(name: .terminalModifierStateChanged,
                                        object: nil,
                                        userInfo: ["command": false])
        // Swallow key-up events to avoid UIKit key replay paths.
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        NotificationCenter.default.post(name: .terminalModifierStateChanged,
                                        object: nil,
                                        userInfo: ["command": false])
        // Swallow cancelled key events for the same reason as key-up events.
    }

    private func arrowSequence(_ direction: String) -> String {
        let prefix = applicationCursorEnabled ? "\u{1B}O" : "\u{1B}["
        return prefix + direction
    }

    private func translatedUIKitInputToken(_ input: String) -> String? {
        switch input {
        case UIKeyCommand.inputUpArrow, "UIKeyInputUpArrow", "UIKeyInputUp":
            return applicationCursorEnabled ? "k" : arrowSequence("A")
        case UIKeyCommand.inputDownArrow, "UIKeyInputDownArrow", "UIKeyInputDown":
            return applicationCursorEnabled ? "j" : arrowSequence("B")
        case UIKeyCommand.inputLeftArrow, "UIKeyInputLeftArrow", "UIKeyInputLeft":
            return applicationCursorEnabled ? "h" : arrowSequence("D")
        case UIKeyCommand.inputRightArrow, "UIKeyInputRightArrow", "UIKeyInputRight":
            return applicationCursorEnabled ? "l" : arrowSequence("C")
        case UIKeyCommand.inputEscape, "UIKeyInputEscape":
            return "\u{1B}"
        default:
            return nil
        }
    }

    private func handle(press: UIPress) -> Bool {
        guard let key = press.key else { return false }

        // Route plain '.' directly from key press events so nextvi repeat ('.')
        // does not depend on UITextInput session state.
        if (key.characters == "." || key.characters == "\u{2026}"),
           key.modifierFlags.intersection([.control, .alternate, .command, .alphaShift, .shift]).isEmpty {
            if press.phase == .began {
                suppressNextPlainDotInsert = true
                onInput?(".")
                DispatchQueue.main.async { [weak self] in
                    self?.suppressNextPlainDotInsert = false
                }
            }
            return true
        }

        if key.modifierFlags.contains(.command) {
            let chars = key.charactersIgnoringModifiers
            switch chars {
            case "+", "=":
                handleIncreaseFont()
                return true
            case "-":
                handleDecreaseFont()
                return true
            default:
                break
            }
        }

        if key.keyCode == .keyboardTab {
            if key.modifierFlags.contains(.shift) {
                onInput?("\u{1B}[Z")
            } else {
                onInput?("\t")
            }
            return true
        }

        if key.modifierFlags.contains(.control),
           let scalar = key.charactersIgnoringModifiers.lowercased().unicodeScalars.first {
            let value = scalar.value
            if scalar == "c" {
                triggerInterrupt()
                return true
            }
            if scalar == "z" {
                triggerSuspend()
                return true
            }
            if value >= 0x40, value <= 0x7F,
               let ctrlScalar = UnicodeScalar(value & 0x1F) {
                onInput?(String(ctrlScalar))
                return true
            }
        }

        switch key.keyCode {
        case .keyboardUpArrow:
            if applicationCursorEnabled {
                onInput?("k")
            } else {
                onInput?(arrowSequence("A"))
            }
            return true
        case .keyboardDownArrow:
            if applicationCursorEnabled {
                onInput?("j")
            } else {
                onInput?(arrowSequence("B"))
            }
            return true
        case .keyboardLeftArrow:
            if applicationCursorEnabled {
                onInput?("h")
            } else {
                onInput?(arrowSequence("D"))
            }
            return true
        case .keyboardRightArrow:
            if applicationCursorEnabled {
                onInput?("l")
            } else {
                onInput?(arrowSequence("C"))
            }
            return true
        case .keyboardReturnOrEnter:
            onInput?("\r"); return true
        case .keyboardDeleteOrBackspace:
            deleteBackward(); return true
        case .keyboardDeleteForward:
            onInput?("\u{1B}[3~"); return true
        case .keyboardEscape:
            onInput?("\u{1B}"); return true
        default:
            break
        }

        // Consume plain text-producing hardware key presses directly so we do
        // not depend on UIKit's text-input replay path after pager/editor mode
        // transitions.
        if key.modifierFlags.intersection([.command, .control]).isEmpty {
            let text = key.characters.replacingOccurrences(of: "\n", with: "\r")
            if !text.isEmpty, !text.hasPrefix("UIKeyInput") {
                onInput?(text)
                return true
            }
        }

        // Let keyCommands handle remaining command-key shortcuts.
        return false
    }

    private func installKeyboardObservers() {
        let center = NotificationCenter.default
        let willShow = center.addObserver(
            forName: UIResponder.keyboardWillShowNotification,
            object: nil,
            queue: .main
        ) { [weak self] notification in
            guard let self else { return }
            Task { @MainActor in
                guard self.inputEnabled else { return }
                guard let window = self.window, window.isKeyWindow else { return }
                let isHardware = self.isHardwareKeyboard(notification)
                if isHardware {
                    self.hardwareKeyboardConnected = true
                    self.softKeyboardVisible = false
                    if self.isFirstResponder {
                        self.reloadInputViews()
                    }
                    return
                }
                if self.softKeyboardVisible { return }
                self.softKeyboardVisible = true
                self.hardwareKeyboardConnected = false
                if self.isFirstResponder {
                    self.reloadInputViews()
                    self.onInput?(" ")
                    self.onInput?("\u{7F}")
                }
            }
        }

        let willHide = center.addObserver(
            forName: UIResponder.keyboardWillHideNotification,
            object: nil,
            queue: .main
        ) { [weak self] notification in
            guard let self else { return }
            Task { @MainActor in
                guard self.inputEnabled else { return }
                guard let window = self.window, window.isKeyWindow else { return }
                let isHardware = self.isHardwareKeyboard(notification)
                if isHardware {
                    self.hardwareKeyboardConnected = true
                    self.softKeyboardVisible = false
                    if self.isFirstResponder {
                        self.reloadInputViews()
                    }
                    return
                }
                guard self.softKeyboardVisible else { return }
                self.softKeyboardVisible = false
                if self.isFirstResponder {
                    self.reloadInputViews()
                    self.onInput?(" ")
                    self.onInput?("\u{7F}")
                }
            }
        }

        let focusRequested = center.addObserver(
            forName: .terminalInputFocusRequested,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            guard let self else { return }
            Task { @MainActor in
                guard self.inputEnabled else { return }
                guard let window = self.window, window.isKeyWindow else { return }
                if !self.isFirstResponder {
                    self.becomeFirstResponder()
                } else if !self.softKeyboardVisible {
                    self.reloadInputViews()
                }
            }
        }

        keyboardObservers = [willShow, willHide, focusRequested]
    }

    // MARK: - Accessory button actions
    @objc private func handleEscAction() {
        onInput?("\u{1B}")
    }
    
    @objc private func handleTab() {
        onInput?("\t")
    }

    @objc private func handleCtrlToggle(_ sender: UIButton) {
        controlLatch.toggle()
        var config = sender.configuration ?? UIButton.Configuration.filled()
        config.baseBackgroundColor = controlLatch
        ? UIColor.systemBlue.withAlphaComponent(0.8)
        : UIColor.secondarySystemBackground.withAlphaComponent(0.8)
        sender.configuration = config
    }

    @objc private func handleUpAction() {
        onInput?(arrowSequence("A"))
    }

    @objc private func handleDownAction() {
        onInput?(arrowSequence("B"))
    }

    @objc private func handleLeftAction() {
        onInput?(arrowSequence("D"))
    }

    @objc private func handleRightAction() {
        onInput?(arrowSequence("C"))
    }

    @objc private func handleFSlashAction() {
        onInput?("/")
    }

    @objc private func handleDotAction() {
        onInput?(".")
    }

    @objc private func handleMinusAction() {
        onInput?("-")
    }
   
    @objc private func handlePipeAction() {
        onInput?("|")
    }
    
    @objc private func handlePasteCommand() {
        guard let text = UIPasteboard.general.string, !text.isEmpty else { return }
        onPaste?(text)
    }

    @objc private func handleCopyCommand(_ command: UIKeyCommand) {
        onCopy?()
    }

    @objc private func handleNewTabCommand() {
        onNewTab?()
    }

    @objc private func handleCloseTabCommand() {
        onCloseTab?()
    }

    @objc private func handleIncreaseFont() {
        let settings = TerminalTabManager.shared.selectedAppearanceSettings
        settings.updatePointSize(settings.pointSize + 1.0)
    }

    @objc private func handleDecreaseFont() {
        let settings = TerminalTabManager.shared.selectedAppearanceSettings
        settings.updatePointSize(settings.pointSize - 1.0)
    }

    private func triggerInterrupt() {
        if let onInterrupt {
            onInterrupt()
        } else {
            pscalRuntimeRequestSigint()
        }
    }

    private func triggerSuspend() {
        if let onSuspend {
            onSuspend()
        } else {
            pscalRuntimeRequestSigtstp()
        }
    }
}
