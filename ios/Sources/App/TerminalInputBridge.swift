import SwiftUI
import UIKit

@_silgen_name("pscalRuntimeRequestSigint")
func pscalRuntimeRequestSigint()

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

    final class Coordinator {
        weak var view: TerminalKeyInputView?
        var focusAnchor: Int = 0
    }
}

final class TerminalKeyInputView: UITextView {
    var onInput: ((String) -> Void)?
    var onPaste: ((String) -> Void)?
    private struct RepeatCommand {
        let command: UIKeyCommand
        let output: String
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
        commands.append(UIKeyCommand(input: "v",
                                     modifierFlags: [.command],
                                     action: #selector(handlePasteCommand)))
        return commands
    }

    override func insertText(_ text: String) {
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
        case .keyboardUpArrow:
            onInput?("\u{1B}[A"); return true
        case .keyboardDownArrow:
            onInput?("\u{1B}[B"); return true
        case .keyboardLeftArrow:
            onInput?("\u{1B}[D"); return true
        case .keyboardRightArrow:
            onInput?("\u{1B}[C"); return true
        case .keyboardDeleteForward:
            onInput?("\u{1B}[3~"); return true
        case .keyboardEscape:
            onInput?("\u{1B}"); return true
        default:
            break
        }
        return false
    }

    @objc private func handlePasteCommand() {
        guard let text = UIPasteboard.general.string, !text.isEmpty else { return }
        onPaste?(text)
    }
}
