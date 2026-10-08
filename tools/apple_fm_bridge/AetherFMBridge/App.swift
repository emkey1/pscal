import SwiftUI
import UIKit

/// Aether FM Bridge: serves Apple's Foundation Models (on-device and Private
/// Cloud Compute) to the Aether benchmark harness over the local network. Keep
/// it in the foreground on power: iPadOS suspends a backgrounded app.
@main
struct AetherFMBridgeApp: App {
    var body: some Scene {
        WindowGroup { BridgeView() }
    }
}

@MainActor
final class BridgeState: ObservableObject {
    static let port: UInt16 = 8799
    @Published var log: [String] = []
    @Published var models: String = "checking…"
    let token: String
    let engine = InferenceEngine()
    private var server: BridgeServer?

    init() {
        let defaults = UserDefaults.standard
        if let saved = defaults.string(forKey: "bridgeToken") {
            token = saved
        } else {
            let fresh = (0..<4).map { _ in String(UInt64.random(in: .min ... .max), radix: 36) }.joined()
            defaults.set(fresh, forKey: "bridgeToken")
            token = fresh
        }
    }

    func start() {
        UIApplication.shared.isIdleTimerDisabled = true
        let server = BridgeServer(port: Self.port, token: token, engine: engine)
        server.onLog = { line in Task { @MainActor in self.append(line) } }
        do {
            try server.start()
            self.server = server
            append("listening on port \(Self.port)")
        } catch {
            append("could not listen: \(error)")
        }
        Task {
            let list = await engine.describeModels()
            models = list.map { m in
                "\(m["id"] ?? "?"): \(m["availability"] ?? "?"), context \(m["context_length"] ?? "?")"
                    + ((m["variant"] as? String).map { ", variant \($0)" } ?? "")
            }.joined(separator: "\n")
        }
    }

    private func append(_ line: String) {
        let stamp = Date().formatted(date: .omitted, time: .standard)
        log.append("\(stamp)  \(line)")
        if log.count > 200 { log.removeFirst(log.count - 200) }
    }
}

struct BridgeView: View {
    @StateObject private var state = BridgeState()

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Aether FM Bridge").font(.largeTitle).bold()
            Text("Port \(BridgeState.port)").font(.title3)
            Text("Token: \(state.token)").font(.body.monospaced()).textSelection(.enabled)
            Text(state.models).font(.body.monospaced())
            Divider()
            ScrollView {
                VStack(alignment: .leading) {
                    ForEach(Array(state.log.enumerated()), id: \.offset) { _, line in
                        Text(line).font(.caption.monospaced())
                    }
                }
            }
        }
        .padding()
        .onAppear { state.start() }
    }
}
