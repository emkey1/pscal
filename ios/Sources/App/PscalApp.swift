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
            TerminalRootView()
                .ignoresSafeArea() // Let TerminalRootViewController handle safe areas via UIKit guides
                .onAppear {
                    // Set default window background color to match terminal default
                    // This helps avoid white flashes during rotation/startup
                    // Also register the main scene with EditorWindowManager
                    if let windowScene = UIApplication.shared.connectedScenes
                        .compactMap({ $0 as? UIWindowScene })
                        .first(where: { $0.activationState == .foregroundActive }) ??
                        UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene }).first,
                       let window = windowScene.windows.first {
                        window.backgroundColor = .black
                        EditorWindowManager.shared.mainSceneDidConnect(session: windowScene.session)
                    }
                }
        }
    }
}
