import SwiftUI
import Foundation
import UIKit
import Darwin

@MainActor
final class TerminalTabManager: ObservableObject {
    static let shared = TerminalTabManager()
    private static let multiTabDebugEnabled: Bool = {
        guard let value = ProcessInfo.processInfo.environment["PSCALI_MULTI_TAB_DEBUG"] else {
            return false
        }
        return value != "0"
    }()

    struct Tab: Identifiable {
        enum Kind {
            case shell(PscalRuntimeBootstrap)
            case shellSession(ShellRuntimeSession)
            case ssh(SshRuntimeSession)
        }

        let id: UInt64
        var title: String
        var sessionId: UInt64?
        let kind: Kind
    }

    enum ShellCloseResult {
        case closed
        case root
        case missing
    }

    private let shellId: UInt64 = PSCALRuntimeNextSessionId()
    @Published private(set) var tabs: [Tab]
    @Published var selectedId: UInt64 {
        didSet {
            if oldValue != selectedId {
                logMultiTab("selected tab id=\(selectedId)")
                tabInitLog("selected tab id=\(selectedId) tabs=\(tabs.count)")
            }
        }
    }
    private var nextShellOrdinal: Int = 1
    private var pgidToTab: [Int: UInt64] = [:]
    private func scheduleFocusDance(primary: UInt64, secondary: UInt64) {
        // Avoid tab switching; just reassert focus on the selected tab a few times.
        let base: TimeInterval = 0.20
        let interval: TimeInterval = 0.15
        for i in 0..<6 {
            let delay = base + (interval * Double(i))
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
                NotificationCenter.default.post(name: .terminalInputFocusRequested, object: nil)
            }
        }
    }

    private init() {
        let runtime = PscalRuntimeBootstrap.shared
        let shellTab = Tab(id: shellId,
                          title: TerminalTabManager.sanitizeTitle("Shell"),
                          sessionId: nil,
                          kind: .shell(runtime))
        tabs = [shellTab]
        selectedId = shellId
        nextShellOrdinal = 2
        runtime.assignTabId(shellId)
        logMultiTab("init shell tab id=\(shellId) runtime=\(runtime.runtimeId)")
        tabInitLog("manager init shellTab=\(shellId) runtime=\(runtime.runtimeId) thread=\(Thread.isMainThread ? "main" : "bg")")
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
        tabInitLog("openShellTab request thread=\(Thread.isMainThread ? "main" : "bg") tabs=\(tabs.count)")
        let previousId = selectedId
        let newId = PSCALRuntimeNextSessionId()
        if nextShellOrdinal < 1 {
            nextShellOrdinal = 1
        }
        let title = TerminalTabManager.sanitizeTitle("Shell \(nextShellOrdinal)")
        nextShellOrdinal += 1
        let runtime = PscalRuntimeBootstrap()
        let tab = Tab(id: newId, title: title, sessionId: nil, kind: .shell(runtime))
        tabs.append(tab)
        selectedId = newId
        runtime.assignTabId(newId)
        logMultiTab("open shell tab id=\(newId) runtime=\(runtime.runtimeId)")
        tabInitLog("openShellTab created id=\(newId) runtime=\(runtime.runtimeId) title=\(title)")
        tabInitLog("openShellTab selectedId=\(selectedId) tabs=\(tabs.count)")
        scheduleFocusDance(primary: newId, secondary: previousId)
        return 0
    }
    
    func registerShellPgid(_ pgid: Int, tabId: UInt64) {
        guard pgid > 0 else { return }
        pgidToTab[pgid] = tabId
    }

    func unregisterShellPgid(_ pgid: Int) {
        pgidToTab.removeValue(forKey: pgid)
    }

    func closeTabForPgid(_ pgid: Int) {
        guard pgid > 0 else { return }
        guard let tabId = pgidToTab[pgid],
              let index = tabs.firstIndex(where: { $0.id == tabId }) else { return }
        let tab = tabs[index]
        pgidToTab.removeValue(forKey: pgid)
        if case .shell(let runtime) = tab.kind {
            _ = closeShellTab(tabId: tab.id, runtime: runtime, status: 0)
        }
    }

    func closeSelectedTab() {
        let tab = selectedTab
        if tab.id == shellId {
            tabInitLog("closeSelectedTab root id=\(tab.id)")
            if case .shell(let runtime) = tab.kind {
                runtime.requestClose()
            }
            return
        }
        guard let index = tabs.firstIndex(where: { $0.id == tab.id }) else { return }
        let removedId = removeTab(at: index)
        tabInitLog("closeSelectedTab removed id=\(removedId) tabs=\(tabs.count)")
        switch tab.kind {
        case .shell(let runtime):
            if runtime.exitStatus == nil {
                tabInitLog("closeSelectedTab request shell id=\(tab.id)")
                runtime.requestClose()
            }
        case .shellSession(let session):
            if session.exitStatus == nil {
                tabInitLog("closeSelectedTab request shellSession id=\(tab.id)")
                session.requestClose()
            }
        case .ssh(let session):
            if session.exitStatus == nil {
                tabInitLog("closeSelectedTab request ssh id=\(tab.id)")
                session.requestClose()
            }
        }
    }

    @discardableResult
    func closeShellTab(runtime: PscalRuntimeBootstrap, status: Int32) -> ShellCloseResult {
        closeShellTab(tabId: 0, runtime: runtime, status: status)
    }

    @discardableResult
    func closeShellTab(tabId: UInt64, runtime: PscalRuntimeBootstrap, status: Int32) -> ShellCloseResult {
        let index: Int? = {
            if tabId != 0, let byId = tabs.firstIndex(where: { $0.id == tabId }) {
                return byId
            }
            return tabs.firstIndex(where: { tab in
                if case .shell(let tabRuntime) = tab.kind {
                    return tabRuntime === runtime
                }
                return false
            })
        }()
        guard let index else { return .missing }
        guard case .shell = tabs[index].kind else { return .missing }
        if tabs[index].id == shellId {
            tabInitLog("closeShellTab ignored root id=\(tabs[index].id)")
            return .root
        }
        if case .shell(let runtime) = tabs[index].kind {
            let pgid = runtime.currentShellPgid()
            if pgid > 0 {
                pgidToTab.removeValue(forKey: pgid)
            }
        }
        let removedId = removeTab(at: index)
        logMultiTab("shell tab exit id=\(removedId) status=\(status)")
        tabInitLog("closeShellTab removed id=\(removedId) tabs=\(tabs.count)")
        return .closed
    }

    func requestAppExit() {
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        if scenes.isEmpty {
            exit(0)
        }
        for scene in scenes {
            UIApplication.shared.requestSceneSessionDestruction(scene.session, options: nil, errorHandler: nil)
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) {
            exit(0)
        }
    }

    func openSshSession(argv: [String]) -> Int32 {
        tabInitLog("openSshSession request thread=\(Thread.isMainThread ? "main" : "bg") tabs=\(tabs.count)")
        if hasActiveSshSession {
            errno = EBUSY
            tabInitLog("openSshSession rejected (existing ssh)")
            return -1
        }
        let sessionId = PSCALRuntimeNextSessionId()
        let session = SshRuntimeSession(sessionId: sessionId, argv: argv)
        guard session.start() else {
            let err = session.lastStartErrno
            tabInitLog("openSshSession start failed errno=\(err)")
            return -(err == 0 ? EIO : err)
        }
        let title = TerminalTabManager.sanitizeTitle(sshTitle(argv: argv))
        let tab = Tab(id: sessionId, title: title, sessionId: sessionId, kind: .ssh(session))
        tabs.append(tab)
        selectedId = sessionId
        logMultiTab("open ssh tab id=\(sessionId) title=\(title)")
        tabInitLog("openSshSession created id=\(sessionId) title=\(title)")
        tabInitLog("openSshSession selectedId=\(selectedId) tabs=\(tabs.count)")
        return 0
    }

    func handleSessionExit(sessionId: UInt64, status: Int32) {
        tabInitLog("sessionExit id=\(sessionId) status=\(status) thread=\(Thread.isMainThread ? "main" : "bg")")
        guard let index = tabs.firstIndex(where: { $0.id == sessionId }) else { return }
        switch tabs[index].kind {
        case .shell:
            return
        case .shellSession(let session):
            session.markExited(status: status)
        case .ssh(let session):
            session.markExited(status: status)
        }
        let removedId = removeTab(at: index)
        logMultiTab("session exit id=\(removedId) status=\(status)")
        tabInitLog("sessionExit removed id=\(removedId) tabs=\(tabs.count) selectedId=\(selectedId)")
    }

    @discardableResult
    private func removeTab(at index: Int) -> UInt64 {
        let removedId = tabs[index].id
        tabs.remove(at: index)
        if selectedId == removedId {
            if let shellTab = tabs.first(where: { tab in
                switch tab.kind {
                case .shell, .shellSession:
                    return true
                case .ssh:
                    return false
                }
            }) {
                selectedId = shellTab.id
            } else if let first = tabs.first {
                selectedId = first.id
            }
        }
        return removedId
    }

    var selectedTab: Tab {
        tabs.first(where: { $0.id == selectedId }) ?? tabs[0]
    }

    func selectTab(number: Int) {
        let target = number == 0 ? 10 : number
        guard target > 0 else { return }
        let index = target - 1
        guard tabs.indices.contains(index) else { return }
        let tab = tabs[index]
        guard tab.id != selectedId else { return }
        tabInitLog("selectTab number=\(target) id=\(tab.id) title=\(tab.title)")
        selectedId = tab.id
    }

    func sendInputToSelected(_ text: String) {
        guard !text.isEmpty else { return }
        tabInitLog("sendInputToSelected len=\(text.utf8.count) selectedId=\(selectedId)")
        switch selectedTab.kind {
        case .shell(let runtime):
            runtime.send(text)
        case .shellSession(let session):
            session.send(text)
        case .ssh(let session):
            session.send(text)
        }
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

    func registerShellSession(tabId: UInt64, sessionId: UInt64) {
        guard sessionId != 0, let idx = tabs.firstIndex(where: { $0.id == tabId }) else { return }
        tabs[idx].sessionId = sessionId
    }

    fileprivate func updateTitle(forSessionId sessionId: UInt64, rawTitle: String) -> Bool {
        // Prefer an exact session match; otherwise fall back to the currently selected tab, and
        // finally any tab (first) so that we never fail during early startup when sessionId
        // registration may lag behind the tab being visible.
        let targetIdx =
            tabs.firstIndex(where: { $0.sessionId == sessionId }) ??
            tabs.firstIndex(where: { $0.id == selectedId }) ??
            tabs.indices.first
        guard let idx = targetIdx else { return false }
        let title = TerminalTabManager.sanitizeTitle(rawTitle)
        if tabs[idx].sessionId == nil && sessionId != 0 {
            tabs[idx].sessionId = sessionId
        }
        tabs[idx].title = title
        return true
    }

    fileprivate static func sanitizeTitle(_ raw: String) -> String {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty {
            return "Session"
        }
        return String(trimmed.prefix(15))
    }

    private func logMultiTab(_ message: String) {
        guard Self.multiTabDebugEnabled else { return }
        runtimeDebugLog("[MultiTab] \(message)")
    }
}

