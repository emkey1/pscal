import SwiftUI
import UIKit

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
            guard view.window?.isKeyWindow == true else { return }
            guard pscalIOSSDLModeActive() == 0 else { return }
            guard !TerminalKeyInputView.hasVisibleSDLWindow() else { return }
            guard !TerminalTabManager.shared.isSdlTabSelected else { return }
            _ = view.becomeFirstResponder()
        }
        return view
    }

    func updateUIView(_ uiView: TerminalKeyInputView, context: Context) {
        uiView.onInput = onInput
        uiView.onPaste = onPaste
        uiView.onCopy = onCopy
        uiView.onInterrupt = onInterrupt
        uiView.onSuspend = onSuspend
        uiView.onNewTab = onNewTab
        uiView.onCloseTab = onCloseTab

        if context.coordinator.focusAnchor != focusAnchor {
            context.coordinator.focusAnchor = focusAnchor
            DispatchQueue.main.async {
                guard uiView.window?.isKeyWindow == true else { return }
                guard pscalIOSSDLModeActive() == 0 else { return }
                guard !TerminalKeyInputView.hasVisibleSDLWindow() else { return }
                guard !TerminalTabManager.shared.isSdlTabSelected else { return }
                _ = uiView.becomeFirstResponder()
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

    fileprivate static func hasVisibleSDLWindow() -> Bool {
        let allWindows = UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .flatMap { $0.windows }
        return allWindows.contains { window in
                guard !window.isHidden else { return false }
                let className = NSStringFromClass(type(of: window)).lowercased()
                return className.contains("sdl") && className.contains("window")
            }
    }

    private func isHardwareKeyboard(_ notification: Notification) -> Bool {
        guard let userInfo = notification.userInfo,
              let frameValue = userInfo[UIResponder.keyboardFrameEndUserInfoKey] as? NSValue,
              let window = window else {
            return false
        }
        let frameInWindow = window.convert(frameValue.cgRectValue, from: nil)
        return frameInWindow.height < hardwareKeyboardHeightEpsilon
    }
    
    private struct RepeatCommand {
        let command: UIKeyCommand
        let output: String
    }

    override init(frame: CGRect, textContainer: NSTextContainer?) {
        self.accessoryBar = UIInputView(frame: .zero, inputViewStyle: .keyboard)
        self.repeatKeyCommands = []
        super.init(frame: frame, textContainer: textContainer)
        configureAccessoryBar()
        repeatKeyCommands = buildRepeatCommands()
        installKeyboardObservers()
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        self.accessoryBar = UIInputView(frame: .zero, inputViewStyle: .keyboard)
        self.repeatKeyCommands = []
        super.init(coder: coder)
        configureAccessoryBar()
        repeatKeyCommands = buildRepeatCommands()
        installKeyboardObservers()
    }

    deinit {
        keyboardObservers.forEach { NotificationCenter.default.removeObserver($0) }
    }

    private var controlLatch: Bool = false {
        didSet {
            updateCtrlButtonAppearance()
        }
    }
    private weak var ctrlButton: UIButton?

    // MARK: - FIXED ACCESSORY BAR
    private let accessoryBar: UIInputView

    override var inputAccessoryView: UIView? {
        get {
            (softKeyboardVisible && !hardwareKeyboardConnected) ? accessoryBar : nil
        }
        set { /* ignore external setters */ }
    }

    private var repeatKeyCommands: [RepeatCommand]
    private lazy var controlKeyCommands: [UIKeyCommand] = buildControlCommands()

    private func configureAccessoryBar() {
        accessoryBar.allowsSelfSizing = true
        accessoryBar.translatesAutoresizingMaskIntoConstraints = false

        let stack = UIStackView()
        stack.axis = .horizontal
        stack.alignment = .fill
        stack.distribution = .fillEqually
        stack.spacing = 8
        stack.translatesAutoresizingMaskIntoConstraints = false

        func makeButton(_ title: String, action: Selector, label: String? = nil, hint: String? = nil) -> UIButton {
            let button = UIButton(type: .system)
            if let label {
                button.accessibilityLabel = label
            }
            if let hint {
                button.accessibilityHint = hint
            }
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

        let esc = makeButton("\u{238B}", action: #selector(handleEsc), label: "Escape")
        let ctrl = makeButton("^", action: #selector(handleCtrlToggle), label: "Control", hint: "Toggles the control modifier for the next key press")
        ctrlButton = ctrl
        updateCtrlButtonAppearance()
        let up = makeButton("↑", action: #selector(handleUp), label: "Up Arrow")
        let down = makeButton("↓", action: #selector(handleDown), label: "Down Arrow")
        let left = makeButton("←", action: #selector(handleLeft), label: "Left Arrow")
        let right = makeButton("→", action: #selector(handleRight), label: "Right Arrow")
        let fslash = makeButton("/", action: #selector(handleFSlash), label: "Slash")
        let dot = makeButton(".", action: #selector(handleDot), label: "Period")
        let minus = makeButton("-", action: #selector(handleMinus), label: "Hyphen")
        let pipe = makeButton("|", action: #selector(handlePipe), label: "Pipe")

        if UIDevice.current.userInterfaceIdiom == .phone {
            // Replaced "Tab" with the Unicode symbol \u{21E5}
            let tab = makeButton("\u{21E5}", action: #selector(handleTab), label: "Tab")
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

    private func buildRepeatCommands() -> [RepeatCommand] {
        var commands: [RepeatCommand] = []
        func makeCommand(input: String, output: String, modifiers: UIKeyModifierFlags = []) {
            let command = UIKeyCommand(input: input,
                                       modifierFlags: modifiers,
                                       action: #selector(handleRepeatCommand(_:)))
            if #available(iOS 15.0, *) {
                command.wantsPriorityOverSystemBehavior = true
            }
            commands.append(RepeatCommand(command: command, output: output))
        }

        makeCommand(input: "h", output: "h")
        makeCommand(input: "j", output: "j")
        makeCommand(input: "k", output: "k")
        makeCommand(input: "l", output: "l")
        makeCommand(input: UIKeyCommand.inputUpArrow, output: "\u{1B}[A")
        makeCommand(input: UIKeyCommand.inputDownArrow, output: "\u{1B}[B")
        makeCommand(input: UIKeyCommand.inputLeftArrow, output: "\u{1B}[D")
        makeCommand(input: UIKeyCommand.inputRightArrow, output: "\u{1B}[C")
        makeCommand(input: "\r", output: "\r")
        return commands
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
        controlLatch = false
        let resigned = super.resignFirstResponder()
        if resigned {
            onFocusChange?(false)
        }
        return resigned
    }

    override var keyCommands: [UIKeyCommand]? {
        var commands = repeatKeyCommands.map { $0.command }
        let increase1 = UIKeyCommand(input: "+", modifierFlags: [.command], action: #selector(handleIncreaseFont))
        let increase2 = UIKeyCommand(input: "=", modifierFlags: [.command], action: #selector(handleIncreaseFont))
        let decrease = UIKeyCommand(input: "-", modifierFlags: [.command], action: #selector(handleDecreaseFont))
        if #available(iOS 15.0, *) {
            increase1.wantsPriorityOverSystemBehavior = true
            increase2.wantsPriorityOverSystemBehavior = true
            decrease.wantsPriorityOverSystemBehavior = true
        }
        commands.append(contentsOf: [increase1, increase2, decrease])
        let escape = UIKeyCommand(input: UIKeyCommand.inputEscape,
                                  modifierFlags: [],
                                  action: #selector(handleEscapeKey))
        let tab = UIKeyCommand(input: "\t",
                               modifierFlags: [],
                               action: #selector(handleTabKeyCommand(_:)))
        let shiftTab = UIKeyCommand(input: "\t",
                                    modifierFlags: [.shift],
                                    action: #selector(handleTabKeyCommand(_:)))
        if #available(iOS 15.0, *) {
            escape.wantsPriorityOverSystemBehavior = true
            tab.wantsPriorityOverSystemBehavior = true
            shiftTab.wantsPriorityOverSystemBehavior = true
        }
        commands.append(contentsOf: [escape, tab, shiftTab])
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
        commands.append(contentsOf: controlKeyCommands)
        return commands
    }

    override func insertText(_ text: String) {
        if controlLatch, let scalar = text.unicodeScalars.first {
            controlLatch = false
            if let control = controlCharacter(for: scalar) {
                let handled = maybeTriggerControlSignal(scalar: scalar)
                if !handled {
                    onInput?(control)
                }
                return
            }
        }
        if text.unicodeScalars.count == 1,
           let scalar = text.unicodeScalars.first,
           signalScalarForRawControlCode(scalar) != nil {
            let handled = maybeTriggerControlSignal(scalar: scalar)
            if handled {
                return
            }
        }
        if text.unicodeScalars.count == 1,
           let scalar = text.unicodeScalars.first,
           let optionSequence = optionComposedFallbackSequence(for: scalar) {
            onInput?(optionSequence)
            return
        }
        let normalized = text.replacingOccurrences(of: "\n", with: "\r")
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
        var unhandled: Set<UIPress> = []
        for press in presses {
            if !handle(press: press, eventModifierFlags: event?.modifierFlags) {
                unhandled.insert(press)
            }
        }
        if !unhandled.isEmpty {
            super.pressesBegan(unhandled, with: event)
        }
    }

    override func pressesChanged(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        if let event {
            NotificationCenter.default.post(name: .terminalModifierStateChanged,
                                            object: nil,
                                            userInfo: ["command": event.modifierFlags.contains(.command)])
        }
        var unhandled: Set<UIPress> = []
        for press in presses {
            if !handle(press: press, eventModifierFlags: event?.modifierFlags) {
                unhandled.insert(press)
            }
        }
        if !unhandled.isEmpty {
            super.pressesChanged(unhandled, with: event)
        }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        NotificationCenter.default.post(name: .terminalModifierStateChanged,
                                        object: nil,
                                        userInfo: ["command": false])
        super.pressesEnded(presses, with: event)
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        NotificationCenter.default.post(name: .terminalModifierStateChanged,
                                        object: nil,
                                        userInfo: ["command": false])
        super.pressesCancelled(presses, with: event)
    }

    private func arrowSequence(_ direction: String) -> String {
        let prefix = applicationCursorEnabled ? "\u{1B}O" : "\u{1B}["
        return prefix + direction
    }

    @objc private func handleRepeatCommand(_ command: UIKeyCommand) {
        if let input = command.input {
            switch input {
            case UIKeyCommand.inputUpArrow:
                onInput?(arrowSequence("A"))
                return
            case UIKeyCommand.inputDownArrow:
                onInput?(arrowSequence("B"))
                return
            case UIKeyCommand.inputLeftArrow:
                onInput?(arrowSequence("D"))
                return
            case UIKeyCommand.inputRightArrow:
                onInput?(arrowSequence("C"))
                return
            case "\r":
                onInput?("\r")
                return
            default:
                break
            }
        }
        if let match = repeatKeyCommands.first(where: { $0.command === command }) {
            onInput?(match.output)
            return
        }
        guard let input = command.input else { return }
        switch input {
        case UIKeyCommand.inputUpArrow:
            onInput?(arrowSequence("A"))
        case UIKeyCommand.inputDownArrow:
            onInput?(arrowSequence("B"))
        case UIKeyCommand.inputLeftArrow:
            onInput?(arrowSequence("D"))
        case UIKeyCommand.inputRightArrow:
            onInput?(arrowSequence("C"))
        case "\r":
            onInput?("\r")
        default:
            onInput?(input)
        }
    }

    private func handle(press: UIPress, eventModifierFlags: UIKeyModifierFlags?) -> Bool {
        guard let key = press.key else { return false }
        let modifierFlags = key.modifierFlags.union(eventModifierFlags ?? [])

        if key.keyCode == .keyboardTab {
            if modifierFlags.contains(.shift) {
                onInput?("\u{1B}[Z")
            } else {
                onInput?("\t")
            }
            return true
        }

        if modifierFlags.contains(.alternate),
           !modifierFlags.contains(.control),
           !modifierFlags.contains(.command),
           let optionSequence = optionModifiedSequence(for: key, modifierFlags: modifierFlags) {
            onInput?(optionSequence)
            return true
        }

        if modifierFlags.contains(.control),
           let scalar = key.charactersIgnoringModifiers.unicodeScalars.first {
            if let control = controlCharacter(for: scalar) {
                let handled = maybeTriggerControlSignal(scalar: scalar)
                if !handled {
                    onInput?(control)
                }
                return true
            }
            if signalScalarForRawControlCode(scalar) != nil {
                let handled = maybeTriggerControlSignal(scalar: scalar)
                if !handled {
                    onInput?(String(scalar))
                }
                return true
            }
        }

        switch key.keyCode {
        case .keyboardReturnOrEnter:
            onInput?("\r"); return true
        case .keyboardDeleteForward:
            onInput?("\u{1B}[3~"); return true
        case .keyboardEscape:
            onInput?("\u{1B}"); return true
        default:
            break
        }
        return false
    }

    private func optionModifiedSequence(for key: UIKey, modifierFlags: UIKeyModifierFlags) -> String? {
        switch key.keyCode {
        case .keyboardUpArrow:
            return "\u{1B}[1;3A"
        case .keyboardDownArrow:
            return "\u{1B}[1;3B"
        case .keyboardLeftArrow:
            return "\u{1B}[1;3D"
        case .keyboardRightArrow:
            return "\u{1B}[1;3C"
        default:
            break
        }

        if let letterScalar = optionLetterScalar(for: key.keyCode,
                                                 shift: modifierFlags.contains(.shift)) {
            return "\u{1B}" + String(letterScalar)
        }

        guard key.charactersIgnoringModifiers.unicodeScalars.count == 1,
              let scalar = key.charactersIgnoringModifiers.unicodeScalars.first,
              scalar.isASCII else {
            return nil
        }

        var outputScalar = scalar
        if modifierFlags.contains(.shift),
           scalar.value >= 0x61,
           scalar.value <= 0x7A,
           let upper = UnicodeScalar(scalar.value - 0x20) {
            outputScalar = upper
        }
        return "\u{1B}" + String(outputScalar)
    }

    private func optionLetterScalar(for keyCode: UIKeyboardHIDUsage, shift: Bool) -> UnicodeScalar? {
        let base: UInt32
        switch keyCode {
        case .keyboardA: base = 0x61
        case .keyboardB: base = 0x62
        case .keyboardC: base = 0x63
        case .keyboardD: base = 0x64
        case .keyboardE: base = 0x65
        case .keyboardF: base = 0x66
        case .keyboardG: base = 0x67
        case .keyboardH: base = 0x68
        case .keyboardI: base = 0x69
        case .keyboardJ: base = 0x6A
        case .keyboardK: base = 0x6B
        case .keyboardL: base = 0x6C
        case .keyboardM: base = 0x6D
        case .keyboardN: base = 0x6E
        case .keyboardO: base = 0x6F
        case .keyboardP: base = 0x70
        case .keyboardQ: base = 0x71
        case .keyboardR: base = 0x72
        case .keyboardS: base = 0x73
        case .keyboardT: base = 0x74
        case .keyboardU: base = 0x75
        case .keyboardV: base = 0x76
        case .keyboardW: base = 0x77
        case .keyboardX: base = 0x78
        case .keyboardY: base = 0x79
        case .keyboardZ: base = 0x7A
        default:
            return nil
        }
        return UnicodeScalar(shift ? (base - 0x20) : base)
    }

    private func optionComposedFallbackSequence(for scalar: UnicodeScalar) -> String? {
        let mapped: UnicodeScalar
        switch scalar {
        case "å", "Å": mapped = "a"
        case "∫": mapped = "b"
        case "ç", "Ç": mapped = "c"
        case "∂": mapped = "d"
        case "´", "É": mapped = "e"
        case "ƒ": mapped = "f"
        case "©": mapped = "g"
        case "˙": mapped = "h"
        case "ˆ": mapped = "i"
        case "∆": mapped = "j"
        case "˚": mapped = "k"
        case "¬": mapped = "l"
        case "µ": mapped = "m"
        case "˜", "ı": mapped = "n"
        case "ø", "Ø": mapped = "o"
        case "π", "∏": mapped = "p"
        case "œ", "Œ": mapped = "q"
        case "®": mapped = "r"
        case "ß": mapped = "s"
        case "†": mapped = "t"
        case "¨": mapped = "u"
        case "√": mapped = "v"
        case "∑": mapped = "w"
        case "≈": mapped = "x"
        case "¥": mapped = "y"
        case "Ω": mapped = "z"
        case "¡": mapped = "1"
        case "™": mapped = "2"
        case "£": mapped = "3"
        case "¢": mapped = "4"
        case "∞": mapped = "5"
        case "§": mapped = "6"
        case "¶": mapped = "7"
        case "•": mapped = "8"
        case "ª": mapped = "9"
        case "º": mapped = "0"
        case "≤": mapped = ","
        case "≥": mapped = "."
        case "÷": mapped = "/"
        case "–": mapped = "-"
        case "≠": mapped = "="
        default:
            return nil
        }
        return "\u{1B}" + String(mapped)
    }

    @objc private func handleEscapeKey() {
        onInput?("\u{1B}")
    }

    @objc private func handleTabKeyCommand(_ command: UIKeyCommand) {
        if command.modifierFlags.contains(.shift) {
            onInput?("\u{1B}[Z")
        } else {
            onInput?("\t")
        }
    }

    private func buildControlCommands() -> [UIKeyCommand] {
        let inputs = "abcdefghijklmnopqrstuvwxyz".map { String($0) }
        return inputs.map { input in
            let command = UIKeyCommand(input: input,
                                       modifierFlags: [.control],
                                       action: #selector(handleControlCommand(_:)))
            if #available(iOS 15.0, *) {
                command.wantsPriorityOverSystemBehavior = true
            }
            return command
        }
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
                guard pscalIOSSDLModeActive() == 0 else { return }
                guard !Self.hasVisibleSDLWindow() else { return }
                guard !TerminalTabManager.shared.isSdlTabSelected else { return }
                if !self.isFirstResponder {
                    _ = self.becomeFirstResponder()
                } else if !self.softKeyboardVisible {
                    self.reloadInputViews()
                }
            }
        }

        keyboardObservers = [willShow, willHide, focusRequested]
    }

    // MARK: - Accessory button actions
    @objc private func handleEsc() {
        onInput?("\u{1B}")
    }
    
    @objc private func handleTab() {
        onInput?("\t")
    }

    @objc private func handleCtrlToggle(_ sender: UIButton) {
        controlLatch.toggle()
    }

    @objc private func handleUp() {
        onInput?(arrowSequence("A"))
    }

    @objc private func handleDown() {
        onInput?(arrowSequence("B"))
    }

    @objc private func handleLeft() {
        onInput?(arrowSequence("D"))
    }

    @objc private func handleRight() {
        onInput?(arrowSequence("C"))
    }

    @objc private func handleFSlash() {
        onInput?("/")
    }

    @objc private func handleDot() {
        onInput?(".")
    }

    @objc private func handleMinus() {
        onInput?("-")
    }
   
    @objc private func handlePipe() {
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

    @objc private func handleControlCommand(_ command: UIKeyCommand) {
        guard let input = command.input,
              let scalar = input.lowercased().unicodeScalars.first else { return }
        if let control = controlCharacter(for: scalar) {
            let handled = maybeTriggerControlSignal(scalar: scalar)
            if !handled {
                onInput?(control)
            }
        }
    }

    private func controlCharacter(for scalar: UnicodeScalar) -> String? {
        let value = scalar.value
        guard value >= 0x40, value <= 0x7F,
              let ctrlScalar = UnicodeScalar(value & 0x1F) else {
            return nil
        }
        return String(ctrlScalar)
    }

    private func signalScalarForRawControlCode(_ scalar: UnicodeScalar) -> UnicodeScalar? {
        let value = scalar.value
        guard value >= 0x01 && value <= 0x1A else {
            return nil
        }
        return UnicodeScalar(value + 0x60)
    }

    private func maybeTriggerControlSignal(scalar: UnicodeScalar) -> Bool {
        let normalized = signalScalarForRawControlCode(scalar) ?? scalar
        let lowered = UnicodeScalar(UInt32(tolower(Int32(normalized.value)))) ?? normalized
        switch lowered {
        case "c":
            return triggerInterrupt()
        case "z":
            return triggerSuspend()
        default:
            return false
        }
    }

    private func triggerInterrupt() -> Bool {
        guard let onInterrupt else {
            return false
        }
        onInterrupt()
        return true
    }

    private func triggerSuspend() -> Bool {
        guard let onSuspend else {
            return false
        }
        onSuspend()
        return true
    }

    private func updateCtrlButtonAppearance() {
        guard let button = ctrlButton else { return }
        var config = button.configuration ?? UIButton.Configuration.filled()
        config.baseBackgroundColor = controlLatch
            ? UIColor.systemBlue.withAlphaComponent(0.8)
            : UIColor.secondarySystemBackground.withAlphaComponent(0.8)
        button.configuration = config

        if controlLatch {
            button.accessibilityTraits.insert(.selected)
        } else {
            button.accessibilityTraits.remove(.selected)
        }
    }
}
