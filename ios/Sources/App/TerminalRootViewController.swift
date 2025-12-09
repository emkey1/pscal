import SwiftUI
import UIKit

/// SwiftUI wrapper for TerminalRootViewController
struct TerminalRootView: UIViewControllerRepresentable {
    func makeUIViewController(context: Context) -> TerminalRootViewController {
        return TerminalRootViewController()
    }

    func updateUIViewController(_ uiViewController: TerminalRootViewController, context: Context) {
        // No updates needed for now
    }
}

/// UIKit shell that hosts the SwiftUI TerminalView and relies on keyboardLayoutGuide
/// to keep the terminal aligned with the on-screen keyboard.
final class TerminalRootViewController: UIViewController {
    private let hostingController = UIHostingController(rootView: TerminalView(showsOverlay: true))

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black // Ensure background matches terminal

        addChild(hostingController)
        hostingController.view.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(hostingController.view)

        // Pin the hosted SwiftUI view to the safe area and keyboard guide.
        NSLayoutConstraint.activate([
            hostingController.view.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor),
            hostingController.view.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor),
            hostingController.view.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            hostingController.view.bottomAnchor.constraint(equalTo: view.keyboardLayoutGuide.topAnchor)
        ])

        hostingController.didMove(toParent: self)
    }
}
