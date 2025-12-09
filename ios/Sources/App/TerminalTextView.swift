import SwiftUI
import UIKit

struct TerminalTextView: UIViewRepresentable {
    var text: NSAttributedString
    var cursor: TerminalCursorInfo?
    @Binding var fontSize: CGFloat
    @Binding var terminalColor: UIColor

    var onInput: (String) -> Void
    var onSettingsChanged: (CGFloat, UIColor) -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(self)
    }

    func makeUIView(context: Context) -> NativeTerminalView {
        let textView = NativeTerminalView()
        textView.delegate = context.coordinator

        textView.onSettingsChanged = { newSize, newColor in
            self.onSettingsChanged(newSize, newColor)
        }

        textView.onInput = { input in
            self.onInput(input)
        }

        textView.updateAppearance(color: terminalColor, fontSize: fontSize)

        return textView
    }

    func updateUIView(_ uiView: NativeTerminalView, context: Context) {
        uiView.updateAppearance(color: terminalColor, fontSize: fontSize)
        uiView.updateContent(text: text, cursor: cursor)
    }

    class Coordinator: NSObject, UITextViewDelegate {
        var parent: TerminalTextView

        init(_ parent: TerminalTextView) {
            self.parent = parent
        }

        func textView(_ textView: UITextView, shouldChangeTextIn range: NSRange, replacementText text: String) -> Bool {
            // We intercept all input and send it to the backend
            // The backend (PTY) will echo it back if appropriate

            // Handle special case for newline if needed, but usually we just send everything
            if text == "\n" {
                parent.onInput("\r")
            } else if text == "" {
                // Backspace
                parent.onInput("\u{7F}")
            } else {
                parent.onInput(text)
            }

            return false // Prevent local change
        }

        func textViewDidChange(_ textView: UITextView) {
            // Should not happen if we return false above
        }
    }
}
