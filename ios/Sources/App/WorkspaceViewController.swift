// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceViewController.m at
// 0f75fee1c) and licensed to PSCAL under the Apache License 2.0.

import Combine
import SwiftUI
import UIKit

/// While Workspace is on screen it decides what the window-level keys do. See
/// `TerminalWindow.sendEvent`.
@MainActor
protocol TerminalKeyRouting: AnyObject {
    func handleWindowPresses(_ event: UIPressesEvent) -> Bool
    /// False while nothing that takes Ctrl-C holds the keyboard, such as the bare desktop.
    var terminalControlKeysGoToSelected: Bool { get }
}

enum WorkspaceSettings {
    private static let openAtLaunchKey = "com.pscal.workspace.openAtLaunch"
    static let savedDesktopsKey = "com.pscal.workspace.savedDesktops"

    static var opensAtLaunch: Bool {
        get { UserDefaults.standard.bool(forKey: openAtLaunchKey) }
        set { UserDefaults.standard.set(newValue, forKey: openAtLaunchKey) }
    }
}

/// A desktop of movable windows inside the one iOS window, which is why it works on
/// iPhone: it doesn't depend on scenes or Stage Manager.
///
/// Sessions belong to `TerminalTabManager`. Workspace is another way of showing them, so
/// leaving it and coming back ends nothing. `selectedId` stays "the session with the
/// keyboard".
@MainActor
final class WorkspaceViewController: UIViewController, UIGestureRecognizerDelegate, TerminalKeyRouting {
    /// What was on screen when Workspace was last left, so coming back puts it back.
    private static var arrangementAtLeave: [[String: Any]]?
    private static var didApplySavedArrangement = false

    private let manager = TerminalTabManager.shared
    private let desktopSurfaceView = UIView()
    private let menuPip = UIButton(type: .system)
    private let desktopToast = UILabel()
    private var toastHide: DispatchWorkItem?

    private(set) var desktopWindows: [WorkspaceWindowView] = []
    private var contentByWindow: [Int: WorkspaceTerminalTabsViewController] = [:]
    private var appletByWindow: [Int: WorkspaceAppletViewController] = [:]
    private var cascadeIndex = 0
    private weak var focusedWindow: WorkspaceWindowView?
    private var desktopHasFocus = false

    private(set) var activeDesktopIndex = 0
    private(set) var desktopCount = 1
    private var lockedDesktops = Set<Int>()
    private var desktopNames: [String] = []
    private var savedSignature: String?

    private var managerObserver: AnyCancellable?
    private var syncScheduled = false
    private var didInitialSync = false
    private var lastSeenSelectedId: UInt64 = 0
    /// Where a new session goes when it was asked for as a tab of a window.
    private weak var pendingTabWindow: WorkspaceWindowView?

    private var keyboardObservers: [NSObjectProtocol] = []
    private var keyboardFrameInScreen: CGRect?
    private weak var raisedWindow: WorkspaceWindowView?
    private var frameBeforeKeyboardRaise: CGRect = .null

    /// Asks the root to go back to the tab bar.
    var leaveHandler: (() -> Void)?

    // MARK: - Lifecycle

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = WorkspaceTheme.desktopBackground

        desktopSurfaceView.translatesAutoresizingMaskIntoConstraints = false
        desktopSurfaceView.backgroundColor = WorkspaceTheme.desktopBackground
        desktopSurfaceView.accessibilityIdentifier = "workspace.desktop"
        view.addSubview(desktopSurfaceView)

