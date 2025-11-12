import SwiftUI
import UIKit

struct TerminalInputBridge: UIViewRepresentable {
    @Binding var focusAnchor: Int
    var onInput: (String) -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    func makeUIView(context: Context) -> TerminalInputView {
        let view = TerminalInputView()
        view.onInput = onInput
        DispatchQueue.main.async {
            view.becomeFirstResponder()
        }
        return view
    }

    func updateUIView(_ uiView: TerminalInputView, context: Context) {
        uiView.onInput = onInput
        if context.coordinator.focusAnchor != focusAnchor {
            context.coordinator.focusAnchor = focusAnchor
            DispatchQueue.main.async {
                uiView.becomeFirstResponder()
            }
        }
    }

    final class Coordinator {
        var focusAnchor: Int = 0
    }
}

final class TerminalInputView: UIView, UIKeyInput, UITextInputTraits {
    var onInput: ((String) -> Void)?

    // UITextInputTraits
    var autocorrectionType: UITextAutocorrectionType = .no
    var spellCheckingType: UITextSpellCheckingType = .no
    var keyboardType: UIKeyboardType = .default
    var keyboardAppearance: UIKeyboardAppearance = .default
    var returnKeyType: UIReturnKeyType = .default
    var enablesReturnKeyAutomatically: Bool = false
    var isSecureTextEntry: Bool = false
    var textContentType: UITextContentType! = .none
    var smartQuotesType: UITextSmartQuotesType = .no
    var smartInsertDeleteType: UITextSmartInsertDeleteType = .no
    var smartDashesType: UITextSmartDashesType = .no

    override var canBecomeFirstResponder: Bool {
        true
    }

    var hasText: Bool { false }

    func insertText(_ text: String) {
        onInput?(text)
    }

    func deleteBackward() {
        onInput?("\u{7F}")
    }

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var handledAll = true
        for press in presses {
            if !handle(press: press) {
                handledAll = false
            }
        }
        if !handledAll {
            super.pressesBegan(presses, with: event)
        }
    }

    private func handle(press: UIPress) -> Bool {
        guard let key = press.key else { return false }

        switch key.keyCode {
        case .keyboardUpArrow:
            emit("\u{1B}[A")
            return true
        case .keyboardDownArrow:
            emit("\u{1B}[B")
            return true
        case .keyboardLeftArrow:
            emit("\u{1B}[D")
            return true
        case .keyboardRightArrow:
            emit("\u{1B}[C")
            return true
        case .keyboardDeleteForward:
            emit("\u{1B}[3~")
            return true
        case .keyboardDeleteOrBackspace:
            emit("\u{7F}")
            return true
        default:
            break
        }

        if key.modifierFlags.contains(.control) {
            let chars = key.charactersIgnoringModifiers.lowercased()
            guard let scalar = chars.unicodeScalars.first else { return false }
            let value = scalar.value
            if value >= 0x40, value <= 0x7F,
               let ctrlScalar = UnicodeScalar(value & 0x1F) {
                emit(String(ctrlScalar))
                return true
            }
        }

        return false
    }

    private func emit(_ text: String) {
        onInput?(text)
    }
}
