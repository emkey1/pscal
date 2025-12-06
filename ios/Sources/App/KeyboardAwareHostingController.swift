import SwiftUI
import UIKit

// Global notification for keyboard overlap changes.
let keyboardOverlapNotification = Notification.Name("KeyboardOverlapDidChange")

final class KeyboardHostingController<Content: View>: UIHostingController<Content> {
    private var bottomConstraint: NSLayoutConstraint?

    override init(rootView: Content) {
        super.init(rootView: rootView)
    }

    @MainActor required dynamic init?(coder aDecoder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        setupKeyboardGuide()
        observeKeyboard()
    }

    private func setupKeyboardGuide() {
        let guide = UILayoutGuide()
        guide.identifier = "KeyboardGuide"
        view.addLayoutGuide(guide)

        NSLayoutConstraint.activate([
            guide.leftAnchor.constraint(equalTo: view.leftAnchor),
            guide.rightAnchor.constraint(equalTo: view.rightAnchor),
            guide.heightAnchor.constraint(equalToConstant: 0),
            guide.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor)
        ])

        if let contentView = view.subviews.first {
            contentView.translatesAutoresizingMaskIntoConstraints = false
            NSLayoutConstraint.activate([
                contentView.topAnchor.constraint(equalTo: view.topAnchor),
                contentView.leftAnchor.constraint(equalTo: view.leftAnchor),
                contentView.rightAnchor.constraint(equalTo: view.rightAnchor)
            ])
            bottomConstraint = contentView.bottomAnchor.constraint(equalTo: guide.topAnchor)
            bottomConstraint?.priority = .defaultHigh
            bottomConstraint?.isActive = true
        }
    }

    private func observeKeyboard() {
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(handleKeyboard),
            name: UIResponder.keyboardWillChangeFrameNotification,
            object: nil
        )
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(handleKeyboard),
            name: UIResponder.keyboardWillHideNotification,
            object: nil
        )
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    @objc private func handleKeyboard(_ notification: Notification) {
        guard
            let userInfo = notification.userInfo,
            let frame = (userInfo[UIResponder.keyboardFrameEndUserInfoKey] as? NSValue)?.cgRectValue
        else {
            return
        }

        let converted = view.convert(frame, from: nil)
        let overlap = max(view.bounds.maxY - converted.origin.y, 0)
        let duration = (userInfo[UIResponder.keyboardAnimationDurationUserInfoKey] as? NSNumber)?.doubleValue ?? 0.25
        let curveRaw = (userInfo[UIResponder.keyboardAnimationCurveUserInfoKey] as? NSNumber)?.uintValue ?? UIView.AnimationOptions.curveEaseInOut.rawValue
        let curve = UIView.AnimationOptions(rawValue: curveRaw << 16)

        bottomConstraint?.constant = -overlap
        NotificationCenter.default.post(name: keyboardOverlapNotification,
                                        object: nil,
                                        userInfo: ["overlap": overlap])
        UIView.animate(withDuration: duration, delay: 0, options: curve, animations: {
            self.view.layoutIfNeeded()
        })
    }
}

struct KeyboardAwareContainer<Content: View>: UIViewControllerRepresentable {
    let content: Content

    func makeUIViewController(context: Context) -> KeyboardHostingController<Content> {
        KeyboardHostingController(rootView: content)
    }

    func updateUIViewController(_ controller: KeyboardHostingController<Content>, context: Context) {
        controller.rootView = content
    }
}