        // The root menu fires on the bare desktop only, never on a window.
        let longPress = UILongPressGestureRecognizer(target: self, action: #selector(handleDesktopLongPress(_:)))
        longPress.minimumPressDuration = 0.4
        longPress.delegate = self
        desktopSurfaceView.addGestureRecognizer(longPress)

        // A tap on the bare desktop gives the Workspace the keyboard, so nothing else
        // holds it. The on-screen keyboard goes away with it.
        let tap = UITapGestureRecognizer(target: self, action: #selector(handleDesktopTap(_:)))
        tap.cancelsTouchesInView = false
        tap.delegate = self
        desktopSurfaceView.addGestureRecognizer(tap)

        // Two fingers work anywhere, including on top of a window, so the menu is always
        // reachable even when windows cover the whole desktop.
        let twoFinger = UILongPressGestureRecognizer(target: self, action: #selector(handleTwoFingerLongPress(_:)))
        twoFinger.minimumPressDuration = 0.4
        twoFinger.numberOfTouchesRequired = 2
        view.addGestureRecognizer(twoFinger)

        // A two-finger horizontal swipe moves between Desktops.
        for direction: UISwipeGestureRecognizer.Direction in [.left, .right] {
            let swipe = UISwipeGestureRecognizer(target: self, action: #selector(handleDesktopSwipe(_:)))
            swipe.direction = direction
            swipe.numberOfTouchesRequired = 2
            view.addGestureRecognizer(swipe)
        }

        buildMenuPip()
        buildToast()
        NSLayoutConstraint.activate([
            desktopSurfaceView.topAnchor.constraint(equalTo: view.topAnchor),
            desktopSurfaceView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            desktopSurfaceView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            desktopSurfaceView.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])
        installKeyboardObservers()
        lastSeenSelectedId = manager.selectedId
        managerObserver = manager.objectWillChange.sink { [weak self] _ in
            self?.scheduleSync()
        }
    }

    deinit {
        keyboardObservers.forEach { NotificationCenter.default.removeObserver($0) }
    }

    private func buildMenuPip() {
        menuPip.translatesAutoresizingMaskIntoConstraints = false
        menuPip.setImage(UIImage(systemName: "line.3.horizontal"), for: .normal)
        menuPip.tintColor = WorkspaceTheme.ink(on: WorkspaceTheme.accent)
        menuPip.backgroundColor = WorkspaceTheme.accent
        menuPip.layer.cornerRadius = 22
        menuPip.layer.borderWidth = 1.5
        menuPip.layer.borderColor = UIColor(white: 1, alpha: 0.85).cgColor
        menuPip.layer.shadowColor = UIColor.black.cgColor
        menuPip.layer.shadowOpacity = 0.35
        menuPip.layer.shadowRadius = 6
        menuPip.layer.shadowOffset = CGSize(width: 0, height: 2)
        menuPip.accessibilityLabel = "Workspace menu"
        menuPip.accessibilityHint = "Opens the desktop menu for window management"
        menuPip.accessibilityIdentifier = "workspace.menu"
        menuPip.addTarget(self, action: #selector(menuPipTapped(_:)), for: .touchUpInside)
        view.addSubview(menuPip)
        NSLayoutConstraint.activate([
            menuPip.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor, constant: -16),
            menuPip.bottomAnchor.constraint(equalTo: view.keyboardLayoutGuide.topAnchor, constant: -16),
            menuPip.widthAnchor.constraint(equalToConstant: 44),
            menuPip.heightAnchor.constraint(equalToConstant: 44),
        ])
    }

    private func buildToast() {
        desktopToast.translatesAutoresizingMaskIntoConstraints = false
        desktopToast.font = .systemFont(ofSize: 15, weight: .semibold)
        desktopToast.textColor = .white
        desktopToast.backgroundColor = UIColor(white: 0, alpha: 0.72)
        desktopToast.textAlignment = .center
        desktopToast.layer.cornerRadius = 10
        desktopToast.layer.masksToBounds = true
        desktopToast.alpha = 0
        desktopToast.isUserInteractionEnabled = false
        view.addSubview(desktopToast)
        NSLayoutConstraint.activate([
            desktopToast.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            desktopToast.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor, constant: 12),
            desktopToast.heightAnchor.constraint(equalToConstant: 32),
        ])
    }

    private func showDesktopToast() {
        let name = nameForDesktop(activeDesktopIndex)
        desktopToast.text = "  \(name)  (\(activeDesktopIndex + 1) / \(desktopCount))  "
        toastHide?.cancel()
        desktopToast.alpha = 1
        let hide = DispatchWorkItem { [weak self] in
            UIView.animate(withDuration: 0.25) {
                self?.desktopToast.alpha = 0
            }
        }
        toastHide = hide
        DispatchQueue.main.asyncAfter(deadline: .now() + 1, execute: hide)
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        guard desktopSurfaceView.bounds.width > 0, desktopSurfaceView.bounds.height > 0 else { return }
        if !didInitialSync {
            didInitialSync = true
            // Next turn: the tab bar's views are still leaving, and a session can only be
            // attached to one terminal view. Detaching after attaching would stop its output.
            DispatchQueue.main.async { [weak self] in
                self?.firstSync()
            }
        }
        for window in desktopWindows {
            applyInitialFrameIfNeeded(window)
            clampToVisibleBounds(window)
        }
    }

    private func firstSync() {
        syncFromManager()
        if let arrangement = Self.arrangementAtLeave {
            Self.arrangementAtLeave = nil
            applyArrangement(arrangement)
        } else if !Self.didApplySavedArrangement {
            Self.didApplySavedArrangement = true
            if let saved = savedArrangement() {
                applyArrangement(saved)
            }
        }
    }

    override var canBecomeFirstResponder: Bool { true }

    /// Takes every window down without touching a session, on the way back to tabs.
    func tearDownWindows() {
        Self.arrangementAtLeave = arrangementDescriptors()
        managerObserver = nil
        for window in desktopWindows {
            if let content = contentByWindow[window.serial] {
                content.setWindowState(visible: false, focused: false)
                content.hostedTabs.map(\.tabID).forEach { content.removeTab(tabID: $0) }
                content.willMove(toParent: nil)
                content.view.removeFromSuperview()
                content.removeFromParent()
            }
            if let applet = appletByWindow[window.serial] {
                applet.willMove(toParent: nil)
                applet.view.removeFromSuperview()
                applet.removeFromParent()
            }
            window.removeFromSuperview()
        }
        desktopWindows.removeAll()
        contentByWindow.removeAll()
        appletByWindow.removeAll()
        focusedWindow = nil
    }

    // MARK: - Sessions

    private func scheduleSync() {
        guard didInitialSync, !syncScheduled else { return }
        syncScheduled = true
        // objectWillChange fires before the change, so read the manager next turn.
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.syncScheduled = false
            self.syncFromManager()
        }
    }

    /// Makes the windows match the manager's sessions. Idempotent.
    func syncFromManager() {
        let tabs = manager.tabs
        let liveIDs = Set(tabs.map(\.id))
        let focusedBefore = focusedWindow
        var removedAny = false

        for window in desktopWindows {
            guard let content = contentByWindow[window.serial] else { continue }
            for hosted in content.hostedTabs where !liveIDs.contains(hosted.tabID) {
                content.removeTab(tabID: hosted.tabID)
                removedAny = true
            }
        }
        for window in desktopWindows where contentByWindow[window.serial]?.hostedTabs.isEmpty == true {
            removeWindow(window)
        }

        var addedAny = false
        for tab in tabs where windowHosting(tabID: tab.id) == nil {
            if let target = pendingTabWindow, let content = contentByWindow[target.serial] {
                content.addTab(tab, select: true)
            } else {
                _ = openWindow(for: tab)
            }
            addedAny = true
        }
        pendingTabWindow = nil

        if removedAny && !addedAny {
            // The manager picks the first shell when a selected tab goes. Stay in the window
            // the person was working in.
            if let window = focusedBefore, desktopWindows.contains(where: { $0 === window }) {
                focus(window: window)
            } else if let top = frontmostWindow() {
                focus(window: top)
            } else {
                focusDesktop()
            }
        } else if addedAny || manager.selectedId != lastSeenSelectedId {
            followManagerSelection()
        }
        lastSeenSelectedId = manager.selectedId
        refreshTitles()
        applyStates()
        postDesktopsDidChange()
    }

    private func followManagerSelection() {
        guard let window = windowHosting(tabID: manager.selectedId) else { return }
        contentByWindow[window.serial]?.select(tabID: manager.selectedId)
        if focusedWindow !== window || desktopHasFocus {
            focus(window: window)
        }
    }

    private func refreshTitles() {
        for window in desktopWindows {
            guard let content = contentByWindow[window.serial], let tabID = content.selectedTabID else { continue }
            window.titleLabel.text = manager.tabTitle(tabId: tabID) ?? "Terminal"
            // The root session is the app's own: it has no close affordance, as its tab has
            // none, and neither does a window holding it.
            let holdsRoot = content.hostedTabs.contains { manager.isRootTab($0.tabID) }
            window.closeButton.isHidden = holdsRoot
        }
    }

    func windowHosting(tabID: UInt64) -> WorkspaceWindowView? {
        desktopWindows.first { contentByWindow[$0.serial]?.contains(tabID: tabID) == true }
    }

    func content(of window: WorkspaceWindowView) -> WorkspaceTerminalTabsViewController? {
        contentByWindow[window.serial]
    }

    func applet(of window: WorkspaceWindowView) -> WorkspaceAppletViewController? {
        appletByWindow[window.serial]
    }

    // MARK: - Windows

    private func desktopUsableBounds() -> CGRect {
        var insets = view.safeAreaInsets
        // In an iPadOS 26 window the system's close, minimize and zoom controls cover the
        // desktop's top-leading corner, well below the top safe area a window reports.
        insets.top = max(insets.top, workspaceWindowingControlsTopInset(view))
        let bounds = desktopSurfaceView.bounds
        if WorkspaceMetrics.usesPhoneLayout {
            return bounds.inset(by: insets)
        }
        return bounds.inset(by: UIEdgeInsets(top: insets.top, left: 0, bottom: 0, right: 0))
    }

    private func frameForNewWindow(preferredSize: CGSize) -> CGRect {
        let usable = desktopUsableBounds()
        let phone = WorkspaceMetrics.usesPhoneLayout
        let width = min(preferredSize.width, usable.width)
        let height = min(preferredSize.height, usable.height)
        let step: CGFloat = phone ? 14 : 28
        let count = phone ? 4 : 6
        let verticalFactor: CGFloat = phone ? 0.08 : 0.16
        let offset = CGFloat(cascadeIndex % count) * step
        var originX = usable.minX + max(0, (usable.width - width) * 0.5) + offset
        var originY = usable.minY + max(0, (usable.height - height) * verticalFactor) + offset
        originX = min(originX, usable.maxX - width)
        originY = min(originY, usable.maxY - height)
        cascadeIndex += 1
        return CGRect(x: originX, y: originY, width: width, height: height).integral
    }

    private func applyInitialFrameIfNeeded(_ window: WorkspaceWindowView) {
        guard !window.didApplyInitialFrame, desktopSurfaceView.bounds.width > 0, desktopSurfaceView.bounds.height > 0 else {
            return
        }
        window.frame = frameForNewWindow(preferredSize: window.preferredSize)
        window.didApplyInitialFrame = true
    }

    private func clampedFrame(_ input: CGRect, for window: WorkspaceWindowView) -> CGRect {
        var frame = input
        let minSize = window.minimumSize
        let maxSize = window.maximumSize
        if minSize.width > 0 { frame.size.width = max(frame.width, minSize.width) }
        if minSize.height > 0 { frame.size.height = max(frame.height, minSize.height) }
        if maxSize.width > 0 { frame.size.width = min(frame.width, maxSize.width) }
        if maxSize.height > 0 { frame.size.height = min(frame.height, maxSize.height) }
        // Before the surface has been laid out its bounds are empty, and clamping against
        // them would collapse the window to nothing.
        let usable = desktopUsableBounds()
        guard usable.width > 0, usable.height > 0 else { return frame }
        let surface = desktopSurfaceView.bounds
        // On an iPad the size is limited by the whole surface, as the position is, so a
        // window can sit against any edge and also reach the opposite one. The phone keeps
        // the usable bounds: there the insets are the notch and home indicator on every side.
        let sizeBounds = WorkspaceMetrics.usesPhoneLayout ? usable : surface
        frame.size.width = min(frame.width, sizeBounds.width)
        frame.size.height = min(frame.height, sizeBounds.height)

        let visibleWidth = min(frame.width, 140)
        let minX = surface.minX - (frame.width - visibleWidth)
        let maxX = max(minX, surface.maxX - visibleWidth)
        let visibleHeight = min(frame.height, WorkspaceMetrics.titleBarHeight)
        let minY = surface.minY
        let maxY = max(minY, surface.maxY - visibleHeight)
        frame.origin.x = min(max(frame.minX, minX), maxX).rounded()
        frame.origin.y = min(max(frame.minY, minY), maxY).rounded()
        return frame
    }

    private func clampToVisibleBounds(_ window: WorkspaceWindowView) {
        guard !window.bounds.isEmpty else { return }
        // If a previous pass shrank or moved this window for a smaller surface, try the
        // remembered frame first, so it pops back the moment the surface fits it. Otherwise
        // show the clamped form of the remembered frame, so repeated passes can't ratchet
        // the window down.
        if !window.frameBeforeBoundsClamp.isNull {
            let remembered = window.frameBeforeBoundsClamp
            let restored = clampedFrame(remembered, for: window)
            window.frame = restored
            if restored == remembered {
                window.frameBeforeBoundsClamp = .null
            }
            return
        }
        let clamped = clampedFrame(window.frame, for: window)
        if clamped != window.frame {
            window.frameBeforeBoundsClamp = window.frame
        }
        window.frame = clamped
    }

    private func defaultTerminalWindowSize() -> CGSize {
        // Scaled to orientation so terminals don't dominate: in landscape half the width by
        // half the height, in portrait the full width by a quarter of the height.
        let usable = desktopUsableBounds()
        guard usable.width > 1, usable.height > 1 else { return WorkspaceMetrics.preferredTerminalSize }
        return usable.width >= usable.height
            ? CGSize(width: usable.width / 2, height: usable.height / 2)
            : CGSize(width: usable.width, height: usable.height / 4)
    }

    private func installCommonHandlers(on window: WorkspaceWindowView) {
        window.closeHandler = { [weak self, weak window] in
            guard let self, let window else { return }
            self.closeWindow(window)
        }
        window.didBecomeFrontmostHandler = { [weak self, weak window] in
            guard let self, let window else { return }
            self.focus(window: window)
        }
        window.frameDidChangeHandler = { [weak self, weak window] in
            guard let self, let window else { return }
            if self.raisedWindow === window {
                // The person moved it, so it stays where they put it when the keyboard goes.
                self.raisedWindow = nil
                self.frameBeforeKeyboardRaise = .null
            }
            self.applyKeyboardToFocusedWindow()
        }
        window.setUtilityButton(title: "☰") { [weak self, weak window] in
            guard let self, let window else { return }
            self.presentRootMenu(from: window.utilityButton, sourceRect: window.utilityButton.bounds)
        }
    }

    private func embed(_ child: UIViewController, in window: WorkspaceWindowView) {
        addChild(child)
        child.view.translatesAutoresizingMaskIntoConstraints = false
        window.contentContainerView.addSubview(child.view)
        NSLayoutConstraint.activate([
            child.view.topAnchor.constraint(equalTo: window.contentContainerView.topAnchor),
            child.view.leadingAnchor.constraint(equalTo: window.contentContainerView.leadingAnchor),
            child.view.trailingAnchor.constraint(equalTo: window.contentContainerView.trailingAnchor),
            child.view.bottomAnchor.constraint(equalTo: window.contentContainerView.bottomAnchor),
        ])
        child.didMove(toParent: self)
    }

    @discardableResult
    private func openWindow(for tab: TerminalTabManager.Tab) -> WorkspaceWindowView {
        let window = WorkspaceWindowView(title: tab.title, showsCloseButton: true)
        window.preferredSize = defaultTerminalWindowSize()
        window.resizable = true
        window.titleBarDoubleTapZoomEnabled = true
        window.minimumSize = WorkspaceMetrics.minimumTerminalSize
        window.frame = CGRect(origin: .zero, size: window.preferredSize)
        window.desktopIndex = activeDesktopIndex
        desktopSurfaceView.addSubview(window)
        desktopWindows.append(window)

        let content = WorkspaceTerminalTabsViewController()
        content.windowView = window
        contentByWindow[window.serial] = content
        embed(content, in: window)
        content.addTab(tab, select: true)

        content.newTabHandler = { [weak self, weak window] in
            guard let self, let window else { return }
            self.openNewTab(in: window)
        }
        content.closeTabHandler = { [weak self] tabID in
            self?.manager.closeTab(id: tabID)
            self?.syncFromManager()
        }
        content.selectionHandler = { [weak self, weak window] _ in
            guard let self, let window else { return }
            self.focus(window: window)
        }
        installCommonHandlers(on: window)
        applyInitialFrameIfNeeded(window)
        desktopSurfaceView.bringSubviewToFront(window)
        window.applyChrome(active: false)
        return window
    }

    /// Opens an applet's window, or brings forward the one a global applet already has.
    @discardableResult
    func openTool(_ identifier: String) -> WorkspaceWindowView? {
        if WorkspaceToolRegistry.isGlobal(identifier), let existing = window(forTool: identifier) {
            focus(window: existing)
            return existing
        }
        guard let applet = WorkspaceToolRegistry.makeViewController(for: identifier) else { return nil }
        applet.workspaceHost = self
        let window = WorkspaceWindowView(title: WorkspaceToolRegistry.title(for: identifier), showsCloseButton: true)
        window.toolIdentifier = identifier
        window.preferredSize = WorkspaceToolRegistry.preferredSize(for: identifier)
        window.resizable = true
        window.minimumSize = WorkspaceToolRegistry.minimumSize(for: identifier)
        window.frame = CGRect(origin: .zero, size: window.preferredSize)
        window.desktopIndex = activeDesktopIndex
        desktopSurfaceView.addSubview(window)
        desktopWindows.append(window)
        appletByWindow[window.serial] = applet
        embed(applet, in: window)
        installCommonHandlers(on: window)
        window.applyChrome(active: false)
        if identifier == WorkspaceToolRegistry.desktops {
            placeAtTopRight(window)
        } else {
            applyInitialFrameIfNeeded(window)
        }
        focus(window: window)
        postDesktopsDidChange()
        return window
    }

    func window(forTool identifier: String) -> WorkspaceWindowView? {
        desktopWindows.first { $0.toolIdentifier == identifier }
    }

    private func placeAtTopRight(_ window: WorkspaceWindowView) {
        let usable = desktopUsableBounds()
        guard usable.width > 0 else {
            applyInitialFrameIfNeeded(window)
            return
        }
        let inset: CGFloat = WorkspaceMetrics.usesPhoneLayout ? 8 : 12
        var frame = window.frame
        frame.origin.x = usable.maxX - frame.width - inset
        frame.origin.y = usable.minY + inset
        window.frame = clampedFrame(frame, for: window)
        window.didApplyInitialFrame = true
    }

    /// The Desktops applet's window follows how many Desktops it lists.
    func autosizeDesktopsWindow(contentHeight: CGFloat) {
        guard let window = window(forTool: WorkspaceToolRegistry.desktops), !window.zoomedToFullscreen else { return }
        let usable = desktopUsableBounds()
        let wanted = contentHeight + WorkspaceMetrics.titleBarHeight
        let height = min(max(wanted, window.minimumSize.height), usable.height > 0 ? usable.height : wanted)
        guard abs(window.frame.height - height) > 0.5 else { return }
        var frame = window.frame
        frame.size.height = height
        window.frame = clampedFrame(frame, for: window)
        window.preferredSize = window.frame.size
    }

    private func removeWindow(_ window: WorkspaceWindowView) {
        if let content = contentByWindow.removeValue(forKey: window.serial) {
            content.willMove(toParent: nil)
            content.view.removeFromSuperview()
            content.removeFromParent()
        }
        if let applet = appletByWindow.removeValue(forKey: window.serial) {
            applet.willMove(toParent: nil)
            applet.view.removeFromSuperview()
            applet.removeFromParent()
        }
        if raisedWindow === window {
            raisedWindow = nil
            frameBeforeKeyboardRaise = .null
        }
        desktopWindows.removeAll { $0 === window }
        window.removeFromSuperview()
        if focusedWindow === window {
            focusedWindow = nil
        }
    }

    /// Closing a terminal window ends its sessions, and an exiting shell closes its tab, both
    /// by way of the manager. Closing an applet ends nothing else.
    func closeWindow(_ window: WorkspaceWindowView) {
        if appletByWindow[window.serial] != nil {
            let wasFocused = focusedWindow === window
            removeWindow(window)
            if wasFocused {
                if let top = frontmostWindow() {
                    focus(window: top)
                } else {
                    focusDesktop()
                }
            }
            postDesktopsDidChange()
            return
        }
        guard let content = contentByWindow[window.serial] else { return }
        for hosted in content.hostedTabs where !manager.isRootTab(hosted.tabID) {
            manager.closeTab(id: hosted.tabID)
        }
        syncFromManager()
    }

    func openNewTerminalWindow() {
        pendingTabWindow = nil
        _ = manager.openShellTab()
        syncFromManager()
    }

    func openNewTab(in window: WorkspaceWindowView) {
        pendingTabWindow = window
        _ = manager.openShellTab()
        syncFromManager()
    }

    /// The top window on the Desktop being looked at.
    func frontmostWindow() -> WorkspaceWindowView? {
        desktopSurfaceView.subviews.reversed()
            .compactMap { $0 as? WorkspaceWindowView }
            .first { isOnActiveDesktop($0) }
    }

    private func isOnActiveDesktop(_ window: WorkspaceWindowView) -> Bool {
        window.desktopIndex == activeDesktopIndex || window.toolIdentifier.map(WorkspaceToolRegistry.isGlobal) == true
    }

    var focusedWindowForTesting: WorkspaceWindowView? { focusedWindow }
    var desktopSizeForTesting: CGSize { desktopSurfaceView.bounds.size }

    func setWindowFrameForTesting(_ window: WorkspaceWindowView, fractions: CGRect) {
        let bounds = desktopSurfaceView.bounds
        let frame = CGRect(x: bounds.width * fractions.minX,
                           y: bounds.height * fractions.minY,
                           width: bounds.width * fractions.width,
                           height: bounds.height * fractions.height).integral
        window.frame = clampedFrame(frame, for: window)
        window.preferredSize = window.frame.size
        window.frameDidChangeHandler?()
    }

    func assignWindow(_ window: WorkspaceWindowView, toDesktop index: Int) {
        guard window.toolIdentifier.map(WorkspaceToolRegistry.isGlobal) != true else { return }
        window.desktopIndex = max(0, min(index, desktopCount - 1))
        applyDesktopVisibility()
        if focusedWindow === window, !isOnActiveDesktop(window) {
            focusTopOrDesktop()
        }
        postDesktopsDidChange()
    }

    // MARK: - Focus

    /// Brings a window forward and gives it the keyboard, going to its Desktop if it is on
    /// another.
    func focus(window: WorkspaceWindowView) {
        if !isOnActiveDesktop(window) {
            activeDesktopIndex = window.desktopIndex
            showDesktopToast()
            postDesktopsDidChange()
        }
        desktopHasFocus = false
        desktopSurfaceView.bringSubviewToFront(window)
        focusedWindow = window
        if let tabID = contentByWindow[window.serial]?.selectedTabID {
            if manager.selectedId != tabID {
                manager.selectedId = tabID
            }
            lastSeenSelectedId = tabID
        } else {
            // An applet with no typing of its own takes the keyboard away from whatever
            // terminal had it.
            becomeFirstResponder()
        }
        applyDesktopVisibility()
        applyKeyboardToFocusedWindow()
        DispatchQueue.main.async {
            NotificationCenter.default.post(name: .terminalInputFocusRequested, object: nil)
        }
    }

    private func focusDesktop() {
        desktopHasFocus = true
        focusedWindow = nil
        applyStates()
        applyKeyboardToFocusedWindow()
        becomeFirstResponder()
    }

    func focusDesktopForTesting() {
        focusDesktop()
    }

    private func focusTopOrDesktop() {
        if let top = frontmostWindow() {
            focus(window: top)
        } else {
            focusDesktop()
        }
    }

    private func applyStates() {
        for window in desktopWindows {
            let onDesktop = isOnActiveDesktop(window)
            let active = !desktopHasFocus && window === focusedWindow && onDesktop
            window.isChromeActive = active
            contentByWindow[window.serial]?.setWindowState(visible: onDesktop, focused: active)
        }
    }

    private func cycleWindows(step: Int) {
        let ordered = desktopSurfaceView.subviews
            .compactMap { $0 as? WorkspaceWindowView }
            .filter { isOnActiveDesktop($0) }
        guard ordered.count > 1, let current = focusedWindow, let index = ordered.firstIndex(where: { $0 === current }) else {
            if let top = frontmostWindow() {
                focus(window: top)
            }
            return
        }
        // The frontmost is last, so going forward means the one at the bottom.
        let next = step > 0 ? ordered[0] : ordered[(index + ordered.count - 1) % ordered.count]
        focus(window: next)
    }

    // MARK: - Desktops

    private func applyDesktopVisibility() {
        for window in desktopWindows {
            window.isHidden = !isOnActiveDesktop(window)
            if window.toolIdentifier.map(WorkspaceToolRegistry.isGlobal) == true {
                // Global chrome appears on every Desktop, in front so it isn't covered, but
                // not over the window that was just given focus.
                if focusedWindow !== window, let top = focusedWindow {
                    desktopSurfaceView.bringSubviewToFront(window)
                    desktopSurfaceView.bringSubviewToFront(top)
                }
            }
        }
        applyStates()
    }

    private func postDesktopsDidChange() {
        NotificationCenter.default.post(name: .workspaceDesktopsDidChange, object: self)
    }

    func switchToDesktop(_ requested: Int) {
        let index = max(0, min(requested, desktopCount - 1))
        guard index != activeDesktopIndex else { return }
        activeDesktopIndex = index
        focusedWindow = focusedWindow.flatMap { isOnActiveDesktop($0) ? $0 : nil }
        applyDesktopVisibility()
        showDesktopToast()
        focusTopOrDesktop()
        postDesktopsDidChange()
    }

    func createNewDesktop() {
        desktopCount += 1
        switchToDesktop(desktopCount - 1)
        postDesktopsDidChange()
    }

    /// Closes the Desktop's windows and shifts the higher Desktops down. A locked Desktop
    /// can't be removed. A window whose session can't end, the root shell's, moves to the
    /// first Desktop instead.
    func removeDesktop(_ requested: Int) {
        guard desktopCount > 1 else { return }
        let index = max(0, min(requested, desktopCount - 1))
        guard !isDesktopLocked(index) else { return }
        for window in desktopWindows {
            if window.toolIdentifier.map(WorkspaceToolRegistry.isGlobal) == true { continue }
            if window.desktopIndex == index {
                let holdsRoot = contentByWindow[window.serial]?.hostedTabs.contains { manager.isRootTab($0.tabID) } == true
                if holdsRoot {
                    window.desktopIndex = 0
                }
                closeWindow(window)
            } else if window.desktopIndex > index {
                window.desktopIndex -= 1
            }
        }
        lockedDesktops = Set(lockedDesktops.compactMap { $0 > index ? $0 - 1 : ($0 < index ? $0 : nil) })
        if index < desktopNames.count {
            desktopNames.remove(at: index)
        }
        trimDesktopNames()
        desktopCount -= 1
        if activeDesktopIndex >= desktopCount {
            activeDesktopIndex = desktopCount - 1
        } else if activeDesktopIndex > index {
            activeDesktopIndex -= 1
        }
        applyDesktopVisibility()
        focusTopOrDesktop()
        postDesktopsDidChange()
    }

    func isDesktopLocked(_ index: Int) -> Bool {
        // The first Desktop is permanently protected.
        index == 0 || lockedDesktops.contains(index)
    }

    func toggleDesktopLock(_ index: Int) {
        guard index > 0, index < desktopCount else { return }
        if lockedDesktops.contains(index) {
            lockedDesktops.remove(index)
        } else {
            lockedDesktops.insert(index)
        }
        postDesktopsDidChange()
    }

    func customNameForDesktop(_ index: Int) -> String? {
        guard desktopNames.indices.contains(index), !desktopNames[index].isEmpty else { return nil }
        return desktopNames[index]
    }

    /// A Desktop without a name is named for its position, so removing one renumbers the
    /// unnamed ones after it.
    func nameForDesktop(_ index: Int) -> String {
        customNameForDesktop(index) ?? "Desktop \(index + 1)"
    }

    func setName(_ name: String, forDesktop index: Int) {
        guard index >= 0, index < desktopCount else { return }
        while desktopNames.count <= index {
            desktopNames.append("")
        }
        desktopNames[index] = name.trimmingCharacters(in: .whitespacesAndNewlines)
        trimDesktopNames()
        postDesktopsDidChange()
    }

    private func trimDesktopNames() {
        while desktopNames.last?.isEmpty == true {
            desktopNames.removeLast()
        }
    }

    func setDesktopCountForTesting(_ count: Int) {
        desktopCount = max(1, count)
        if activeDesktopIndex >= desktopCount {
            activeDesktopIndex = desktopCount - 1
        }
        applyDesktopVisibility()
        postDesktopsDidChange()
    }

    // MARK: - Saved arrangements

    private func normalizedFrame(_ frame: CGRect) -> [String: Double]? {
        let usable = desktopUsableBounds()
        guard usable.width > 0, usable.height > 0 else { return nil }
        return [
            "x": Double((frame.minX - usable.minX) / usable.width),
            "y": Double((frame.minY - usable.minY) / usable.height),
            "width": Double(frame.width / usable.width),
            "height": Double(frame.height / usable.height),
        ]
    }

    private func applyNormalizedFrame(_ descriptor: [String: Any]?, to window: WorkspaceWindowView) {
        guard let descriptor else { return }
        let usable = desktopUsableBounds()
        guard usable.width > 0, usable.height > 0 else { return }
        func value(_ key: String) -> CGFloat {
            CGFloat((descriptor[key] as? NSNumber)?.doubleValue ?? 0)
        }
        let width = min(max(value("width") > 0 ? value("width") * usable.width : window.preferredSize.width, 1), usable.width)
        let height = min(max(value("height") > 0 ? value("height") * usable.height : window.preferredSize.height, 1), usable.height)
        let originX = min(max(usable.minX + value("x") * usable.width, usable.minX), usable.maxX - width)
        let originY = min(max(usable.minY + value("y") * usable.height, usable.minY), usable.maxY - height)
        let frame = CGRect(x: originX, y: originY, width: width, height: height).integral
        window.frame = frame
        window.preferredSize = frame.size
        window.didApplyInitialFrame = true
        clampToVisibleBounds(window)
    }

    /// An arrangement is how many Desktops there are, which window is on which, and where
    /// they sit. Nothing live: a shell, its scrollback, an applet's contents.
    func arrangementDescriptors() -> [[String: Any]] {
        var layout: [[String: Any]] = []
        var desktops: [String: Any] = ["kind": "desktops", "count": desktopCount, "active": activeDesktopIndex]
        if !desktopNames.isEmpty {
            desktops["names"] = desktopNames
        }
        layout.append(desktops)
        for window in desktopSurfaceView.subviews.compactMap({ $0 as? WorkspaceWindowView }) {
            var descriptor: [String: Any] = ["desktopIndex": window.desktopIndex]
            if let frame = normalizedFrame(window.frame) {
                descriptor["frame"] = frame
            }
            if let identifier = window.toolIdentifier {
                descriptor["kind"] = "tool"
                descriptor["toolIdentifier"] = identifier
            } else if let content = contentByWindow[window.serial] {
                descriptor["kind"] = "terminal"
                descriptor["tabCount"] = content.hostedTabs.count
                descriptor["selectedTab"] = content.selectedIndex
                // These last for this run only. They let a restore move the window that holds
                // the sessions instead of opening another; after a relaunch none match.
                descriptor["tabIDs"] = content.hostedTabs.map { String($0.tabID) }
                descriptor["root"] = content.hostedTabs.contains { manager.isRootTab($0.tabID) }
            } else {
                continue
            }
            layout.append(descriptor)
        }
        return layout
    }

    /// Structure, not pixels: how many Desktops, and which window is on which. Frames are
    /// saved but left out of this, or nudging a window would light up "unsaved" mid-drag.
    private func signature(of layout: [[String: Any]]) -> String {
        var count = 1
        var names: [String] = []
        var parts: [String] = []
        for descriptor in layout {
            switch descriptor["kind"] as? String {
            case "desktops":
                count = max(1, (descriptor["count"] as? NSNumber)?.intValue ?? 1)
                names = descriptor["names"] as? [String] ?? []
            case "tool":
                parts.append("\(descriptor["toolIdentifier"] as? String ?? "")@\((descriptor["desktopIndex"] as? NSNumber)?.intValue ?? 0)")
            case "terminal":
                parts.append("terminal@\((descriptor["desktopIndex"] as? NSNumber)?.intValue ?? 0)")
            default:
                break
            }
        }
        return "\(count)|\(parts.sorted().joined(separator: ","))|\(names.joined(separator: "\u{1e}"))"
    }

    private func savedArrangement() -> [[String: Any]]? {
        UserDefaults.standard.array(forKey: WorkspaceSettings.savedDesktopsKey) as? [[String: Any]]
    }

    var arrangementIsSaved: Bool {
        if savedSignature == nil, let saved = savedArrangement() {
            savedSignature = signature(of: saved)
        }
        return savedSignature != nil && savedSignature == signature(of: arrangementDescriptors())
    }

    func saveArrangement() {
        let layout = arrangementDescriptors()
        UserDefaults.standard.set(layout, forKey: WorkspaceSettings.savedDesktopsKey)
        // Now rather than at the system's leisure: the next thing this has to survive is the
        // app being killed.
        UserDefaults.standard.synchronize()
        savedSignature = signature(of: layout)
        postDesktopsDidChange()
    }

    func restoreArrangementFromButton() {
        guard let layout = savedArrangement(), !layout.isEmpty else {
            let alert = UIAlertController(title: "No Saved Desktops",
                                          message: "Press Save first, then Restore brings that arrangement back.",
                                          preferredStyle: .alert)
            alert.addAction(UIAlertAction(title: "OK", style: .default))
            present(alert, animated: true)
            return
        }
        applyArrangement(layout)
        savedSignature = signature(of: arrangementDescriptors())
        postDesktopsDidChange()
    }

    /// Puts the windows back where they were and does nothing else. A terminal is moved, or
    /// opened fresh if its window is gone, every one after a relaunch. It is never closed,
    /// because closing one ends a shell. An applet the layout doesn't mention was opened after
    /// the save, so it goes.
    func applyArrangement(_ layout: [[String: Any]]) {
        var toolDescriptors: [[String: Any]] = []
        var terminalDescriptors: [[String: Any]] = []
        var desktopsDescriptor: [String: Any]?
        for descriptor in layout {
            switch descriptor["kind"] as? String {
            case "desktops": desktopsDescriptor = descriptor
            case "tool": toolDescriptors.append(descriptor)
            case "terminal": terminalDescriptors.append(descriptor)
            default: break
            }
        }

        // Desktops first: a window can't go on one that doesn't exist yet. Never shrink past
        // one that holds something: Restore isn't a licence to throw work away.
        let highestLive = desktopWindows.map(\.desktopIndex).max() ?? 0
        let savedCount = max(1, (desktopsDescriptor?["count"] as? NSNumber)?.intValue ?? 1)
        desktopCount = max(savedCount, highestLive + 1)
        desktopNames = (desktopsDescriptor?["names"] as? [String]) ?? []
        trimDesktopNames()

        func assign(_ window: WorkspaceWindowView, _ descriptor: [String: Any]) {
            applyNormalizedFrame(descriptor["frame"] as? [String: Any], to: window)
            if window.toolIdentifier.map(WorkspaceToolRegistry.isGlobal) == true {
                window.desktopIndex = 0
                return
            }
            let index = max(0, (descriptor["desktopIndex"] as? NSNumber)?.intValue ?? 0)
            window.desktopIndex = index
            desktopCount = max(desktopCount, index + 1)
        }

        var wanted = Set<String>()
        for descriptor in toolDescriptors {
            guard let identifier = descriptor["toolIdentifier"] as? String, !identifier.isEmpty,
                  wanted.insert(identifier).inserted else { continue }
            guard let window = window(forTool: identifier) ?? openTool(identifier) else { continue }
            assign(window, descriptor)
        }
        for window in desktopWindows {
            if let identifier = window.toolIdentifier, !WorkspaceToolRegistry.isGlobal(identifier), !wanted.contains(identifier) {
                closeWindow(window)
            }
        }

        var claimed = Set<Int>()
        for descriptor in terminalDescriptors {
            let tabIDs = (descriptor["tabIDs"] as? [String] ?? []).compactMap(UInt64.init)
            var window = tabIDs.lazy.compactMap { self.windowHosting(tabID: $0) }.first
            if window == nil, (descriptor["root"] as? NSNumber)?.boolValue == true {
                window = manager.tabs.first { self.manager.isRootTab($0.id) }.flatMap { self.windowHosting(tabID: $0.id) }
            }
            if let found = window, claimed.contains(found.serial) {
                window = nil
            }
            if window == nil {
                // After a relaunch: a fresh shell where the saved one was, with as many tabs.
                openNewTerminalWindow()
                window = windowHosting(tabID: manager.selectedId)
                let tabCount = max(1, (descriptor["tabCount"] as? NSNumber)?.intValue ?? 1)
                if let opened = window, tabCount > 1 {
                    for _ in 1..<tabCount {
                        openNewTab(in: opened)
                    }
                }
            }
            guard let target = window else { continue }
            claimed.insert(target.serial)
            assign(target, descriptor)
            if let selected = (descriptor["selectedTab"] as? NSNumber)?.intValue {
                contentByWindow[target.serial]?.setSelectedIndexWithoutFocus(selected)
            }
        }

        if let saved = (desktopsDescriptor?["active"] as? NSNumber)?.intValue, saved >= 0, saved < desktopCount {
            activeDesktopIndex = saved
        }
        focusedWindow = focusedWindow.flatMap { isOnActiveDesktop($0) ? $0 : nil }
        applyDesktopVisibility()
        focusTopOrDesktop()
        refreshTitles()
        postDesktopsDidChange()
    }

    // MARK: - Keyboard

    private func installKeyboardObservers() {
        let center = NotificationCenter.default
        let change = center.addObserver(forName: UIResponder.keyboardWillChangeFrameNotification, object: nil, queue: .main) { [weak self] note in
            let value = note.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? NSValue
            let frame = value?.cgRectValue
            MainActor.assumeIsolated {
                self?.keyboardFrameInScreen = frame
                self?.applyKeyboardToFocusedWindow()
            }
        }
        let hide = center.addObserver(forName: UIResponder.keyboardWillHideNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.keyboardFrameInScreen = nil
                self?.applyKeyboardToFocusedWindow()
            }
        }
        keyboardObservers = [change, hide]
    }

