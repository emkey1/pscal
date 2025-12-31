import SwiftUI
import Foundation
import Darwin

final class TerminalTabManager: ObservableObject {
    static let shared = TerminalTabManager()

    struct Tab: Identifiable {
        enum Kind {
            case shellMain
            case shell(ShellRuntimeSession)
            case ssh(SshRuntimeSession)
        }

        let id: UInt64
        let title: String
        let kind: Kind
    }

    private let shellId: UInt64 = 1
    private var nextShellId: UInt64 = 2
    @Published private(set) var tabs: [Tab]
    @Published var selectedId: UInt64

    private init() {
        let shellTab = Tab(id: shellId, title: "Shell", kind: .shellMain)
        tabs = [shellTab]
        selectedId = shellId
    }

    var hasActiveSshSession: Bool {
        tabs.contains { tab in
            if case .ssh = tab.kind {
                return true
            }
            return false
        }
    }

    func openShellTab() -> Int32 {
        var result: Int32 = 0
        let argv = ["exsh"]
        let action = {
            let shellCount = self.tabs.filter { tab in
                switch tab.kind {
                case .shellMain, .shell:
                    return true
                case .ssh:
                    return false
                }
            }.count
            var newId = self.nextShellId
            while self.tabs.contains(where: { $0.id == newId }) {
                newId &+= 1
            }
            self.nextShellId = newId &+ 1
            let title = "Shell \(shellCount + 1)"
            let session = ShellRuntimeSession(sessionId: newId, argv: argv)
            guard session.start() else {
                let err = session.lastStartErrno
                result = -(err == 0 ? EIO : err)
                return
            }
            let tab = Tab(id: newId, title: title, kind: .shell(session))
            self.tabs.append(tab)
            self.selectedId = newId
        }
        if Thread.isMainThread {
            action()
        } else {
            DispatchQueue.main.sync(execute: action)
        }
        return result
    }

    func openSshSession(argv: [String]) -> Int32 {
        var result: Int32 = 0
        let action = {
            if self.hasActiveSshSession {
                errno = EBUSY
                result = -1
                return
            }
            let sessionId = UInt64(Date().timeIntervalSince1970 * 1000.0)
            let session = SshRuntimeSession(sessionId: sessionId, argv: argv)
            guard session.start() else {
                let err = session.lastStartErrno
                result = -(err == 0 ? EIO : err)
                return
            }
            let title = self.sshTitle(argv: argv)
            let tab = Tab(id: sessionId, title: title, kind: .ssh(session))
            self.tabs.append(tab)
            self.selectedId = sessionId
        }
        if Thread.isMainThread {
            action()
        } else {
            DispatchQueue.main.sync(execute: action)
        }
        return result
    }

    func handleSessionExit(sessionId: UInt64, status: Int32) {
        let action = {
            guard let index = self.tabs.firstIndex(where: { $0.id == sessionId }) else { return }
            switch self.tabs[index].kind {
            case .shellMain:
                return
            case .shell(let session):
                session.markExited(status: status)
            case .ssh(let session):
                session.markExited(status: status)
            }
            self.tabs.remove(at: index)
            if self.selectedId == sessionId {
                if let shellTab = self.tabs.first(where: { tab in
                    switch tab.kind {
                    case .shellMain, .shell:
                        return true
                    case .ssh:
                        return false
                    }
                }) {
                    self.selectedId = shellTab.id
                } else if let first = self.tabs.first {
                    self.selectedId = first.id
                }
            }
        }
        if Thread.isMainThread {
            action()
        } else {
            DispatchQueue.main.async(execute: action)
        }
    }

    var selectedTab: Tab {
        tabs.first(where: { $0.id == selectedId }) ?? tabs[0]
    }

    private func sshTitle(argv: [String]) -> String {
        guard argv.count > 1 else { return "SSH" }
        var skipNext = false
        for arg in argv.dropFirst() {
            if skipNext {
                skipNext = false
                continue
            }
            if arg == "-l" || arg == "-p" || arg == "-i" || arg == "-F" || arg == "-o" || arg == "-b" {
                skipNext = true
                continue
            }
            if arg.hasPrefix("-") {
                continue
            }
            return "SSH \(arg)"
        }
        return "SSH"
    }
}

struct TerminalTabsRootView: View {
    @ObservedObject private var manager = TerminalTabManager.shared

    var body: some View {
        VStack(spacing: 0) {
            TerminalTabBar(tabs: manager.tabs, selectedId: $manager.selectedId)
            selectedContent
        }
        .onChange(of: manager.selectedId) { _ in
            NotificationCenter.default.post(name: .terminalInputFocusRequested, object: nil)
        }
    }

    @ViewBuilder
    private var selectedContent: some View {
        switch manager.selectedTab.kind {
        case .shellMain:
            TerminalView(showsOverlay: true)
        case .shell(let session):
            ShellTerminalView(session: session)
        case .ssh(let session):
            SshTerminalView(session: session)
        }
    }
}

private struct TerminalTabBar: View {
    let tabs: [TerminalTabManager.Tab]
    @Binding var selectedId: UInt64

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                ForEach(tabs) { tab in
                    TerminalTabButton(
                        title: tab.title,
                        isSelected: tab.id == selectedId
                    ) {
                        selectedId = tab.id
                    }
                }
                Button(action: { _ = TerminalTabManager.shared.openShellTab() }) {
                    Image(systemName: "plus")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundColor(.secondary)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 6)
                        .background(
                            RoundedRectangle(cornerRadius: 12)
                                .fill(Color(.secondarySystemBackground))
                        )
                }
                .buttonStyle(.plain)
                .accessibilityLabel("New Shell Tab")
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 6)
        }
        .background(Color(.secondarySystemBackground))
        .overlay(Divider(), alignment: .bottom)
    }
}

private struct TerminalTabButton: View {
    let title: String
    let isSelected: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 14, weight: .semibold))
                .foregroundColor(isSelected ? .primary : .secondary)
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .background(
                    RoundedRectangle(cornerRadius: 12)
                        .fill(isSelected ? Color(.systemBackground) : Color(.secondarySystemBackground))
                )
        }
        .buttonStyle(.plain)
    }
}

@_cdecl("pscalRuntimeOpenSshSession")
func pscalRuntimeOpenSshSession(_ argc: Int32,
                                _ argv: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?) -> Int32 {
    guard argc > 0, let argv else { return -1 }
    var args: [String] = []
    args.reserveCapacity(Int(argc))
    for i in 0..<Int(argc) {
        if let ptr = argv[i] {
            args.append(String(cString: ptr))
        } else {
            args.append("")
        }
    }
    return TerminalTabManager.shared.openSshSession(argv: args)
}

@_cdecl("pscalRuntimeOpenShellTab")
func pscalRuntimeOpenShellTab() -> Int32 {
    return TerminalTabManager.shared.openShellTab()
}

@_cdecl("pscalRuntimeSshSessionExited")
func pscalRuntimeSshSessionExited(_ sessionId: UInt64, _ status: Int32) {
    TerminalTabManager.shared.handleSessionExit(sessionId: sessionId, status: status)
}

@_cdecl("pscalRuntimeShellSessionExited")
func pscalRuntimeShellSessionExited(_ sessionId: UInt64, _ status: Int32) {
    TerminalTabManager.shared.handleSessionExit(sessionId: sessionId, status: status)
}
