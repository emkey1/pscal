import SwiftUI
import UIKit

@MainActor
final class TerminalWindow: UIWindow {
    private static func hasVisibleSDLWindow() -> Bool {
        let allWindows = UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .flatMap { $0.windows }
        return allWindows.contains { window in
            guard !window.isHidden else { return false }
            let className = NSStringFromClass(type(of: window)).lowercased()
            return className.contains("sdl") && className.contains("window")
        }
    }

    override var canBecomeKey: Bool {
        if pscalIOSSDLModeActive() != 0 {
            return false
        }
        return !Self.hasVisibleSDLWindow()
    }

    override func becomeKey() {
        if pscalIOSSDLModeActive() != 0 {
            return
        }
        super.becomeKey()
    }

    private lazy var commandKeyCommands: [UIKeyCommand] = {
        let newTab = UIKeyCommand(input: "t",
                                  modifierFlags: [.command],
                                  action: #selector(handleNewTabCommand))
        newTab.discoverabilityTitle = "New Tab"
        let closeTab = UIKeyCommand(input: "w",
                                    modifierFlags: [.command],
                                    action: #selector(handleCloseTabCommand))
        closeTab.discoverabilityTitle = "Close Tab"
        if #available(iOS 15.0, *) {
            newTab.wantsPriorityOverSystemBehavior = true
            closeTab.wantsPriorityOverSystemBehavior = true
        }
        var commands: [UIKeyCommand] = [newTab, closeTab]
        for i in 0...9 {
            let input = "\(i)"
            let title = (i == 0) ? "Select Tab 10" : "Select Tab \(i)"
            let selectCmd = UIKeyCommand(input: input,
                                         modifierFlags: [.command],
                                         action: #selector(handleSelectTabCommand(_:)))
            selectCmd.discoverabilityTitle = title
            let selectAlt = UIKeyCommand(input: input,
                                         modifierFlags: [.command, .alternate],
                                         action: #selector(handleSelectTabCommand(_:)))
            selectAlt.discoverabilityTitle = title
            if #available(iOS 15.0, *) {
                selectCmd.wantsPriorityOverSystemBehavior = true
                selectAlt.wantsPriorityOverSystemBehavior = true
            }
            commands.append(selectCmd)
            commands.append(selectAlt)
        }
        return commands
    }()

    private lazy var controlKeyCommands: [UIKeyCommand] = {
        let interrupt = UIKeyCommand(input: "c",
                                     modifierFlags: [.control],
                                     action: #selector(handleInterruptKeyCommand))
        interrupt.discoverabilityTitle = "Interrupt"
        let suspend = UIKeyCommand(input: "z",
                                   modifierFlags: [.control],
                                   action: #selector(handleSuspendKeyCommand))
        suspend.discoverabilityTitle = "Suspend"
        if #available(iOS 15.0, *) {
            interrupt.wantsPriorityOverSystemBehavior = true
            suspend.wantsPriorityOverSystemBehavior = true
        }
        return [interrupt, suspend]
    }()

    override var keyCommands: [UIKeyCommand]? {
        commandKeyCommands + controlKeyCommands
    }

    override func sendEvent(_ event: UIEvent) {
        if let pressesEvent = event as? UIPressesEvent {
            let router = TerminalTabManager.shared.keyRouter
            if let router, router.handleWindowPresses(pressesEvent) {
                return
            }
            let controlKeysGoToSelected = router?.terminalControlKeysGoToSelected ?? true
            if controlKeysGoToSelected, !terminalInputHasFirstResponder(), handleTerminalControlKey(pressesEvent) {
                return
            }
            if router == nil, handleCommandKey(pressesEvent) {
                return
            }
        }
        super.sendEvent(event)
    }

    private func handleTerminalControlKey(_ event: UIPressesEvent) -> Bool {
        let presses = event.allPresses
        guard !presses.isEmpty else {
            return false
        }
        for press in presses {
            if press.phase != .began {
                continue
            }
            guard let key = press.key else { continue }

            let rawScalars = key.charactersIgnoringModifiers.unicodeScalars
            let scalar = rawScalars.first
            let rawCharsScalars = key.characters.unicodeScalars
            let rawCharsScalar = rawCharsScalars.first
            let loweredValue = scalar.map { UInt32(tolower(Int32($0.value))) }
            let loweredCharsValue = rawCharsScalar.map { UInt32(tolower(Int32($0.value))) }
            let hasControl = key.modifierFlags.contains(.control)

            let isInterruptKey = key.keyCode == .keyboardC
            let isSuspendKey = key.keyCode == .keyboardZ
            let isInterrupt = (scalar?.value == 0x03) ||
                (rawCharsScalar?.value == 0x03) ||
                (hasControl && (loweredValue == 0x63 || loweredCharsValue == 0x63 || isInterruptKey))
            let isSuspend = (scalar?.value == 0x1A) ||
                (rawCharsScalar?.value == 0x1A) ||
                (hasControl && (loweredValue == 0x7A || loweredCharsValue == 0x7A || isSuspendKey))
            if isInterrupt {
                TerminalTabManager.shared.sendInterruptToSelected()
                return true
            }
            if isSuspend {
                TerminalTabManager.shared.sendSuspendToSelected()
                return true
            }
        }
        return false
    }

    private func terminalInputHasFirstResponder() -> Bool {
        return firstResponder(in: self) is TerminalKeyInputView
    }

    private func firstResponder(in view: UIView?) -> UIView? {
        guard let view else {
            return nil
        }
        if view.isFirstResponder {
            return view
        }
        for subview in view.subviews {
            if let responder = firstResponder(in: subview) {
                return responder
            }
        }
        return nil
    }

    @objc private func handleInterruptKeyCommand() {
        TerminalTabManager.shared.sendInterruptToSelected()
    }

    @objc private func handleSuspendKeyCommand() {
        TerminalTabManager.shared.sendSuspendToSelected()
    }

    private func handleCommandKey(_ event: UIPressesEvent) -> Bool {
        let presses = event.allPresses
        guard !presses.isEmpty else {
            return false
        }
        for press in presses {
            if press.phase != .began {
                continue
            }
            guard let key = press.key else { continue }
            let allowed: UIKeyModifierFlags = [.command, .shift, .alternate, .control]
            let modifiers = key.modifierFlags.intersection(allowed)
            let hasCommand = modifiers.contains(.command)
            if !hasCommand {
                continue
            }
            let input = key.charactersIgnoringModifiers.lowercased()
            let extraMods = modifiers.subtracting([.command])
            if (extraMods == [] || extraMods == [.alternate]),
               let number = Int(input) {
                TerminalTabManager.shared.selectTab(number: number)
                return true
            }
            if modifiers == .command {
                switch input {
                case "w":
                    TerminalTabManager.shared.closeSelectedTab()
                    return true
                case "t":
                    _ = TerminalTabManager.shared.openShellTab()
                    return true
                default:
                    break
                }
            }
        }
        return false
    }

    @objc private func handleNewTabCommand() {
        _ = TerminalTabManager.shared.openShellTab()
    }

    @objc private func handleCloseTabCommand() {
        TerminalTabManager.shared.closeSelectedTab()
    }

    @objc private func handleSelectTabCommand(_ command: UIKeyCommand) {
        guard let raw = command.input, let number = Int(raw) else { return }
        TerminalTabManager.shared.selectTab(number: number)
    }
}

/// UIKit shell that hosts the SwiftUI TerminalView and relies on keyboardLayoutGuide
/// to keep the terminal aligned with the on-screen keyboard.
final class TerminalRootViewController: UIViewController {
    /// A second iPad scene shares the one `TerminalTabManager`, and a session has one
    /// terminal view, so one scene at a time holds Workspace.
    private static weak var workspaceOwner: TerminalRootViewController?
    private static let instances = NSHashTable<TerminalRootViewController>.weakObjects()

    private let hostingController = UIHostingController(rootView: TerminalTabsRootView())
    private var keyboardObservers: [NSObjectProtocol] = []
    private var lastKeyboardOverlap: CGFloat = 0
    private(set) var workspace: WorkspaceViewController?

    /// The root the person is looking at.
    static var activeInstance: TerminalRootViewController? {
        let all = instances.allObjects
        return all.first { $0.view.window?.isKeyWindow == true } ?? all.first
    }

    override var shouldAutorotate: Bool { return true }
    override var supportedInterfaceOrientations: UIInterfaceOrientationMask {
        PscalAppDelegate.activeOrientationMask
    }

    override var preferredInterfaceOrientationForPresentation: UIInterfaceOrientation {
        let mask = PscalAppDelegate.activeOrientationMask
        if mask.contains(.portrait) {
            return .portrait
        }
        if mask.contains(.landscapeRight) {
            return .landscapeRight
        }
        if mask.contains(.landscapeLeft) {
            return .landscapeLeft
        }
        return .portrait
    }
    
    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        if let scene = view.window?.windowScene {
            let mask = PscalAppDelegate.activeOrientationMask
            if #available(iOS 16.0, *) {
                scene.requestGeometryUpdate(.iOS(interfaceOrientations: mask)) { _ in }
            }
        }
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        Self.instances.add(self)
        installTabBar()
        installKeyboardObservers()
        WorkspaceTestHooks.installIfEnabled()
        if ProcessInfo.processInfo.environment["PSCALI_WORKSPACE_START"] == "1" || WorkspaceSettings.opensAtLaunch {
            enterWorkspace()
        }
    }

    deinit {
        keyboardObservers.forEach { NotificationCenter.default.removeObserver($0) }
    }

    private func installTabBar() {
        addChild(hostingController)
        hostingController.view.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(hostingController.view)

        // Pin the hosted SwiftUI view to the safe area and keyboard guide.
        NSLayoutConstraint.activate([
            hostingController.view.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor),
            hostingController.view.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor),
            hostingController.view.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            hostingController.view.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor)
        ])

        hostingController.didMove(toParent: self)
    }

    /// Workspace is another way of showing the same sessions, so moving between it and the
    /// tab bar ends nothing.
    func enterWorkspace() {
        guard workspace == nil, Self.workspaceOwner == nil else { return }
        Self.workspaceOwner = self
        // Workspace handles the keyboard per window, and doesn't shrink as a whole.
        additionalSafeAreaInsets.bottom = 0
        lastKeyboardOverlap = 0
        hostingController.willMove(toParent: nil)
        hostingController.view.removeFromSuperview()
        hostingController.removeFromParent()

        let controller = WorkspaceViewController()
        controller.leaveHandler = { [weak self] in
            self?.leaveWorkspace()
        }
        addChild(controller)
        controller.view.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(controller.view)
        NSLayoutConstraint.activate([
            controller.view.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            controller.view.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            controller.view.topAnchor.constraint(equalTo: view.topAnchor),
            controller.view.bottomAnchor.constraint(equalTo: view.bottomAnchor)
        ])
        controller.didMove(toParent: self)
        workspace = controller
        TerminalTabManager.shared.keyRouter = controller
    }

    func leaveWorkspace() {
        guard let controller = workspace else { return }
        TerminalTabManager.shared.keyRouter = nil
        controller.tearDownWindows()
        controller.willMove(toParent: nil)
        controller.view.removeFromSuperview()
        controller.removeFromParent()
        workspace = nil
        if Self.workspaceOwner === self {
            Self.workspaceOwner = nil
        }
        // Next turn: the windows' terminal views are still leaving, and a session can only
        // be attached to one terminal view.
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.installTabBar()
            NotificationCenter.default.post(name: .terminalInputFocusRequested, object: nil)
        }
    }

    private func installKeyboardObservers() {
        let center = NotificationCenter.default
        let willChange = center.addObserver(
            forName: UIResponder.keyboardWillChangeFrameNotification,
            object: nil,
            queue: .main
        ) { [weak self] notification in
            self?.handleKeyboard(notification: notification)
        }
        let willHide = center.addObserver(
            forName: UIResponder.keyboardWillHideNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.updateKeyboardInset(overlap: 0)
        }
        keyboardObservers = [willChange, willHide]
    }

    private func handleKeyboard(notification: Notification) {
        guard
            let userInfo = notification.userInfo,
            let frameValue = userInfo[UIResponder.keyboardFrameEndUserInfoKey] as? NSValue
        else { return }
        let kbFrame = frameValue.cgRectValue
        let converted = view.convert(kbFrame, from: nil)
        let overlap = max(0, view.bounds.maxY - converted.minY)
        updateKeyboardInset(overlap: overlap)
    }

    private func updateKeyboardInset(overlap: CGFloat) {
        guard workspace == nil else { return }
        // The safe area already includes the inset set here, and a repeated notification mustn't count it twice.
        let baseBottomInset = view.safeAreaInsets.bottom - additionalSafeAreaInsets.bottom
        let extra = max(0, overlap - baseBottomInset)
        if abs(extra - lastKeyboardOverlap) < 0.5 {
            return
        }
        additionalSafeAreaInsets.bottom = extra
        let prev = lastKeyboardOverlap
        lastKeyboardOverlap = extra
        // When the keyboard shows or hides, bring the prompt back into view. Scroll for that and send
        // no keys: they would reach whatever program is running.
        let transitioned = (prev == 0 && extra > 0) || (prev > 0 && extra == 0)
        if transitioned {
            TerminalTabManager.shared.scrollSelectedToBottom()
        }
    }
}