    /// Keeps the focused terminal above the keyboard: raise its window as far as the top of
    /// the desktop allows, and what the keyboard still covers comes off the terminal's
    /// bottom. The window goes back when the keyboard goes.
    private func applyKeyboardToFocusedWindow() {
        if let raised = raisedWindow, raised !== focusedWindow || keyboardFrameInScreen == nil {
            if !frameBeforeKeyboardRaise.isNull {
                raised.frame = frameBeforeKeyboardRaise
            }
            raisedWindow = nil
            frameBeforeKeyboardRaise = .null
        }
        for window in desktopWindows where window !== focusedWindow {
            contentByWindow[window.serial]?.setKeyboardOverlap(0)
        }
        guard let window = focusedWindow, let content = contentByWindow[window.serial] else { return }
        guard let keyboard = keyboardFrameInScreen, let hostWindow = view.window else {
            content.setKeyboardOverlap(0)
            return
        }
        let keyboardInSurface = desktopSurfaceView.convert(hostWindow.convert(keyboard, from: nil), from: hostWindow)
        let top = keyboardInSurface.minY
        if window.frame.maxY > top, !keyboardInSurface.isEmpty, !window.zoomedToFullscreen {
            let room = max(0, window.frame.minY - desktopUsableBounds().minY)
            let shift = min(window.frame.maxY - top, room)
            if shift > 0 {
                if raisedWindow == nil {
                    raisedWindow = window
                    frameBeforeKeyboardRaise = window.frame
                }
                window.frame.origin.y -= shift
            }
        }
        let overlap = keyboardInSurface.isEmpty ? 0 : max(0, window.frame.maxY - top)
        content.setKeyboardOverlap(overlap)
    }

