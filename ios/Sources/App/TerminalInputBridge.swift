import SwiftUI
import UIKit

@_silgen_name("pscalRuntimeRequestSigint")
func pscalRuntimeRequestSigint()

extension Notification.Name {
    static let terminalModifierStateChanged = Notification.Name("terminalModifierStateChanged")
}

struct TerminalInputBridge: UIViewRepresentable {
    @Binding var focusAnchor: Int
    var onInput: (String) -> Void
    var onPaste: ((String) -> Void)?

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
    private var hardwareKeyboardConnected: Bool = false
    private var keyboardObservers: [NSObjectProtocol] = []
    
    private struct RepeatCommand {
        let command: UIKeyCommand
        let output: String
    }

    override init(frame: CGRect, textContainer: NSTextContainer?) {
        super.init(frame: frame, textContainer: textContainer)
        installKeyboardObservers()
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        super.init(coder: coder)
        installKeyboardObservers()
    }

    deinit {
        keyboardObservers.forEach { NotificationCenter.default.removeObserver($0) }
    }

    private var controlLatch: Bool = false

    // MARK: - FIXED ACCESSORY BAR
    private lazy var accessoryBar: UIInputView = {
        // 1. Init with .zero. The system manages the frame.
        let bar = UIInputView(frame: .zero, inputViewStyle: .keyboard)
        
        // 2. Enable Auto Layout internal sizing
        bar.allowsSelfSizing = true
        bar.translatesAutoresizingMaskIntoConstraints = false
        
        // 3. REMOVED: bar.autoresizingMask (This causes conflicts during rotation)

        let stack = UIStackView()
        stack.axis = .horizontal
        stack.alignment = .center
        stack.distribution = .fillProportionally
        stack.spacing = 8
        stack.translatesAutoresizingMaskIntoConstraints = false

        func makeButton(_ title: String, action: Selector) -> UIButton {
            let button = UIButton(type: .system)
            var config = UIButton.Configuration.filled()
            config.cornerStyle = .medium
            config.baseBackgroundColor = UIColor.secondarySystemBackground.withAlphaComponent(0.8)
            config.baseForegroundColor = .label
            config.title = title
            // Reduced insets slightly to fit better on smaller screens (iPad mini)
            config.contentInsets = NSDirectionalEdgeInsets(top: 6, leading: 4, bottom: 6, trailing: 4)
            button.configuration = config
            // Dynamic type support for accessibility
            button.titleLabel?.font = UIFontMetrics.default.scaledFont(for: .systemFont(ofSize: 15, weight: .semibold))
            button.addTarget(self, action: action, for: .touchUpInside)
            return button
        }

        let esc = makeButton("Esc", action: #selector(handleEsc))
        let ctrl = makeButton("Ctrl", action: #selector(handleCtrlToggle))
        let up = makeButton("↑", action: #selector(handleUp))
        let down = makeButton("↓", action: #selector(handleDown))
        let left = makeButton("←", action: #selector(handleLeft))
        let right = makeButton("→", action: #selector(handleRight))
        let fslash = makeButton("/", action: #selector(handleFSlash))
        let dot = makeButton(".", action: #selector(handleDot))
        let minus = makeButton("-", action: #selector(handleMinus))

        // Updated Layout Logic: Include Tab only for phones
        if UIDevice.current.userInterfaceIdiom == .phone {
            let tab = makeButton("Tab", action: #selector(handleTab))
            [esc, ctrl, tab, dot, fslash, minus, up, down, left, right].forEach(stack.addArrangedSubview)
        } else {
            [esc, ctrl, dot, fslash, minus, up, down, left, right].forEach(stack.addArrangedSubview)
        }

        bar.addSubview(stack)
        
        // 4. Use safeAreaLayoutGuide. This keeps buttons accessible on all device orientations.
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: bar.safeAreaLayoutGuide.leadingAnchor, constant: 8),
            stack.trailingAnchor.constraint(equalTo: bar.safeAreaLayoutGuide.trailingAnchor, constant: -8),
            stack.topAnchor.constraint(equalTo: bar.topAnchor, constant: 6),
            stack.bottomAnchor.constraint(equalTo: bar.bottomAnchor, constant: -6),
            // Ensure the bar has a minimum height, but allow it to grow if font size is huge
            bar.heightAnchor.constraint(greaterThanOrEqualToConstant: 50)
        ])
        return bar
    }()

    override var inputAccessoryView: UIView? {
        get {
            // NOTE: If you want the Esc/Ctrl keys to be available even when a
            // hardware keyboard is attached (common for Vim usage), remove this check.
            hardwareKeyboardConnected ? nil : accessoryBar
        }
        set { /* ignore external setters */ }
    }

    private lazy var repeatKeyCommands: [RepeatCommand] = {
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
    }()

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
        commands.append(UIKeyCommand(input: "v",
                                     modifierFlags: [.command],
                                     action: #selector(handlePasteCommand)))
        return commands
    }

    override func insertText(_ text: String) {
        if controlLatch, let scalar = text.unicodeScalars.first {
            controlLatch = false
            let value = scalar.value
            if scalar == "c" {
                pscalRuntimeRequestSigint()
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
            reloadInputViews()
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
                pscalRuntimeRequestSigint()
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

    private func installKeyboardObservers() {
        let willShow = NotificationCenter.default.addObserver(
            forName: UIResponder.keyboardWillShowNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            guard let self else { return }
            if self.hardwareKeyboardConnected {
                self.hardwareKeyboardConnected = false
                self.reloadInputViews()
            }
        }

        keyboardObservers = [willShow]
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
        sender.backgroundColor = controlLatch
        ? UIColor.systemBlue.withAlphaComponent(0.8)
        : UIColor.secondarySystemBackground.withAlphaComponent(0.8)
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
}