struct TerminalTabsRootView: View {
    @ObservedObject private var manager = TerminalTabManager.shared

    var body: some View {
        if TerminalDebugFlags.printChanges {
            let _ = Self._printChanges()
            traceViewChanges("TerminalTabsRootView body")
        }
        return VStack(spacing: 0) {
            TerminalTabBar(tabs: manager.tabs, selectedId: $manager.selectedId)
            ZStack {
                let tab = manager.selectedTab
                tabContent(for: tab, isSelected: true)
                    .id(tab.id)
            }
        }
        .onChange(of: manager.selectedId) { _ in
            NotificationCenter.default.post(name: .terminalInputFocusRequested, object: nil)
        }
    }

    @ViewBuilder
    private func tabContent(for tab: TerminalTabManager.Tab, isSelected: Bool) -> some View {
        switch tab.kind {
        case .shell(let runtime):
            TerminalView(showsOverlay: true, isActive: isSelected, runtime: runtime)
        case .shellSession(let session):
            ShellTerminalView(session: session, isActive: isSelected)
        case .ssh(let session):
            SshTerminalView(session: session, isActive: isSelected)
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
                        if selectedId != tab.id {
                            selectedId = tab.id
                        }
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

@MainActor
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

private func runOnMainBlocking<T>(_ label: String, work: @MainActor @escaping () -> T) -> T? {
    let start = DispatchTime.now()
    tabInitLog("\(label) enqueue thread=\(Thread.isMainThread ? "main" : "bg")")
    if Thread.isMainThread {
        return MainActor.assumeIsolated {
            work()
        }
    }
    var result: T?
    let semaphore = DispatchSemaphore(value: 0)
    Task { @MainActor in
        result = work()
        semaphore.signal()
    }
    semaphore.wait()
    let end = DispatchTime.now()
    let elapsedMs = Double(end.uptimeNanoseconds - start.uptimeNanoseconds) / 1_000_000.0
    tabInitLog("\(label) completed wait_ms=\(String(format: "%.1f", elapsedMs))")
    return result
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
    if let first = args.first {
        sshDebugLog("[ssh-session] open request argc=\(argc) argv0=\(first)")
    } else {
        sshDebugLog("[ssh-session] open request argc=\(argc)")
    }
    if let result = runOnMainBlocking("openSshSession", work: { TerminalTabManager.shared.openSshSession(argv: args) }) {
        sshDebugLog("[ssh-session] open result=\(result)")
        return result
    }
    sshDebugLog("[ssh-session] open result=EAGAIN")
    return -EAGAIN
}

@_cdecl("pscalRuntimeOpenShellTab")
func pscalRuntimeOpenShellTab() -> Int32 {
    if let result = runOnMainBlocking("openShellTab", work: { TerminalTabManager.shared.openShellTab() }) {
        return result
    }
    return -EAGAIN
}

@_cdecl("pscalRuntimeSetTabTitleForSession")
func pscalRuntimeSetTabTitleForSession(_ sessionId: UInt64, _ titlePtr: UnsafePointer<CChar>?) -> Int32 {
    let raw = titlePtr.map { String(cString: $0) } ?? ""
    if let success = runOnMainBlocking("setTabTitle", work: {
        TerminalTabManager.shared.updateTitle(forSessionId: sessionId, rawTitle: raw)
    }), success {
        return 0
    }
    return -EINVAL
}

@_cdecl("pscalRuntimeSshSessionExited")
func pscalRuntimeSshSessionExited(_ sessionId: UInt64, _ status: Int32) {
    Task { @MainActor in
        TerminalTabManager.shared.handleSessionExit(sessionId: sessionId, status: status)
    }
}

@_cdecl("pscalRuntimeShellSessionExited")
func pscalRuntimeShellSessionExited(_ sessionId: UInt64, _ status: Int32) {
    Task { @MainActor in
        TerminalTabManager.shared.handleSessionExit(sessionId: sessionId, status: status)
    }
}