    // MARK: - Window-level keys

    func handleWindowPresses(_ event: UIPressesEvent) -> Bool {
        for press in event.allPresses where press.phase == .began {
            guard let key = press.key else { continue }
            let modifiers = key.modifierFlags.intersection([.command, .shift, .alternate, .control])
            let input = key.charactersIgnoringModifiers.lowercased()
            if key.keyCode == .keyboardTab {
                if modifiers == .control {
                    cycleWindows(step: 1)
                    return true
                }
                if modifiers == [.control, .shift] {
                    cycleWindows(step: -1)
                    return true
                }
            }
            // Cmd and Ctrl-Option, not Ctrl alone: a terminal claims Ctrl, Shift and Alt with
            // the arrows for its own sequences, and a shell uses them to jump words.
            if modifiers == .command || modifiers == [.control, .alternate] {
                if key.keyCode == .keyboardLeftArrow {
                    switchToDesktop(activeDesktopIndex - 1)
                    return true
                }
                if key.keyCode == .keyboardRightArrow {
                    switchToDesktop(activeDesktopIndex + 1)
                    return true
                }
            }
            if modifiers == [.control, .alternate], let number = Int(input), number >= 1 {
                switchToDesktop(number - 1)
                return true
            }
            if modifiers == .command {
                if input == "t" {
                    if let window = focusedWindow, contentByWindow[window.serial] != nil {
                        openNewTab(in: window)
                    } else {
                        openNewTerminalWindow()
                    }
                    return true
                }
                if input == "n" {
                    openNewTerminalWindow()
                    return true
                }
                if input == "w" {
                    closeFocused()
                    return true
                }
                if let number = Int(input), number >= 1 {
                    if let window = focusedWindow {
                        contentByWindow[window.serial]?.selectTab(atIndex: number - 1)
                    }
                    return true
                }
            }
            if modifiers == [.command, .shift], let window = focusedWindow {
                if input == "]" || input == "}" {
                    contentByWindow[window.serial]?.selectNextTab(step: 1)
                    return true
                }
                if input == "[" || input == "{" {
                    contentByWindow[window.serial]?.selectNextTab(step: -1)
                    return true
                }
            }
        }
        return false
    }

