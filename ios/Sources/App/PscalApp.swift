import SwiftUI
import UIKit

/// Hosts the SwiftUI terminal inside a UIKit layout that stays keyboard-safe and rotation-friendly.
final class TerminalRootViewController: UIViewController {

    // MARK: - Properties

    /// SwiftUI terminal view. We let SwiftUI handle the Reset / Settings overlay.
    private let terminalHostingController = UIHostingController(
        rootView: TerminalView(showsOverlay: true)
    )

    /// Track constraints so we can cleanly wipe and rebuild on rotation/trait changes.
    private var activeConstraints: [NSLayoutConstraint] = []

    // MARK: - Lifecycle

    override func viewDidLoad() {
        super.viewDidLoad()

        // This is only the window background; actual terminal colors come from settings.
        view.backgroundColor = .systemBackground

        setupViewHierarchy()
        rebuildLayout()
    }

    // MARK: - Rotation Handling

    override func viewWillTransition(
        to size: CGSize,
        with coordinator: UIViewControllerTransitionCoordinator
    ) {
        super.viewWillTransition(to: size, with: coordinator)

        // Rebuild constraints after rotation so the terminal stays pinned above the keyboard.
        coordinator.animate(alongsideTransition: { _ in
            self.rebuildLayout()
            self.view.layoutIfNeeded()
        }, completion: nil)
    }

    // MARK: - Setup

    private func setupViewHierarchy() {
        // Terminal host
        addChild(terminalHostingController)
        terminalHostingController.view.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(terminalHostingController.view)
        terminalHostingController.didMove(toParent: self)
    }

    /// Central layout function used at startup and after rotations.
    private func rebuildLayout() {
        // Clear old constraints
        if !activeConstraints.isEmpty {
            NSLayoutConstraint.deactivate(activeConstraints)
            activeConstraints.removeAll()
        }

        var newConstraints: [NSLayoutConstraint] = []

        // --- Terminal View Constraints ---
        newConstraints.append(contentsOf: [
            // Pin top to the safe-area top (so we stay below the notch / status bar)
            terminalHostingController.view.topAnchor.constraint(
                equalTo: view.safeAreaLayoutGuide.topAnchor
            ),

            // Fill horizontally; safe-area here is optional, but harmless.
            terminalHostingController.view.leadingAnchor.constraint(
                equalTo: view.safeAreaLayoutGuide.leadingAnchor
            ),
            terminalHostingController.view.trailingAnchor.constraint(
                equalTo: view.safeAreaLayoutGuide.trailingAnchor
            ),

            // Hard-pin bottom to the keyboard layout guide.
            // When the keyboard is hidden, keyboardLayoutGuide.top == safeArea.bottom.
            terminalHostingController.view.bottomAnchor.constraint(
                equalTo: view.keyboardLayoutGuide.topAnchor
            )
        ])

        NSLayoutConstraint.activate(newConstraints)
        activeConstraints = newConstraints
    }
}

// MARK: - SwiftUI Bridge & Entry Point

struct TerminalContainerView: UIViewControllerRepresentable {
    func makeUIViewController(context: Context) -> TerminalRootViewController {
        TerminalRootViewController()
    }

    func updateUIViewController(
        _ uiViewController: TerminalRootViewController,
        context: Context
    ) {
        // No-op
    }
}

@main
struct PscalApp: App {
    @UIApplicationDelegateAdaptor(PscalAppDelegate.self) var appDelegate

    init() {
        // Safe to call multiple times; PscalRuntimeBootstrap.start() is idempotent.
        PscalRuntimeBootstrap.shared.start()
    }

    var body: some Scene {
        WindowGroup {
            TerminalContainerView()
        }
    }
}
