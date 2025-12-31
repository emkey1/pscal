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
    var onInterrupt: (() -> Void)? = nil
    var onSuspend: (() -> Void)? = nil

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
        view.onInterrupt = onInterrupt
        view.onSuspend = onSuspend
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
        uiView.onInput = onInput
        uiView.onPaste = onPaste
        uiView.onInterrupt = onInterrupt
        uiView.onSuspend = onSuspend

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
    var onInterrupt: (() -> Void)?
    var onSuspend: (() -> Void)?
    private var hardwareKeyboardConnected: Bool = false
    private var softKeyboardVisible: Bool = false
    private var keyboardObservers: [NSObjectProtocol] = []
    
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

    private var controlLatch: Bool = false
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

        let esc = makeButton("Esc", action: #selector(handleEsc))
        let ctrl = makeButton("Ctrl", action: #selector(handleCtrlToggle))
        ctrlButton = ctrl
        let up = makeButton("↑", action: #selector(handleUp))
        let down = makeButton("↓", action: #selector(handleDown))
        let left = makeButton("←", action: #selector(handleLeft))
        let right = makeButton("→", action: #selector(handleRight))
        let fslash = makeButton("/", action: #selector(handleFSlash))
        let dot = makeButton(".", action: #selector(handleDot))
        let minus = makeButton("-", action: #selector(handleMinus))
        let pipe = makeButton("|", action: #selector(handlePipe))

        if UIDevice.current.userInterfaceIdiom == .phone {
            let tab = makeButton("Tab", action: #selector(handleTab))
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
        makeCommand(input: "\r", output: "\n")
        return commands
    }

    override var canBecomeFirstResponder: Bool { true }

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
        if #available(iOS 15.0, *) {
            escape.wantsPriorityOverSystemBehavior = true
        }
        commands.append(escape)
        commands.append(UIKeyCommand(input: "v",
                                     modifierFlags: [.command],
                                     action: #selector(handlePasteCommand)))
        let interrupt = UIKeyCommand(input: "c",
                                     modifierFlags: [.command],
                                     action: #selector(handleCommandInterrupt))
        if #available(iOS 15.0, *) {
            interrupt.wantsPriorityOverSystemBehavior = true
        }
        commands.append(interrupt)
        commands.append(contentsOf: controlKeyCommands)
        return commands
    }

    override func insertText(_ text: String) {
        if controlLatch, let scalar = text.unicodeScalars.first {
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
        onInput?(text)
    }

    override func deleteBackward() {
        onInput?("\u{7F}")
    }

    override func paste(_ sender: Any?) {
        guard let text = UIPasteboard.general.string, !text.isEmpty else { return }
        onPaste?(text)
    }

    @objc private func handleCommandInterrupt() {
        triggerInterrupt()
    }

    override func canPerformAction(_ action: Selector, withSender sender: Any?) -> Bool {
        if action == #selector(paste(_:)) {
            return UIPasteboard.general.hasStrings
        }
        if action == #selector(copy(_:)) {
            return false
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
            if !handle(press: press) {
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
            if !handle(press: press) {
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

    @objc private func handleRepeatCommand(_ command: UIKeyCommand) {
        if let match = repeatKeyCommands.first(where: { $0.command === command }) {
            onInput?(match.output)
            return
        }
        guard let input = command.input else { return }
        switch input {
        case UIKeyCommand.inputUpArrow:
            onInput?("\u{1B}[A")
        case UIKeyCommand.inputDownArrow:
            onInput?("\u{1B}[B")
        case UIKeyCommand.inputLeftArrow:
            onInput?("\u{1B}[D")
        case UIKeyCommand.inputRightArrow:
            onInput?("\u{1B}[C")
        case "\r":
            onInput?("\n")
        default:
            onInput?(input)
        }
    }

    private func handle(press: UIPress) -> Bool {
        guard let key = press.key else { return false }

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
        case .keyboardDeleteForward:
            onInput?("\u{1B}[3~"); return true
        case .keyboardEscape:
            onInput?("\u{1B}"); return true
        default:
            break
        }
        return false
    }

    @objc private func handleEscapeKey() {
        onInput?("\u{1B}")
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
        ) { [weak self] _ in
            guard let self else { return }
            Task { @MainActor in
                self.softKeyboardVisible = true
                self.hardwareKeyboardConnected = false
                self.reloadInputViews()
                if self.isFirstResponder {
                    self.onInput?(" ")
                    self.onInput?("\u{08}")
                }
            }
        }

        let willHide = center.addObserver(
            forName: UIResponder.keyboardWillHideNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            guard let self else { return }
            Task { @MainActor in
                self.softKeyboardVisible = false
                self.reloadInputViews()
                if self.isFirstResponder {
                    self.onInput?(" ")
                    self.onInput?("\u{08}")
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
    @objc private func handleEsc() {
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

    @objc private func handleUp() {
        onInput?("\u{1B}[A")
    }

    @objc private func handleDown() {
        onInput?("\u{1B}[B")
    }

    @objc private func handleLeft() {
        onInput?("\u{1B}[D")
    }

    @objc private func handleRight() {
        onInput?("\u{1B}[C")
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

    @objc private func handleIncreaseFont() {
        let current = TerminalFontSettings.shared.pointSize
        TerminalFontSettings.shared.updatePointSize(current + 1.0)
    }

    @objc private func handleDecreaseFont() {
        let current = TerminalFontSettings.shared.pointSize
        TerminalFontSettings.shared.updatePointSize(current - 1.0)
    }

    @objc private func handleControlCommand(_ command: UIKeyCommand) {
        guard let input = command.input,
              let scalar = input.lowercased().unicodeScalars.first else { return }
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
        }
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
