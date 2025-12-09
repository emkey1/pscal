import SwiftUI
import UIKit

/// UIKit shell that hosts the SwiftUI TerminalView and relies on keyboardLayoutGuide
/// to keep the terminal aligned with the on-screen keyboard.
final class TerminalRootViewController: UIViewController {
    private let hostingController = UIHostingController(rootView: TerminalView(showsOverlay: true))

    override var shouldAutorotate: Bool { return false }
    override var supportedInterfaceOrientations: UIInterfaceOrientationMask {
        PscalAppDelegate.lockedOrientationMask ?? .portrait
    }

    override var preferredInterfaceOrientationForPresentation: UIInterfaceOrientation {
        if let mask = PscalAppDelegate.lockedOrientationMask {
            if mask.contains(.landscape) {
                return .landscapeLeft
            }
        }
        return .portrait
    }
    
    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        if let scene = view.window?.windowScene,
           let mask = PscalAppDelegate.lockedOrientationMask {
            if #available(iOS 16.0, *) {
                scene.requestGeometryUpdate(.iOS(interfaceOrientations: mask)) { _ in }
            } else {
                // For iOS 15, set the mask on the scene’s windows to discourage rotation.
                scene.windows.forEach { $0.overrideUserInterfaceStyle = $0.overrideUserInterfaceStyle }
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
            hostingController.view.bottomAnchor.constraint(equalTo: view.keyboardLayoutGuide.topAnchor)
        ])

        hostingController.didMove(toParent: self)
    }
}