    var terminalControlKeysGoToSelected: Bool {
        guard !desktopHasFocus, let window = focusedWindow else { return false }
        return contentByWindow[window.serial] != nil
    }

    private func closeFocused() {
        guard let window = focusedWindow else { return }
        if appletByWindow[window.serial] != nil {
            closeWindow(window)
            return
        }
        guard let tabID = contentByWindow[window.serial]?.selectedTabID else { return }
        manager.closeTab(id: tabID)
        syncFromManager()
    }

    // MARK: - Gestures

    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer, shouldReceive touch: UITouch) -> Bool {
        if gestureRecognizer.view === desktopSurfaceView {
            return touch.view === desktopSurfaceView
        }
        return true
    }

    @objc private func handleDesktopTap(_ recognizer: UITapGestureRecognizer) {
        if recognizer.state == .ended {
            focusDesktop()
        }
    }

    @objc private func handleDesktopLongPress(_ recognizer: UILongPressGestureRecognizer) {
        guard recognizer.state == .began else { return }
        let point = recognizer.location(in: desktopSurfaceView)
        presentRootMenu(from: desktopSurfaceView, sourceRect: CGRect(origin: point, size: CGSize(width: 1, height: 1)))
    }

    @objc private func handleTwoFingerLongPress(_ recognizer: UILongPressGestureRecognizer) {
        guard recognizer.state == .began else { return }
        let point = recognizer.location(in: desktopSurfaceView)
        presentRootMenu(from: desktopSurfaceView, sourceRect: CGRect(origin: point, size: CGSize(width: 1, height: 1)))
    }

    @objc private func handleDesktopSwipe(_ recognizer: UISwipeGestureRecognizer) {
        switchToDesktop(activeDesktopIndex + (recognizer.direction == .left ? 1 : -1))
    }

    @objc private func menuPipTapped(_ sender: UIButton) {
        presentRootMenu(from: sender, sourceRect: sender.bounds)
    }

    // MARK: - Menus

    /// The window a menu was raised from: a window's own ☰ button is inside it.
    private func window(containing sourceView: UIView) -> WorkspaceWindowView? {
        var view: UIView? = sourceView
        while let current = view {
            if let window = current as? WorkspaceWindowView {
                return window
            }
            view = current.superview
        }
        return nil
    }

    private func presentSheet(title: String?, actions: [UIAlertAction], from sourceView: UIView, sourceRect: CGRect) {
        let sheet = UIAlertController(title: title, message: nil, preferredStyle: .actionSheet)
        actions.forEach(sheet.addAction)
        sheet.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        // An iPad presents this as a popover, which must have an anchor or UIKit raises.
        sheet.popoverPresentationController?.sourceView = sourceView
        sheet.popoverPresentationController?.sourceRect = sourceRect
        present(sheet, animated: true)
    }

    /// An item that presents another controller defers to the next turn: presenting from
    /// inside an alert action's handler races the alert's own dismissal, UIKit drops it as
    /// "already presenting", and the item silently does nothing.
    private func deferred(_ work: @escaping @MainActor () -> Void) -> (UIAlertAction) -> Void {
        { _ in
            DispatchQueue.main.async {
                MainActor.assumeIsolated(work)
            }
        }
    }

    func presentRootMenu(from sourceView: UIView, sourceRect: CGRect) {
        var actions: [UIAlertAction] = []
        actions.append(UIAlertAction(title: "New Terminal", style: .default) { [weak self] _ in
            self?.openNewTerminalWindow()
        })
        let menuWindow = window(containing: sourceView)
        if let menuWindow, contentByWindow[menuWindow.serial] != nil {
            actions.append(UIAlertAction(title: "New Tab", style: .default) { [weak self, weak menuWindow] _ in
                guard let self, let menuWindow else { return }
                self.openNewTab(in: menuWindow)
            })
        }
        actions.append(UIAlertAction(title: "Windows", style: .default, handler: deferred { [weak self] in
            self?.presentWindowList(from: sourceView, sourceRect: sourceRect)
        }))
        actions.append(UIAlertAction(title: "Desktops", style: .default) { [weak self] _ in
            self?.openTool(WorkspaceToolRegistry.desktops)
        })
        actions.append(UIAlertAction(title: "New Desktop", style: .default) { [weak self] _ in
            self?.createNewDesktop()
        })
        if let menuWindow, desktopCount > 1, menuWindow.toolIdentifier.map(WorkspaceToolRegistry.isGlobal) != true {
            actions.append(UIAlertAction(title: "Move to Desktop…", style: .default, handler: deferred { [weak self] in
                self?.presentMoveToDesktop(menuWindow, from: sourceView, sourceRect: sourceRect)
            }))
        }
        if let tabID = (menuWindow ?? focusedWindow).flatMap({ contentByWindow[$0.serial]?.selectedTabID }) {
            actions.append(UIAlertAction(title: "Terminal Settings", style: .default, handler: deferred { [weak self] in
                self?.presentTerminalSettings(tabID: tabID)
            }))
        }
        actions.append(UIAlertAction(title: "App Diagnostics", style: .default, handler: deferred { [weak self] in
            self?.present(UIHostingController(rootView: AppDiagnosticsView()), animated: true)
        }))
        let opensAtLaunch = WorkspaceSettings.opensAtLaunch
        actions.append(UIAlertAction(title: "Open at Launch: \(opensAtLaunch ? "On" : "Off")", style: .default) { _ in
            WorkspaceSettings.opensAtLaunch = !opensAtLaunch
        })
        actions.append(UIAlertAction(title: "Back to Tabs", style: .default, handler: deferred { [weak self] in
            self?.leaveHandler?()
        }))
        presentSheet(title: "Workspace", actions: actions, from: sourceView, sourceRect: sourceRect)
    }

    private func presentWindowList(from sourceView: UIView, sourceRect: CGRect) {
        let ordered = desktopSurfaceView.subviews.reversed().compactMap { $0 as? WorkspaceWindowView }
        let actions = ordered.map { window -> UIAlertAction in
            let title = window.titleLabel.text ?? "Window"
            let label = desktopCount > 1 ? "\(title) (\(nameForDesktop(window.desktopIndex)))" : title
            return UIAlertAction(title: label, style: .default) { [weak self, weak window] _ in
                guard let self, let window else { return }
                self.focus(window: window)
            }
        }
        presentSheet(title: "Windows", actions: actions, from: sourceView, sourceRect: sourceRect)
    }

    private func presentMoveToDesktop(_ window: WorkspaceWindowView, from sourceView: UIView, sourceRect: CGRect) {
        let actions = (0..<desktopCount).filter { $0 != window.desktopIndex }.map { index -> UIAlertAction in
            UIAlertAction(title: nameForDesktop(index), style: .default) { [weak self, weak window] _ in
                guard let self, let window else { return }
                self.assignWindow(window, toDesktop: index)
            }
        }
        presentSheet(title: "Move to Desktop", actions: actions, from: sourceView, sourceRect: sourceRect)
    }

    private func presentTerminalSettings(tabID: UInt64) {
        guard let tab = manager.tabs.first(where: { $0.id == tabID }) else { return }
        let settings = TerminalSettingsView(appearanceSettings: tab.appearanceSettings, tabId: tabID)
        present(UIHostingController(rootView: settings), animated: true)
    }
}
