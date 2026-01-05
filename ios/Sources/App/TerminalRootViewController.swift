import SwiftUI
import UIKit

/// UIKit shell that hosts the SwiftUI TerminalView and relies on keyboardLayoutGuide
/// to keep the terminal aligned with the on-screen keyboard.
final class TerminalRootViewController: UIViewController {
    private let hostingController = UIHostingController(rootView: TerminalTabsRootView())
    private var keyboardObservers: [NSObjectProtocol] = []
    private var lastKeyboardOverlap: CGFloat = 0

    override var shouldAutorotate: Bool { return true }
    override var supportedInterfaceOrientations: UIInterfaceOrientationMask {
        PscalAppDelegate.activeOrientationMask
    }

    override var preferredInterfaceOrientationForPresentation: UIInterfaceOrientation {
        let mask = PscalAppDelegate.activeOrientationMask
        if mask.contains(.portrait) {
            return .portrait
        }
        if mask.contains(.landscapeRight) {
            return .landscapeRight
        }
        if mask.contains(.landscapeLeft) {
            return .landscapeLeft
        }
        return .portrait
    }
    
    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        if let scene = view.window?.windowScene {
            let mask = PscalAppDelegate.activeOrientationMask
            if #available(iOS 16.0, *) {
                scene.requestGeometryUpdate(.iOS(interfaceOrientations: mask)) { _ in }
            }
        }
    }

    override func viewDidLoad() {
        super.viewDidLoad()

        addChild(hostingController)
        hostingController.view.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(hostingController.view)

        // Pin the hosted SwiftUI view to the safe area and keyboard guide.
        NSLayoutConstraint.activate([
            hostingController.view.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor),
            hostingController.view.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor),
            hostingController.view.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            hostingController.view.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor)
        ])

        hostingController.didMove(toParent: self)
        installKeyboardObservers()
    }

    deinit {
        keyboardObservers.forEach { NotificationCenter.default.removeObserver($0) }
    }

    private func installKeyboardObservers() {
        let center = NotificationCenter.default
        let willChange = center.addObserver(
            forName: UIResponder.keyboardWillChangeFrameNotification,
            object: nil,
            queue: .main
        ) { [weak self] notification in
            self?.handleKeyboard(notification: notification)
        }
        let willHide = center.addObserver(
            forName: UIResponder.keyboardWillHideNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.updateKeyboardInset(overlap: 0)
        }
        keyboardObservers = [willChange, willHide]
    }

    private func handleKeyboard(notification: Notification) {
        guard
            let userInfo = notification.userInfo,
            let frameValue = userInfo[UIResponder.keyboardFrameEndUserInfoKey] as? NSValue
        else { return }
        let kbFrame = frameValue.cgRectValue
        let converted = view.convert(kbFrame, from: nil)
        let overlap = max(0, view.bounds.maxY - converted.minY)
        updateKeyboardInset(overlap: overlap)
    }

    private func updateKeyboardInset(overlap: CGFloat) {
        let baseBottomInset = view.safeAreaInsets.bottom
        let extra = max(0, overlap - baseBottomInset)
        additionalSafeAreaInsets.bottom = extra
        let prev = lastKeyboardOverlap
        lastKeyboardOverlap = extra
        // When the keyboard shows or hides, nudge the shell to ensure the prompt is visible.
        let transitioned = (prev == 0 && extra > 0) || (prev > 0 && extra == 0)
        if transitioned {
            TerminalTabManager.shared.sendInputToSelected(" ")
            TerminalTabManager.shared.sendInputToSelected("\u{7F}") // delete
        }
    }
}
