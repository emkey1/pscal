import SwiftUI
import UIKit

@main
struct PscalApp: App {
    @UIApplicationDelegateAdaptor(PscalAppDelegate.self) var appDelegate

    init() {
        // Safe to call multiple times; PscalRuntimeBootstrap.start() is idempotent.
        PscalRuntimeBootstrap.shared.start()
    }

    var body: some Scene {
        WindowGroup {
            TerminalView(showsOverlay: true)
                .onAppear {
                    // Set default window background color to match terminal default
                    // This helps avoid white flashes during rotation/startup
                    if let window = UIApplication.shared.windows.first {
                        window.backgroundColor = .black
                    }
                }
        }
    }
}

// Keep the AppDelegate as it might be used for other lifecycle events in the future
// even if we moved away from the complex VC structure.
class PscalAppDelegate: NSObject, UIApplicationDelegate {
    func application(
        _ application: UIApplication,
        didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey : Any]? = nil
    ) -> Bool {
        return true
    }
}
