import SwiftUI
import UIKit

@main
struct PscalApp: App {
    @UIApplicationDelegateAdaptor(PscalAppDelegate.self) var appDelegate

    var body: some Scene {
        WindowGroup {
            TerminalTabsRootView()
                .onAppear {
                    // Set default window background color to match terminal default
                    // This helps avoid white flashes during rotation/startup
                    if let windowScene = UIApplication.shared.connectedScenes
                        .compactMap({ $0 as? UIWindowScene })
                        .first(where: { $0.activationState == .foregroundActive }),
                       let window = windowScene.windows.first {
                        window.backgroundColor = .black
                    }
                }
        }
    }
}
