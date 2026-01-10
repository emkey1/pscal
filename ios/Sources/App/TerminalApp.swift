import SwiftUI

@main
struct TerminalApp: App {
    @UIApplicationDelegateAdaptor(PscalAppDelegate.self) var appDelegate

    var body: some Scene {
        WindowGroup {
            TerminalViewControllerWrapper()
                .preferredColorScheme(.dark)
        }
    }
}

/// SwiftUI bridge that hosts the UIKit terminal controller.
struct TerminalViewControllerWrapper: UIViewControllerRepresentable {
    func makeUIViewController(context: Context) -> TerminalViewController {
        TerminalViewController()
    }

    func updateUIViewController(_ uiViewController: TerminalViewController, context: Context) {}
}
