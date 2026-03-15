import SwiftUI
import Foundation
import UIKit
import Darwin
import Combine
import UniformTypeIdentifiers

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
            case ssh(ShellRuntimeSession)
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
    private var pendingTitlesBySessionId: [UInt64: String] = [:]
    private var pendingStartupCommandsBySessionId: [UInt64: String] = [:]
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
        let sessionId = PSCALRuntimeNextSessionId()
        let session = ShellRuntimeSession(sessionId: sessionId, argv: argv, program: .ssh)
        let defaultTitle = TerminalTabManager.sanitizeTitle(sshTitle(argv: argv))
        let profileID = "ssh.1"
        let title = Self.startupTabTitle(
            forProfileID: profileID,
            fallback: defaultTitle,
            restoreProfileDefaults: true
        )
        let appearance = appearanceSettings(forProfileID: profileID)
        let tab = Tab(id: sessionId,
                      title: title,
                      startupCommand: "",
                      sessionId: sessionId,
                      appearanceProfileID: profileID,
                      appearanceSettings: appearance,
                      kind: .ssh(session))
        let previousSelectedId = selectedId
        tabs.append(tab)
        selectedId = sessionId
        guard session.start() else {
            let err = session.lastStartErrno
            if let idx = tabs.firstIndex(where: { $0.id == sessionId }) {
                _ = removeTab(at: idx)
            } else {
                selectedId = previousSelectedId
            }
            tabInitLog("openSshSession start failed errno=\(err)")
            return -(err == 0 ? EIO : err)
        }
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
        pendingTitlesBySessionId.removeValue(forKey: sessionId)
        pendingStartupCommandsBySessionId.removeValue(forKey: sessionId)
        switch tabs[index].kind {
        case .shell:
            return
        case .shellSession(let session):
            session.markExited(status: status)
        case .ssh(let session):
            session.markExited(status: status)
            return
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
            pendingTitlesBySessionId.removeValue(forKey: sessionId)
            pendingStartupCommandsBySessionId.removeValue(forKey: sessionId)
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
            session.sendInterrupt()
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
            session.sendSuspend()
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
        applyPendingSessionMetadata(sessionId: sessionId, tabIndex: idx)
        applyStartupCommandIfReady(forSessionId: sessionId)
    }

    func handlePromptReady(sessionId: UInt64) {
        guard sessionId != 0 else { return }
        promptReadySessions.insert(sessionId)
        applyStartupCommandIfReady(forSessionId: sessionId)
        autoRestoreFromStaleSdlIfNeeded(promptReadySessionId: sessionId)
    }

    private func autoRestoreFromStaleSdlIfNeeded(promptReadySessionId sessionId: UInt64) {
        guard let sdlIndex = tabs.firstIndex(where: { tab in
            if case .sdl = tab.kind {
                return true
            }
            return false
        }) else {
            return
        }

        guard case .sdl(let ownerTabId) = tabs[sdlIndex].kind else {
            return
        }
        guard let ownerIndex = tabs.firstIndex(where: { $0.id == ownerTabId }) else {
            return
        }
        guard let ownerSessionId = tabs[ownerIndex].sessionId, ownerSessionId == sessionId else {
            return
        }

        tabInitLog("autoRestoreFromStaleSdl ownerSessionId=\(sessionId) sdlTab=\(tabs[sdlIndex].id)")
        handleSdlDidClose()
    }

    fileprivate func updateTitle(forSessionId sessionId: UInt64, rawTitle: String) -> Bool {
        guard sessionId != 0 else { return false }
        guard let idx = tabs.firstIndex(where: { $0.sessionId == sessionId }) else {
            pendingTitlesBySessionId[sessionId] = rawTitle
            return true
        }
        return applyTabTitle(rawTitle, toTabAtIndex: idx, persist: true)
    }

    fileprivate func updateStartupCommand(forSessionId sessionId: UInt64, rawCommand: String) -> Bool {
        guard sessionId != 0 else { return false }
        guard let idx = tabs.firstIndex(where: { $0.sessionId == sessionId }) else {
            pendingStartupCommandsBySessionId[sessionId] = rawCommand
            return true
        }
        return applyStartupCommand(rawCommand, toTabAtIndex: idx, persist: false)
    }

    private func applyPendingSessionMetadata(sessionId: UInt64, tabIndex: Int) {
        if let pendingTitle = pendingTitlesBySessionId.removeValue(forKey: sessionId) {
            _ = applyTabTitle(pendingTitle, toTabAtIndex: tabIndex, persist: true)
        }
        if let pendingStartup = pendingStartupCommandsBySessionId.removeValue(forKey: sessionId) {
            _ = applyStartupCommand(pendingStartup, toTabAtIndex: tabIndex, persist: false)
        }
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
        if persist && tabKindSupportsPersistentStartupCommand(at: index) {
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
        case .ssh:
            break
        case .sdl:
            break
        }
    }

    private func tabKindSupportsPersistentStartupCommand(at index: Int) -> Bool {
        guard tabs.indices.contains(index) else { return false }
        switch tabs[index].kind {
        case .shell, .shellSession:
            return true
        case .ssh, .sdl:
            return false
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
            session.sendInterrupt()
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
            session.sendSuspend()
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
            ShellTerminalView(session: session,
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
                .accessibilityHint("Opens a new local shell session in a new tab")
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
        .accessibilityLabel(title)
        .accessibilityAddTraits(isSelected ? [.isSelected, .isButton] : [.isButton])
        .accessibilityHint(isSelected ? "Selected tab" : "Selects this tab")
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
            .accessibilityLabel("Bring SDL to Front")
            .accessibilityHint("Brings the SDL window to the foreground if it is hidden behind the terminal")

            Button(action: onReturn) {
                Text("Return to Shell")
                    .font(.system(size: 14, weight: .semibold))
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
            }
            .buttonStyle(.borderedProminent)
            .accessibilityLabel("Return to Shell")
            .accessibilityHint("Closes the SDL tab and returns you to a shell prompt")
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

@MainActor
private final class RuntimeMountFolderPicker: NSObject, UIDocumentPickerDelegate, UIAdaptivePresentationControllerDelegate {
    static let shared = RuntimeMountFolderPicker()
    private static let bookmarkDefaultsKey = "com.pscal.mount.securityScopedBookmarks.v1"
    private static let bookmarkCreationOptions: URL.BookmarkCreationOptions = {
#if targetEnvironment(macCatalyst)
        return [.withSecurityScope]
#else
        return []
#endif
    }()
    private static let bookmarkResolutionOptions: URL.BookmarkResolutionOptions = {
#if targetEnvironment(macCatalyst)
        return [.withSecurityScope]
#else
        return []
#endif
    }()

    private var completion: ((String?, Int32) -> Void)?
    private var securityScopedURLs: [String: URL] = [:]
    private var bookmarkDataByPath: [String: Data] = [:]
    private var bookmarksLoaded = false

    private static func normalizePath(_ path: String) -> String {
        var normalized = (path as NSString).standardizingPath
        while normalized.count > 1 && normalized.hasSuffix("/") {
            normalized.removeLast()
        }
        return normalized
    }

    private func persistBookmarks() {
        UserDefaults.standard.set(bookmarkDataByPath, forKey: Self.bookmarkDefaultsKey)
    }

    private func loadPersistedBookmarksIfNeeded() {
        guard !bookmarksLoaded else {
            return
        }
        bookmarksLoaded = true
        if let stored = UserDefaults.standard.object(forKey: Self.bookmarkDefaultsKey) as? [String: Data] {
            bookmarkDataByPath = stored
        } else if let dict = UserDefaults.standard.dictionary(forKey: Self.bookmarkDefaultsKey) {
            var recovered: [String: Data] = [:]
            for (key, value) in dict {
                if let data = value as? Data {
                    recovered[key] = data
                }
            }
            bookmarkDataByPath = recovered
        } else {
            bookmarkDataByPath = [:]
        }
        restoreSecurityScopedURLsFromBookmarks()
    }

    private func restoreSecurityScopedURLsFromBookmarks() {
        guard !bookmarkDataByPath.isEmpty else {
            return
        }
        var updatedURLs: [String: URL] = [:]
        var updatedBookmarks: [String: Data] = [:]
        for (_, data) in bookmarkDataByPath {
            var stale = false
            guard let url = try? URL(resolvingBookmarkData: data,
                                     options: Self.bookmarkResolutionOptions,
                                     relativeTo: nil,
                                     bookmarkDataIsStale: &stale) else {
                continue
            }
            let normalized = Self.normalizePath(url.path)
            guard url.startAccessingSecurityScopedResource() else {
                continue
            }
            if let existing = updatedURLs[normalized], existing != url {
                existing.stopAccessingSecurityScopedResource()
            }
            updatedURLs[normalized] = url
            if stale,
               let refreshed = try? url.bookmarkData(options: Self.bookmarkCreationOptions,
                                                     includingResourceValuesForKeys: nil,
                                                     relativeTo: nil) {
                updatedBookmarks[normalized] = refreshed
            } else {
                updatedBookmarks[normalized] = data
            }
        }
        for (path, url) in securityScopedURLs where updatedURLs[path] == nil {
            url.stopAccessingSecurityScopedResource()
        }
        let changed = updatedBookmarks != bookmarkDataByPath
        securityScopedURLs = updatedURLs
        bookmarkDataByPath = updatedBookmarks
        if changed {
            persistBookmarks()
        }
    }

    private func registerSecurityScopedURL(_ selectedURL: URL) -> Bool {
        let normalized = Self.normalizePath(selectedURL.path)
        guard selectedURL.startAccessingSecurityScopedResource() else {
            return false
        }
        if let existing = securityScopedURLs[normalized], existing != selectedURL {
            existing.stopAccessingSecurityScopedResource()
        }
        securityScopedURLs[normalized] = selectedURL
        if let bookmark = try? selectedURL.bookmarkData(options: Self.bookmarkCreationOptions,
                                                        includingResourceValuesForKeys: nil,
                                                        relativeTo: nil) {
            bookmarkDataByPath[normalized] = bookmark
            persistBookmarks()
        }
        return true
    }

    private func hasActiveSecurityScope(for normalizedPath: String) -> Bool {
        if securityScopedURLs[normalizedPath] != nil {
            return true
        }
        for root in securityScopedURLs.keys {
            if normalizedPath == root || normalizedPath.hasPrefix(root + "/") {
                return true
            }
        }
        return false
    }

    func ensureSecurityScopedAccess(forPath path: String) -> Bool {
        let normalizedPath = Self.normalizePath(path)
        loadPersistedBookmarksIfNeeded()
        if hasActiveSecurityScope(for: normalizedPath) {
            return true
        }

        // Try to reactivate a bookmark that is exactly this path or the
        // nearest parent directory bookmark.
        var bestMatch: String?
        for key in bookmarkDataByPath.keys {
            if normalizedPath == key || normalizedPath.hasPrefix(key + "/") {
                if bestMatch == nil || key.count > bestMatch!.count {
                    bestMatch = key
                }
            }
        }
        if let key = bestMatch, let data = bookmarkDataByPath[key] {
            var stale = false
            if let url = try? URL(resolvingBookmarkData: data,
                                  options: Self.bookmarkResolutionOptions,
                                  relativeTo: nil,
                                  bookmarkDataIsStale: &stale),
               registerSecurityScopedURL(url) {
                if stale {
                    restoreSecurityScopedURLsFromBookmarks()
                }
                return hasActiveSecurityScope(for: normalizedPath)
            }
            bookmarkDataByPath.removeValue(forKey: key)
            persistBookmarks()
        }
        return FileManager.default.isReadableFile(atPath: normalizedPath)
    }

    func present(completion: @escaping (String?, Int32) -> Void) {
        guard self.completion == nil else {
            completion(nil, Int32(EBUSY))
            return
        }
        guard let presenter = Self.topPresentingViewController() else {
            completion(nil, Int32(ENODEV))
            return
        }

        self.completion = completion

        let picker: UIDocumentPickerViewController
        if #available(iOS 14.0, *) {
            picker = UIDocumentPickerViewController(forOpeningContentTypes: [.folder], asCopy: false)
        } else {
            picker = UIDocumentPickerViewController(documentTypes: ["public.folder"], in: .open)
        }
        picker.delegate = self
        picker.allowsMultipleSelection = false
        picker.presentationController?.delegate = self
        if let popover = picker.popoverPresentationController {
            popover.sourceView = presenter.view
            popover.sourceRect = presenter.view.bounds
        }
        presenter.present(picker, animated: true)
    }

    func documentPicker(_ controller: UIDocumentPickerViewController,
                        didPickDocumentsAt urls: [URL]) {
        guard let selectedURL = urls.first else {
            finish(path: nil, err: Int32(ECANCELED))
            return
        }
        loadPersistedBookmarksIfNeeded()
        let normalizedPath = Self.normalizePath(selectedURL.path)
        let started = registerSecurityScopedURL(selectedURL)
        if started || FileManager.default.isReadableFile(atPath: normalizedPath) {
            finish(path: normalizedPath, err: 0)
            return
        }
        finish(path: nil, err: Int32(EPERM))
    }

    func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
        finish(path: nil, err: Int32(ECANCELED))
    }

    func presentationControllerDidDismiss(_ presentationController: UIPresentationController) {
        finish(path: nil, err: Int32(ECANCELED))
    }

    private func finish(path: String?, err: Int32) {
        guard let completion else {
            return
        }
        self.completion = nil
        completion(path, err)
    }

    private static func topPresentingViewController() -> UIViewController? {
        let scenes = UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .filter { $0.activationState == .foregroundActive || $0.activationState == .foregroundInactive }
        let windows = scenes.flatMap { $0.windows }
        let preferredWindow = windows.first(where: { $0.isKeyWindow }) ??
            windows.first(where: { !$0.isHidden && $0.alpha > 0.01 })
        guard let root = preferredWindow?.rootViewController else {
            return nil
        }
        return topViewController(from: root)
    }

    private static func topViewController(from root: UIViewController) -> UIViewController {
        if let nav = root as? UINavigationController, let visible = nav.visibleViewController {
            return topViewController(from: visible)
        }
        if let tab = root as? UITabBarController, let selected = tab.selectedViewController {
            return topViewController(from: selected)
        }
        if let presented = root.presentedViewController {
            return topViewController(from: presented)
        }
        return root
    }
}

@_cdecl("pscalRuntimePickMountSourceDirectory")
func pscalRuntimePickMountSourceDirectory() -> UnsafeMutablePointer<CChar>? {
    if Thread.isMainThread {
        errno = EDEADLK
        return nil
    }

    let semaphore = DispatchSemaphore(value: 0)
    var selectedPath: String?
    var pickErrno: Int32 = Int32(EAGAIN)

    DispatchQueue.main.async {
        RuntimeMountFolderPicker.shared.present { path, err in
            selectedPath = path
            pickErrno = err
            semaphore.signal()
        }
    }
    semaphore.wait()

    guard let selectedPath else {
        errno = pickErrno
        return nil
    }
    return strdup(selectedPath)
}

@_cdecl("pscalRuntimeEnsureMountSourceAccess")
func pscalRuntimeEnsureMountSourceAccess(_ path: UnsafePointer<CChar>?) -> Int32 {
    guard let path else {
        errno = EINVAL
        return -1
    }
    let sourcePath = String(cString: path)
    if sourcePath.isEmpty {
        errno = EINVAL
        return -1
    }
    if let ok = runOnMainBlocking("ensureMountSourceAccess", work: {
        RuntimeMountFolderPicker.shared.ensureSecurityScopedAccess(forPath: sourcePath)
    }), ok {
        return 0
    }
    errno = EACCES
    return -1
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
