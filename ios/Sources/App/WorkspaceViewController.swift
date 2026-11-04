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

/// A desktop of movable windows inside the one iOS window, which is why it works on
/// iPhone: it doesn't depend on scenes or Stage Manager.
///
/// Sessions belong to `TerminalTabManager`. Workspace is another way of showing them, so
/// leaving it and coming back ends nothing. `selectedId` stays "the session with the
/// keyboard".
@MainActor
final class WorkspaceViewController: UIViewController, UIGestureRecognizerDelegate, TerminalKeyRouting {
    private let manager = TerminalTabManager.shared
    private let desktopSurfaceView = UIView()
    private let menuPip = UIButton(type: .system)

    private(set) var desktopWindows: [WorkspaceWindowView] = []
    private var contentByWindow: [Int: WorkspaceTerminalTabsViewController] = [:]
    private var cascadeIndex = 0
    private weak var focusedWindow: WorkspaceWindowView?
    private var desktopHasFocus = false

    private var managerObserver: AnyCancellable?
    private var syncScheduled = false
    private var didInitialSync = false
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

        buildMenuPip()
        NSLayoutConstraint.activate([
            desktopSurfaceView.topAnchor.constraint(equalTo: view.topAnchor),
            desktopSurfaceView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            desktopSurfaceView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            desktopSurfaceView.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])
        installKeyboardObservers()
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

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        guard desktopSurfaceView.bounds.width > 0, desktopSurfaceView.bounds.height > 0 else { return }
        if !didInitialSync {
            didInitialSync = true
            // Next turn: the tab bar's views are still leaving, and a session can only be
            // attached to one terminal view. Detaching after attaching would stop its output.
            DispatchQueue.main.async { [weak self] in
                self?.syncFromManager()
            }
        }
        for window in desktopWindows {
            applyInitialFrameIfNeeded(window)
            clampToVisibleBounds(window)
        }
    }

    override var canBecomeFirstResponder: Bool { true }

    /// Takes every window down without touching a session, on the way back to tabs.
    func tearDownWindows() {
        managerObserver = nil
        for window in desktopWindows {
            contentByWindow[window.serial]?.setWindowState(visible: false, focused: false)
            if let content = contentByWindow[window.serial] {
                content.hostedTabs.map(\.tabID).forEach { content.removeTab(tabID: $0) }
                content.willMove(toParent: nil)
                content.view.removeFromSuperview()
                content.removeFromParent()
            }
            window.removeFromSuperview()
        }
        desktopWindows.removeAll()
        contentByWindow.removeAll()
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

        if addedAny || !removedAny {
            followManagerSelection()
        } else if let window = focusedBefore, desktopWindows.contains(where: { $0 === window }) {
            // The manager picks the first shell when a selected tab goes. Stay in the window
            // the person was working in.
            focus(window: window)
        } else if let top = frontmostWindow() {
            focus(window: top)
        }
        refreshTitles()
        applyStates()
    }

    private func followManagerSelection() {
        guard !desktopHasFocus || focusedWindow == nil else {
            return
        }
        guard let window = windowHosting(tabID: manager.selectedId) else { return }
        contentByWindow[window.serial]?.select(tabID: manager.selectedId)
        if focusedWindow !== window {
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

    @discardableResult
    private func openWindow(for tab: TerminalTabManager.Tab) -> WorkspaceWindowView {
        let window = WorkspaceWindowView(title: tab.title, showsCloseButton: true)
        window.preferredSize = defaultTerminalWindowSize()
        window.resizable = true
        window.titleBarDoubleTapZoomEnabled = true
        window.minimumSize = WorkspaceMetrics.minimumTerminalSize
        window.frame = CGRect(origin: .zero, size: window.preferredSize)
        desktopSurfaceView.addSubview(window)
        desktopWindows.append(window)

        let content = WorkspaceTerminalTabsViewController()
        content.windowView = window
        contentByWindow[window.serial] = content
        addChild(content)
        content.view.translatesAutoresizingMaskIntoConstraints = false
        window.contentContainerView.addSubview(content.view)
        NSLayoutConstraint.activate([
            content.view.topAnchor.constraint(equalTo: window.contentContainerView.topAnchor),
            content.view.leadingAnchor.constraint(equalTo: window.contentContainerView.leadingAnchor),
            content.view.trailingAnchor.constraint(equalTo: window.contentContainerView.trailingAnchor),
            content.view.bottomAnchor.constraint(equalTo: window.contentContainerView.bottomAnchor),
        ])
        content.didMove(toParent: self)
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
        applyInitialFrameIfNeeded(window)
        desktopSurfaceView.bringSubviewToFront(window)
        window.applyChrome(active: false)
        return window
    }

    private func removeWindow(_ window: WorkspaceWindowView) {
        if let content = contentByWindow.removeValue(forKey: window.serial) {
            content.willMove(toParent: nil)
            content.view.removeFromSuperview()
            content.removeFromParent()
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

    /// Closing a window ends its sessions, and an exiting shell closes its tab, both by way
    /// of the manager.
    func closeWindow(_ window: WorkspaceWindowView) {
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

    func frontmostWindow() -> WorkspaceWindowView? {
        desktopSurfaceView.subviews.reversed().compactMap { $0 as? WorkspaceWindowView }.first
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

    // MARK: - Focus

    /// Brings a window forward and gives its selected terminal the keyboard.
    func focus(window: WorkspaceWindowView) {
        desktopHasFocus = false
        desktopSurfaceView.bringSubviewToFront(window)
        focusedWindow = window
        if let tabID = contentByWindow[window.serial]?.selectedTabID, manager.selectedId != tabID {
            manager.selectedId = tabID
        }
        applyStates()
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

    private func applyStates() {
        for window in desktopWindows {
            let active = !desktopHasFocus && window === focusedWindow
            window.isChromeActive = active
            contentByWindow[window.serial]?.setWindowState(visible: true, focused: active)
        }
    }

    private func cycleWindows(step: Int) {
        let ordered = desktopSurfaceView.subviews.compactMap { $0 as? WorkspaceWindowView }
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
            if modifiers == .command {
                if input == "t" {
                    if let window = focusedWindow {
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
                    closeFocusedTab()
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
        !desktopHasFocus && focusedWindow != nil
    }

    private func closeFocusedTab() {
        guard let window = focusedWindow, let tabID = contentByWindow[window.serial]?.selectedTabID else { return }
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
        if let menuWindow {
            actions.append(UIAlertAction(title: "New Tab", style: .default) { [weak self, weak menuWindow] _ in
                guard let self, let menuWindow else { return }
                self.openNewTab(in: menuWindow)
            })
        }
        actions.append(UIAlertAction(title: "Windows", style: .default, handler: deferred { [weak self] in
            self?.presentWindowList(from: sourceView, sourceRect: sourceRect)
        }))
        if let tabID = (menuWindow ?? focusedWindow).flatMap({ contentByWindow[$0.serial]?.selectedTabID }) {
            actions.append(UIAlertAction(title: "Terminal Settings", style: .default, handler: deferred { [weak self] in
                self?.presentTerminalSettings(tabID: tabID)
            }))
        }
        actions.append(UIAlertAction(title: "App Diagnostics", style: .default, handler: deferred { [weak self] in
            self?.present(UIHostingController(rootView: AppDiagnosticsView()), animated: true)
        }))
        actions.append(UIAlertAction(title: "Back to Tabs", style: .default, handler: deferred { [weak self] in
            self?.leaveHandler?()
        }))
        presentSheet(title: "Workspace", actions: actions, from: sourceView, sourceRect: sourceRect)
    }

    private func presentWindowList(from sourceView: UIView, sourceRect: CGRect) {
        let ordered = desktopSurfaceView.subviews.reversed().compactMap { $0 as? WorkspaceWindowView }
        let actions = ordered.map { window in
            UIAlertAction(title: window.titleLabel.text ?? "Window", style: .default) { [weak self, weak window] _ in
                guard let self, let window else { return }
                self.focus(window: window)
            }
        }
        presentSheet(title: "Windows", actions: actions, from: sourceView, sourceRect: sourceRect)
    }

    private func presentTerminalSettings(tabID: UInt64) {
        guard let tab = manager.tabs.first(where: { $0.id == tabID }) else { return }
        let settings = TerminalSettingsView(appearanceSettings: tab.appearanceSettings, tabId: tabID)
        present(UIHostingController(rootView: settings), animated: true)
    }
}
