import SwiftUI

/// Temporary SwiftUI entry point for the PSCAL iOS runner.
/// The real implementation will host the exsh terminal and SDL view.
@main
struct PscalApp: App {
    init() {
        PscalRuntimeBootstrap.shared.start()
    }

    var body: some Scene {
        WindowGroup {
            TerminalView()
        }
    }
}
