import SwiftUI
import UIKit

/// UIKit shell that hosts the SwiftUI TerminalView and relies on keyboardLayoutGuide
/// to keep the terminal aligned with the on-screen keyboard.
final class TerminalRootViewController: UIViewController {
    private let hostingController: UIHostingController<TerminalView>

    init(showsOverlay: Bool) {
        self.hostingController = UIHostingController(rootView: TerminalView(showsOverlay: showsOverlay))
        super.init(nibName: nil, bundle: nil)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func viewDidLoad() {
        super.viewDidLoad()

        // Ensure background matches terminal default to avoid white gaps
        view.backgroundColor = .black

        addChild(hostingController)
        hostingController.view.translatesAutoresizingMaskIntoConstraints = false
        // Remove background color of hosting view so it's transparent?
        // No, TerminalView has its own background.
        // But if we want the black window background to show through in safe areas (if clipped),
        // we should let hosting view fit the constraints.

        view.addSubview(hostingController.view)

        // Pin the hosted SwiftUI view to the safe area and keyboard guide.
        // We pin leading/trailing/top to safe area to avoid drawing under notch/home indicator.
        // We pin bottom to keyboardLayoutGuide.topAnchor to automatically resize when keyboard appears.
        NSLayoutConstraint.activate([
            hostingController.view.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor),
            hostingController.view.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor),
            hostingController.view.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            hostingController.view.bottomAnchor.constraint(equalTo: view.keyboardLayoutGuide.topAnchor)
        ])

        hostingController.didMove(toParent: self)
    }

    func update(showsOverlay: Bool) {
        hostingController.rootView = TerminalView(showsOverlay: showsOverlay)
    }
}

struct TerminalRootView: UIViewControllerRepresentable {
    let showsOverlay: Bool

    func makeUIViewController(context: Context) -> TerminalRootViewController {
        TerminalRootViewController(showsOverlay: showsOverlay)
    }

    func updateUIViewController(_ uiViewController: TerminalRootViewController, context: Context) {
        uiViewController.update(showsOverlay: showsOverlay)
    }
}
