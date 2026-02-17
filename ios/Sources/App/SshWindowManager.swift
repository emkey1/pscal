import SwiftUI
import Foundation
import UIKit
import Darwin
import Combine

@MainActor
final class TerminalTabManager: ObservableObject {
    static let shared = TerminalTabManager()
    private static let multiTabDebugEnabled: Bool = {
        guard let value = ProcessInfo.processInfo.environment["PSCALI_MULTI_TAB_DEBUG"] else {
            return false
        }
        return value != "0"
    }()
    private static let tabTitleDefaultsPrefix = "com.pscal.terminal.tabAppearance."
    private static let tabStartupCommandDefaultsPrefix = "com.pscal.terminal.tabAppearance."
    private static let tabTitleResetOnStartDefaultsPrefix = "com.pscal.terminal.tabAppearance."

    struct Tab: Identifiable {
        enum Kind {
            case shell(PscalRuntimeBootstrap)
            case shellSession(ShellRuntimeSession)
            case ssh(SshRuntimeSession)
            case sdl(ownerTabId: UInt64)
        }

        let id: UInt64
        var title: String
        var startupCommand: String
        var sessionId: UInt64?
        let appearanceProfileID: String
        let appearanceSettings: TerminalTabAppearanceSettings
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
                handleTabSelectionChange(oldId: oldValue, newId: selectedId)
                logMultiTab("selected tab id=\(selectedId)")
                tabInitLog("selected tab id=\(selectedId) tabs=\(tabs.count)")
                bindSelectedAppearanceSettings()
            }
        }
    }
    private var pgidToTab: [Int: UInt64] = [:]
    private var appearanceSettingsByProfileID: [String: TerminalTabAppearanceSettings] = [:]
    private var startupCommandAppliedSessions: Set<UInt64> = []
    private var promptReadySessions: Set<UInt64> = []
    private var activeSdlTabId: UInt64?
    private var selectedAppearanceObserver: AnyCancellable?
    private func scheduleFocusDance(primary: UInt64, secondary: UInt64) {
        // Avoid tab switching; just reassert focus on the selected tab a few times.
        let base: TimeInterval = 0.50
        let interval: TimeInterval = 0.25
        for i in 0..<6 {
            let delay = base + (interval * Double(i))
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
                NotificationCenter.default.post(name: .terminalInputFocusRequested, object: nil)
            }
        }
    }

    private init() {
        let runtime = PscalRuntimeBootstrap.shared
        let rootProfileID = "shell.1"
        let rootAppearance = TerminalTabAppearanceSettings(profileID: rootProfileID)
        let rootTitle = Self.startupTabTitle(
            forProfileID: rootProfileID,
            fallback: TerminalTabManager.sanitizeTitle("Shell"),
            restoreProfileDefaults: true
        )
        let rootStartupCommand = Self.persistedStartupCommand(forProfileID: rootProfileID)
        let shellTab = Tab(id: shellId,
                          title: rootTitle,
                          startupCommand: rootStartupCommand,
                          sessionId: nil,
                          appearanceProfileID: rootProfileID,
                          appearanceSettings: rootAppearance,
                          kind: .shell(runtime))
        tabs = [shellTab]
        selectedId = shellId
        appearanceSettingsByProfileID[rootProfileID] = rootAppearance
        runtime.assignTabId(shellId)
        bindSelectedAppearanceSettings()
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

    func openShellTab(restoreProfileDefaults: Bool = true) -> Int32 {
        tabInitLog("openShellTab request thread=\(Thread.isMainThread ? "main" : "bg") tabs=\(tabs.count)")
        let previousId = selectedId
        let newId = PSCALRuntimeNextSessionId()
        let shellSlot = nextAvailableShellProfileSlot()
        let defaultTitle = TerminalTabManager.sanitizeTitle("Shell \(shellSlot)")
        let profileID = "shell.\(shellSlot)"
        let title = Self.startupTabTitle(
            forProfileID: profileID,
            fallback: defaultTitle,
            restoreProfileDefaults: restoreProfileDefaults
        )
        let startupCommand = restoreProfileDefaults
            ? Self.persistedStartupCommand(forProfileID: profileID)
            : ""
        let appearance = appearanceSettings(forProfileID: profileID)
        let runtime = PscalRuntimeBootstrap()
        let tab = Tab(id: newId,
                      title: title,
                      startupCommand: startupCommand,
                      sessionId: nil,
                      appearanceProfileID: profileID,
                      appearanceSettings: appearance,
                      kind: .shell(runtime))
        tabs.append(tab)
        selectedId = newId
        runtime.assignTabId(newId)
        logMultiTab("open shell tab id=\(newId) runtime=\(runtime.runtimeId)")
        tabInitLog("openShellTab created id=\(newId) runtime=\(runtime.runtimeId) title=\(title) restore=\(restoreProfileDefaults ? 1 : 0)")
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
        case .sdl(let ownerTabId):
            tabInitLog("closeSelectedTab request sdl id=\(tab.id) owner=\(ownerTabId)")
            if let ownerKind = kind(forTabId: ownerTabId) {
                sendInterrupt(for: ownerKind)
            }
            pscalIOSRestoreTerminalWindowKey()
        }
        let removedId = removeTab(at: index)
        tabInitLog("closeSelectedTab removed id=\(removedId) tabs=\(tabs.count)")
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
        let defaultTitle = TerminalTabManager.sanitizeTitle(sshTitle(argv: argv))
        let profileID = "ssh.1"
        let title = Self.startupTabTitle(
            forProfileID: profileID,
            fallback: defaultTitle,
            restoreProfileDefaults: true
        )
        let startupCommand = Self.persistedStartupCommand(forProfileID: profileID)
        let appearance = appearanceSettings(forProfileID: profileID)
        let tab = Tab(id: sessionId,
                      title: title,
                      startupCommand: startupCommand,
                      sessionId: sessionId,
                      appearanceProfileID: profileID,
                      appearanceSettings: appearance,
                      kind: .ssh(session))
        tabs.append(tab)
        // SSH sessions do not emit the exsh prompt-ready callback, so keep
        // startup-command behavior as immediate best-effort for ssh tabs.
        promptReadySessions.insert(sessionId)
        applyStartupCommandIfReady(forSessionId: sessionId)
        selectedId = sessionId
        logMultiTab("open ssh tab id=\(sessionId) title=\(title)")
        tabInitLog("openSshSession created id=\(sessionId) title=\(title)")
        tabInitLog("openSshSession selectedId=\(selectedId) tabs=\(tabs.count)")
        return 0
    }

    func handleSessionExit(sessionId: UInt64, status: Int32) {
        tabInitLog("sessionExit id=\(sessionId) status=\(status) thread=\(Thread.isMainThread ? "main" : "bg")")
        guard let index = tabs.firstIndex(where: { $0.id == sessionId }) else { return }
        startupCommandAppliedSessions.remove(sessionId)
        promptReadySessions.remove(sessionId)
        switch tabs[index].kind {
        case .shell:
            return
        case .shellSession(let session):
            session.markExited(status: status)
        case .ssh(let session):
            session.markExited(status: status)
        case .sdl:
            return
        }
        let removedId = removeTab(at: index)
        logMultiTab("session exit id=\(removedId) status=\(status)")
        tabInitLog("sessionExit removed id=\(removedId) tabs=\(tabs.count) selectedId=\(selectedId)")
    }

    @discardableResult
    private func removeTab(at index: Int) -> UInt64 {
        let removedId = tabs[index].id
        if let sessionId = tabs[index].sessionId {
            startupCommandAppliedSessions.remove(sessionId)
            promptReadySessions.remove(sessionId)
        }
        if removedId == activeSdlTabId {
            activeSdlTabId = nil
        }
        tabs.remove(at: index)
        if selectedId == removedId {
            if let shellTab = tabs.first(where: { tab in
                switch tab.kind {
                case .shell, .shellSession:
                    return true
                case .ssh, .sdl:
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

    var selectedAppearanceSettings: TerminalTabAppearanceSettings {
        selectedTab.appearanceSettings
    }

    var isSdlTabSelected: Bool {
        if case .sdl = selectedTab.kind {
            return true
        }
        return false
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
        case .sdl:
            break
        }
    }

    func sendInterruptToSelected() {
        tabInitLog("sendInterruptToSelected selectedId=\(selectedId)")
        switch selectedTab.kind {
        case .shell(let runtime):
            runtime.sendInterrupt()
        case .shellSession(let session):
            session.sendInterrupt()
        case .ssh(let session):
            session.send("\u{03}")
        case .sdl(let ownerTabId):
            if let ownerKind = kind(forTabId: ownerTabId) {
                sendInterrupt(for: ownerKind)
            } else {
                pscalIOSRestoreTerminalWindowKey()
            }
        }
    }

    func sendSuspendToSelected() {
        tabInitLog("sendSuspendToSelected selectedId=\(selectedId)")
        switch selectedTab.kind {
        case .shell(let runtime):
            runtime.sendSuspend()
        case .shellSession(let session):
            session.sendSuspend()
        case .ssh(let session):
            session.send("\u{1A}")
        case .sdl(let ownerTabId):
            if let ownerKind = kind(forTabId: ownerTabId) {
                sendSuspend(for: ownerKind)
            } else {
                pscalIOSRestoreTerminalWindowKey()
            }
        }
    }

    func handleSdlDidOpen() {
        if let existingIndex = tabs.firstIndex(where: { tab in
            if case .sdl = tab.kind {
                return true
            }
            return false
        }) {
            let ownerTabId = selectedId
            tabs[existingIndex] = Tab(id: tabs[existingIndex].id,
                                      title: tabs[existingIndex].title,
                                      startupCommand: tabs[existingIndex].startupCommand,
                                      sessionId: tabs[existingIndex].sessionId,
                                      appearanceProfileID: tabs[existingIndex].appearanceProfileID,
                                      appearanceSettings: tabs[existingIndex].appearanceSettings,
                                      kind: .sdl(ownerTabId: ownerTabId))
            let existing = tabs[existingIndex]
            activeSdlTabId = existing.id
            selectedId = existing.id
            return
        }

        let ownerTabId = selectedId
        let tabId = PSCALRuntimeNextSessionId()
        let profileID = "sdl.1"
        let title = TerminalTabManager.sanitizeTitle("SDL")
        let appearance = appearanceSettings(forProfileID: profileID)
        let tab = Tab(id: tabId,
                      title: title,
                      startupCommand: "",
                      sessionId: nil,
                      appearanceProfileID: profileID,
                      appearanceSettings: appearance,
                      kind: .sdl(ownerTabId: ownerTabId))
        tabs.append(tab)
        activeSdlTabId = tabId
        tabInitLog("openSdlTab id=\(tabId) owner=\(ownerTabId)")
        selectedId = tabId
    }

    func handleSdlDidClose() {
        guard let index = tabs.firstIndex(where: { tab in
            if case .sdl = tab.kind {
                return true
            }
            return false
        }) else {
            activeSdlTabId = nil
            pscalIOSRestoreTerminalWindowKey()
            return
        }

        var ownerTabId: UInt64?
        if case .sdl(let owner) = tabs[index].kind {
            ownerTabId = owner
        }
        let removedId = removeTab(at: index)
        activeSdlTabId = nil
        tabInitLog("closeSdlTab id=\(removedId)")
        if let ownerTabId,
           tabs.contains(where: { $0.id == ownerTabId }) {
            selectedId = ownerTabId
        } else if selectedId == removedId {
            selectFirstNonSdlTab()
        }
        pscalIOSRestoreTerminalWindowKey()
    }

    func selectFirstNonSdlTab() {
        if let tab = tabs.first(where: { tab in
            if case .sdl = tab.kind {
                return false
            }
            return true
        }) {
            selectedId = tab.id
        } else {
            pscalIOSRestoreTerminalWindowKey()
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
        applyStartupCommandIfReady(forSessionId: sessionId)
    }

    func handlePromptReady(sessionId: UInt64) {
        guard sessionId != 0 else { return }
        promptReadySessions.insert(sessionId)
        applyStartupCommandIfReady(forSessionId: sessionId)
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
        if tabs[idx].sessionId == nil && sessionId != 0 {
            tabs[idx].sessionId = sessionId
        }
        return applyTabTitle(rawTitle, toTabAtIndex: idx, persist: true)
    }

    fileprivate func updateStartupCommand(forSessionId sessionId: UInt64, rawCommand: String) -> Bool {
        // Prefer an exact session match; otherwise fall back to the currently selected tab, and
        // finally any tab (first) so that we never fail during early startup when sessionId
        // registration may lag behind the tab being visible.
        let targetIdx =
            tabs.firstIndex(where: { $0.sessionId == sessionId }) ??
            tabs.firstIndex(where: { $0.id == selectedId }) ??
            tabs.indices.first
        guard let idx = targetIdx else { return false }
        if tabs[idx].sessionId == nil && sessionId != 0 {
            tabs[idx].sessionId = sessionId
        }
        return applyStartupCommand(rawCommand, toTabAtIndex: idx, persist: true)
    }

    @discardableResult
    func copyFirstTabColors(tabId: UInt64) -> Bool {
        guard let targetIdx = tabs.firstIndex(where: { $0.id == tabId }) else { return false }
        guard let sourceAppearance = firstTabAppearanceSettings() else { return false }
        tabs[targetIdx].appearanceSettings.updateBackgroundColor(sourceAppearance.backgroundColor)
        tabs[targetIdx].appearanceSettings.updateForegroundColor(sourceAppearance.foregroundColor)
        return true
    }

    fileprivate static func sanitizeTitle(_ raw: String) -> String {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty {
            return "Session"
        }
        return String(trimmed.prefix(15))
    }

    fileprivate static func sanitizeStartupCommand(_ raw: String) -> String {
        raw.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private func appearanceSettings(forProfileID profileID: String) -> TerminalTabAppearanceSettings {
        if let existing = appearanceSettingsByProfileID[profileID] {
            return existing
        }
        let created = TerminalTabAppearanceSettings(profileID: profileID)
        seedAppearanceColorsFromFirstTabIfNeeded(profileID: profileID, appearance: created)
        appearanceSettingsByProfileID[profileID] = created
        return created
    }

    private func firstTabAppearanceSettings() -> TerminalTabAppearanceSettings? {
        if let root = tabs.first(where: { $0.id == shellId }) {
            return root.appearanceSettings
        }
        return tabs.first?.appearanceSettings
    }

    private func seedAppearanceColorsFromFirstTabIfNeeded(profileID: String,
                                                          appearance: TerminalTabAppearanceSettings) {
        guard let sourceAppearance = firstTabAppearanceSettings() else { return }
        if !TerminalTabAppearanceSettings.hasStoredBackgroundColor(forProfileID: profileID) {
            appearance.updateBackgroundColor(sourceAppearance.backgroundColor)
        }
        if !TerminalTabAppearanceSettings.hasStoredForegroundColor(forProfileID: profileID) {
            appearance.updateForegroundColor(sourceAppearance.foregroundColor)
        }
    }

    private func nextAvailableShellProfileSlot() -> Int {
        var usedSlots = Set<Int>()
        for tab in tabs {
            if let slot = shellProfileSlot(from: tab.appearanceProfileID) {
                usedSlots.insert(slot)
            }
        }
        var slot = 2
        while usedSlots.contains(slot) {
            slot += 1
        }
        return slot
    }

    private func shellProfileSlot(from profileID: String) -> Int? {
        guard profileID.hasPrefix("shell.") else { return nil }
        let suffix = profileID.dropFirst("shell.".count)
        guard let value = Int(suffix), value > 0 else { return nil }
        return value
    }

    private func applyTabTitle(_ rawTitle: String, toTabAtIndex index: Int, persist: Bool) -> Bool {
        guard tabs.indices.contains(index) else { return false }
        tabs[index].title = TerminalTabManager.sanitizeTitle(rawTitle)
        if persist {
            Self.persistTabTitle(tabs[index].title, forProfileID: tabs[index].appearanceProfileID)
        }
        return true
    }

    private func applyStartupCommand(_ rawCommand: String, toTabAtIndex index: Int, persist: Bool) -> Bool {
        guard tabs.indices.contains(index) else { return false }
        let sanitized = TerminalTabManager.sanitizeStartupCommand(rawCommand)
        tabs[index].startupCommand = sanitized
        if persist {
            Self.persistStartupCommand(sanitized, forProfileID: tabs[index].appearanceProfileID)
        }
        return true
    }

    private func applyStartupCommandIfReady(forSessionId sessionId: UInt64) {
        guard sessionId != 0 else { return }
        guard promptReadySessions.contains(sessionId) else { return }
        guard let index = tabs.firstIndex(where: { $0.sessionId == sessionId }) else { return }
        guard !startupCommandAppliedSessions.contains(sessionId) else { return }
        let command = tabs[index].startupCommand
        guard !command.isEmpty else { return }
        startupCommandAppliedSessions.insert(sessionId)
        let payload = command.hasSuffix("\r") ? command : command + "\r"
        switch tabs[index].kind {
        case .shell(let runtime):
            runtime.send(payload)
        case .shellSession(let session):
            session.send(payload)
        case .ssh(let session):
            session.send(payload)
        case .sdl:
            break
        }
    }

    private func kind(forTabId tabId: UInt64) -> Tab.Kind? {
        tabs.first(where: { $0.id == tabId })?.kind
    }

    private func sendInterrupt(for kind: Tab.Kind) {
        switch kind {
        case .shell(let runtime):
            runtime.sendInterrupt()
        case .shellSession(let session):
            session.sendInterrupt()
        case .ssh(let session):
            session.send("\u{03}")
        case .sdl:
            break
        }
    }

    private func sendSuspend(for kind: Tab.Kind) {
        switch kind {
        case .shell(let runtime):
            runtime.sendSuspend()
        case .shellSession(let session):
            session.sendSuspend()
        case .ssh(let session):
            session.send("\u{1A}")
        case .sdl:
            break
        }
    }

    private func activateSdlTabPresentation() {
        UIApplication.shared.sendAction(#selector(UIResponder.resignFirstResponder),
                                        to: nil,
                                        from: nil,
                                        for: nil)
        pscalIOSPromoteSDLWindow()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
            pscalIOSPromoteSDLWindow()
        }
    }

    func promoteSdlToFront() {
        activateSdlTabPresentation()
        let retryDelays: [TimeInterval] = [0.15, 0.30, 0.60, 1.0]
        for delay in retryDelays {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
                guard let self else { return }
                guard self.activeSdlTabId != nil else { return }
                pscalIOSPromoteSDLWindow()
            }
        }
    }

    private func handleTabSelectionChange(oldId: UInt64, newId: UInt64) {
        let oldKind = kind(forTabId: oldId)
        let newKind = kind(forTabId: newId)
        if case .sdl = newKind {
            activateSdlTabPresentation()
            return
        }
        if case .sdl = oldKind {
            pscalIOSRestoreTerminalWindowKey()
        }
    }

    private static func persistedTabTitle(forProfileID profileID: String, fallback: String) -> String {
        let key = tabTitleDefaultsKey(forProfileID: profileID)
        guard let stored = UserDefaults.standard.string(forKey: key) else {
            return fallback
        }
        let sanitized = TerminalTabManager.sanitizeTitle(stored)
        return sanitized.isEmpty ? fallback : sanitized
    }

    private static func startupTabTitle(forProfileID profileID: String,
                                        fallback: String,
                                        restoreProfileDefaults: Bool) -> String {
        guard restoreProfileDefaults else {
            return fallback
        }
        guard persistedResetTabTitleOnAppStart(forProfileID: profileID) else {
            return fallback
        }
        return persistedTabTitle(forProfileID: profileID, fallback: fallback)
    }

    private static func persistTabTitle(_ title: String, forProfileID profileID: String) {
        let key = tabTitleDefaultsKey(forProfileID: profileID)
        UserDefaults.standard.set(TerminalTabManager.sanitizeTitle(title), forKey: key)
    }

    private static func persistedStartupCommand(forProfileID profileID: String) -> String {
        let key = tabStartupCommandDefaultsKey(forProfileID: profileID)
        guard let stored = UserDefaults.standard.string(forKey: key) else {
            return ""
        }
        return TerminalTabManager.sanitizeStartupCommand(stored)
    }

    private static func persistStartupCommand(_ command: String, forProfileID profileID: String) {
        let key = tabStartupCommandDefaultsKey(forProfileID: profileID)
        let sanitized = TerminalTabManager.sanitizeStartupCommand(command)
        if sanitized.isEmpty {
            UserDefaults.standard.removeObject(forKey: key)
        } else {
            UserDefaults.standard.set(sanitized, forKey: key)
        }
    }

    private static func tabTitleDefaultsKey(forProfileID profileID: String) -> String {
        "\(tabTitleDefaultsPrefix)\(sanitizeProfileID(profileID)).tabTitle"
    }

    private static func tabStartupCommandDefaultsKey(forProfileID profileID: String) -> String {
        "\(tabStartupCommandDefaultsPrefix)\(sanitizeProfileID(profileID)).startupCommand"
    }

    private static func persistedResetTabTitleOnAppStart(forProfileID profileID: String) -> Bool {
        let key = tabTitleResetOnStartDefaultsKey(forProfileID: profileID)
        return UserDefaults.standard.bool(forKey: key)
    }

    private static func persistResetTabTitleOnAppStart(_ enabled: Bool, forProfileID profileID: String) {
        let key = tabTitleResetOnStartDefaultsKey(forProfileID: profileID)
        UserDefaults.standard.set(enabled, forKey: key)
    }

    private static func tabTitleResetOnStartDefaultsKey(forProfileID profileID: String) -> String {
        "\(tabTitleResetOnStartDefaultsPrefix)\(sanitizeProfileID(profileID)).resetTitleOnStart"
    }

    private static func sanitizeProfileID(_ value: String) -> String {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "._-"))
        let scalars = value.unicodeScalars.map { allowed.contains($0) ? Character($0) : "_" }
        return String(scalars)
    }

    func tabTitle(tabId: UInt64) -> String? {
        tabs.first(where: { $0.id == tabId })?.title
    }

    func resetTabNameOnAppStart(tabId: UInt64) -> Bool {
        guard let tab = tabs.first(where: { $0.id == tabId }) else { return false }
        return Self.persistedResetTabTitleOnAppStart(forProfileID: tab.appearanceProfileID)
    }

    @discardableResult
    func updateTabTitle(tabId: UInt64, rawTitle: String, persist: Bool = true) -> Bool {
        guard let idx = tabs.firstIndex(where: { $0.id == tabId }) else {
            return false
        }
        return applyTabTitle(rawTitle, toTabAtIndex: idx, persist: persist)
    }

    @discardableResult
    func updateResetTabNameOnAppStart(tabId: UInt64, enabled: Bool, persist: Bool = true) -> Bool {
        guard let idx = tabs.firstIndex(where: { $0.id == tabId }) else {
            return false
        }
        if persist {
            Self.persistResetTabTitleOnAppStart(enabled, forProfileID: tabs[idx].appearanceProfileID)
        }
        return true
    }

    func startupCommand(tabId: UInt64) -> String? {
        tabs.first(where: { $0.id == tabId })?.startupCommand
    }

    @discardableResult
    func updateStartupCommand(tabId: UInt64, rawCommand: String, persist: Bool = true) -> Bool {
        guard let idx = tabs.firstIndex(where: { $0.id == tabId }) else {
            return false
        }
        return applyStartupCommand(rawCommand, toTabAtIndex: idx, persist: persist)
    }

    private func bindSelectedAppearanceSettings() {
        let settings = selectedTab.appearanceSettings
        selectedAppearanceObserver = Publishers.CombineLatest4(
            settings.$pointSize,
            settings.$selectedFontID,
            settings.$backgroundColor,
            settings.$foregroundColor
        )
        .sink { pointSize, fontID, background, foreground in
            TerminalFontSettings.shared.applyTransientAppearance(
                pointSize: pointSize,
                fontIdentifier: fontID,
                backgroundColor: background,
                foregroundColor: foreground
            )
        }
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
            if manager.isSdlTabSelected {
                return
            }
            NotificationCenter.default.post(name: .terminalInputFocusRequested, object: nil)
        }
    }

    @ViewBuilder
    private func tabContent(for tab: TerminalTabManager.Tab, isSelected: Bool) -> some View {
        switch tab.kind {
        case .shell(let runtime):
            TerminalView(showsOverlay: true,
                         isActive: isSelected,
                         runtime: runtime,
                         tabId: tab.id,
                         appearanceSettings: tab.appearanceSettings)
        case .shellSession(let session):
            ShellTerminalView(session: session,
                              isActive: isSelected,
                              tabId: tab.id,
                              appearanceSettings: tab.appearanceSettings)
        case .ssh(let session):
            SshTerminalView(session: session,
                            isActive: isSelected,
                            tabId: tab.id,
                            appearanceSettings: tab.appearanceSettings)
        case .sdl:
            SdlTabView {
                TerminalTabManager.shared.selectFirstNonSdlTab()
            } onPromote: {
                TerminalTabManager.shared.promoteSdlToFront()
            }
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

private struct SdlTabView: View {
    let onReturn: () -> Void
    let onPromote: () -> Void

    var body: some View {
        VStack(spacing: 12) {
            Text("SDL tab active")
                .font(.system(size: 17, weight: .semibold))
                .foregroundColor(.primary)
            Text("This tab controls SDL focus. If SDL is behind, tap below.")
                .font(.system(size: 13))
                .foregroundColor(.secondary)
            Button(action: onPromote) {
                Text("Bring SDL to Front")
                    .font(.system(size: 14, weight: .semibold))
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
            }
            .buttonStyle(.borderedProminent)
            Button(action: onReturn) {
                Text("Return to Shell")
                    .font(.system(size: 14, weight: .semibold))
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
            }
            .buttonStyle(.borderedProminent)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color(.systemBackground))
        .onAppear {
            onPromote()
        }
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

@_cdecl("pscalRuntimeSetTabStartupCommandForSession")
func pscalRuntimeSetTabStartupCommandForSession(_ sessionId: UInt64, _ commandPtr: UnsafePointer<CChar>?) -> Int32 {
    let raw = commandPtr.map { String(cString: $0) } ?? ""
    if let success = runOnMainBlocking("setTabStartupCommand", work: {
        TerminalTabManager.shared.updateStartupCommand(forSessionId: sessionId, rawCommand: raw)
    }), success {
        return 0
    }
    return -EINVAL
}

@_cdecl("pscalRuntimePromptReadyForSession")
func pscalRuntimePromptReadyForSession(_ sessionId: UInt64) -> Int32 {
    Task { @MainActor in
        TerminalTabManager.shared.handlePromptReady(sessionId: sessionId)
    }
    return 0
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

@_cdecl("pscalRuntimeSdlDidOpen")
func pscalRuntimeSdlDidOpen() {
    Task { @MainActor in
        TerminalTabManager.shared.handleSdlDidOpen()
    }
}

@_cdecl("pscalRuntimeSdlDidClose")
func pscalRuntimeSdlDidClose() {
    Task { @MainActor in
        TerminalTabManager.shared.handleSdlDidClose()
    }
}
