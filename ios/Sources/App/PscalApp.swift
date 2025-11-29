import SwiftUI
import UIKit

/// Temporary SwiftUI entry point for the PSCAL iOS runner.
/// The real implementation will host the exsh terminal and SDL view.
@main
struct PscalApp: App {
    @UIApplicationDelegateAdaptor(PscalAppDelegate.self) var appDelegate

    init() {
        PscalRuntimeBootstrap.shared.start()
    }

    var body: some Scene {
        WindowGroup {
            TerminalView()
        }
    }
}
