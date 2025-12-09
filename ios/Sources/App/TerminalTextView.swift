import SwiftUI
import UIKit

struct TerminalTextView: UIViewRepresentable {
    var text: NSAttributedString
    var cursor: TerminalCursorInfo?
    @Binding var fontSize: CGFloat
    @Binding var terminalColor: UIColor
    @Binding var backgroundColor: UIColor

    var onInput: (String) -> Void
    var onSettingsChanged: (CGFloat, UIColor) -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(self)
    }

    func makeUIView(context: Context) -> NativeTerminalView {
        let textView = NativeTerminalView()
        textView.delegate = context.coordinator

        // Pass closures that call the coordinator's parent reference,
        // which is updated in updateUIView. However, context.coordinator is stable.
        // But context.coordinator.parent is updated.
        // So we can use context.coordinator.parent inside these closures?
        // Actually, closures in makeUIView capture 'self' of TerminalTextView struct (which is transient)
        // or context.coordinator.

        // The safest way is to update the closures in updateUIView OR use the coordinator to dispatch.
        // Let's use the coordinator to dispatch these actions back to SwiftUI.

        textView.onSettingsChanged = { [weak coordinator = context.coordinator] newSize, newColor in
            coordinator?.parent.onSettingsChanged(newSize, newColor)
        }

        textView.onInput = { [weak coordinator = context.coordinator] input in
            coordinator?.parent.onInput(input)
        }

        textView.updateAppearance(color: terminalColor, backgroundColor: backgroundColor, fontSize: fontSize)

        return textView
    }

    func updateUIView(_ uiView: NativeTerminalView, context: Context) {
        context.coordinator.parent = self
        uiView.updateAppearance(color: terminalColor, backgroundColor: backgroundColor, fontSize: fontSize)
        uiView.updateContent(text: text, cursor: cursor)
    }

    class Coordinator: NSObject, UITextViewDelegate {
        var parent: TerminalTextView

        init(_ parent: TerminalTextView) {
            self.parent = parent
        }

        func textView(_ textView: UITextView, shouldChangeTextIn range: NSRange, replacementText text: String) -> Bool {
            if text == "\n" {
                parent.onInput("\n")
            } else if text == "" {
                if range.length > 0 {
                    parent.onInput("\u{7F}")
                }
            } else {
                parent.onInput(text)
            }
            return false
        }
    }
}
