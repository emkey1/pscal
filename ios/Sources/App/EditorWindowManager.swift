import SwiftUI
import UIKit
import Darwin

private func editorRuntimeLog(_ message: String) {
    #if DEBUG
    message.withCString { ptr in
        pscalRuntimeDebugLog(ptr)
    }
    #else
    _ = message
    #endif
}

final class EditorWindowManager {
    static let shared = EditorWindowManager()
    static let activityType = "com.pscal.editor.scene"

    static var externalWindowEnabled: Bool {
        return TerminalFontSettings.elvisWindowBuildEnabled &&
        TerminalFontSettings.shared.elvisWindowEnabled
    }

    private var preferenceObserver: NSObjectProtocol?
    private var activeSession: UISceneSession?
    private weak var controller: TerminalEditorViewController?
    private weak var mainSession: UISceneSession?
    private var pendingSceneActivity: NSUserActivity?

    private init() {
        preferenceObserver = NotificationCenter.default.addObserver(forName: TerminalFontSettings.preferencesDidChangeNotification,
                                                                    object: nil,
                                                                    queue: .main) { [weak self] _ in
            Task { @MainActor in
                self?.applyPreferenceChange()
            }
        }
    }

    deinit {
        if let observer = preferenceObserver {
            NotificationCenter.default.removeObserver(observer)
        }
    }

    var isVisible: Bool {
        guard EditorWindowManager.externalWindowEnabled else { return false }
        return controller != nil || pendingSceneActivity != nil
    }

    func showWindow() {
        guard EditorWindowManager.externalWindowEnabled else { return }
        DispatchQueue.main.async {
            guard self.activeSession == nil, self.pendingSceneActivity == nil else { return }
            let activity = NSUserActivity(activityType: EditorWindowManager.activityType)
            self.pendingSceneActivity = activity
            UIApplication.shared.requestSceneSessionActivation(nil,
                                                               userActivity: activity,
                                                               options: nil) { _ in
                DispatchQueue.main.async {
                    if self.pendingSceneActivity === activity {
                        self.pendingSceneActivity = nil
                    }
                }
            }
        }
    }

    func hideWindow() {
        guard EditorWindowManager.externalWindowEnabled else { return }
        DispatchQueue.main.async {
            if let session = self.activeSession {
                UIApplication.shared.requestSceneSessionDestruction(session, options: nil, errorHandler: nil)
                self.activeSession = nil
                self.controller = nil
                self.activateMainSceneIfAvailable()
            } else {
                self.pendingSceneActivity = nil
                self.activateMainSceneIfAvailable()
            }
        }
    }

    func refreshWindow() {
        guard EditorWindowManager.externalWindowEnabled else { return }
    }

    func sceneDidConnect(session: UISceneSession, controller: TerminalEditorViewController) {
        guard EditorWindowManager.externalWindowEnabled else { return }
        DispatchQueue.main.async {
            self.activeSession = session
            self.controller = controller
            self.pendingSceneActivity = nil
            // Keep the main session as-is; do not juggle activation to avoid
            // backgrounding other windows when the Elvis scene appears/disappears.
        }
    }

    func sceneDidDisconnect(session: UISceneSession) {
        guard EditorWindowManager.externalWindowEnabled else { return }
        DispatchQueue.main.async {
            if self.activeSession == session {
                self.activeSession = nil
                self.controller = nil
                self.pendingSceneActivity = nil
                self.activateMainSceneIfAvailable()
            }
        }
    }

    func mainSceneDidConnect(session: UISceneSession) {
        mainSession = session
    }

    func mainSceneDidDisconnect(session: UISceneSession) {
        if mainSession == session {
            mainSession = nil
        }
    }

    private func activateMainSceneIfAvailable() {
        guard let session = mainSession else { return }
        UIApplication.shared.requestSceneSessionActivation(session,
                                                           userActivity: nil,
                                                           options: nil,
                                                           errorHandler: nil)
    }

    @MainActor
    private func applyPreferenceChange() {
        if EditorWindowManager.externalWindowEnabled {
            if activeEditorBridge()?.isActive == true {
                showWindow()
                refreshWindow()
            }
        } else {
            hideWindow()
        }
    }

    @MainActor
    private func activeEditorBridge() -> EditorTerminalBridge? {
        let manager = TerminalTabManager.shared
        for tab in manager.tabs {
            if case .shell(let runtime) = tab.kind, runtime.editorBridge.isActive {
                return runtime.editorBridge
            }
        }
        if case .shell(let runtime) = manager.selectedTab.kind {
            return runtime.editorBridge
        }
        return nil
    }
}

class PscalAppDelegate: NSObject, UIApplicationDelegate {
    static var lockedOrientationMask: UIInterfaceOrientationMask?
    static var activeOrientationMask: UIInterfaceOrientationMask {
        lockedOrientationMask ?? defaultOrientationMask
    }

    private static var defaultOrientationMask: UIInterfaceOrientationMask {
        UIDevice.current.userInterfaceIdiom == .pad ? .all : .allButUpsideDown
    }

    func application(_ application: UIApplication,
                     configurationForConnecting connectingSceneSession: UISceneSession,
                     options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        let config = UISceneConfiguration(name: "Default Configuration", sessionRole: connectingSceneSession.role)
        let hasGwinActivity = options.userActivities.contains { $0.activityType == GwinWindowManager.activityType } ||
            connectingSceneSession.stateRestorationActivity?.activityType == GwinWindowManager.activityType
        let hasEditorActivity = options.userActivities.contains { $0.activityType == EditorWindowManager.activityType } ||
            connectingSceneSession.stateRestorationActivity?.activityType == EditorWindowManager.activityType
        if hasGwinActivity {
            config.delegateClass = GwinSceneDelegate.self
        } else if hasEditorActivity {
            config.delegateClass = EditorSceneDelegate.self
        } else {
            config.delegateClass = MainSceneDelegate.self
        }
        return config
    }

    func application(_ application: UIApplication, supportedInterfaceOrientationsFor window: UIWindow?) -> UIInterfaceOrientationMask {
        Self.activeOrientationMask
    }
}

@objc(MainSceneDelegate)
class MainSceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?

    func scene(_ scene: UIScene,
               willConnectTo session: UISceneSession,
               options connectionOptions: UIScene.ConnectionOptions) {
        guard let windowScene = scene as? UIWindowScene else { return }

        let window = UIWindow(windowScene: windowScene)
        let root = TerminalRootViewController()
        window.rootViewController = root
        self.window = window
        window.makeKeyAndVisible()

        if #available(iOS 16.0, *) {
            windowScene.requestGeometryUpdate(.iOS(interfaceOrientations: PscalAppDelegate.activeOrientationMask)) { _ in }
        }

        EditorWindowManager.shared.mainSceneDidConnect(session: windowScene.session)
    }

    func sceneDidDisconnect(_ scene: UIScene) {
        EditorWindowManager.shared.mainSceneDidDisconnect(session: scene.session)
        window = nil
    }
}

@objc(EditorSceneDelegate)
class EditorSceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?

    func scene(_ scene: UIScene,
               willConnectTo session: UISceneSession,
               options connectionOptions: UIScene.ConnectionOptions) {
        guard let windowScene = scene as? UIWindowScene else { return }
        let controller = TerminalEditorViewController()
        let window = UIWindow(windowScene: windowScene)
        window.rootViewController = controller
        self.window = window
        window.makeKeyAndVisible()
        EditorWindowManager.shared.sceneDidConnect(session: windowScene.session, controller: controller)
    }

    func sceneDidDisconnect(_ scene: UIScene) {
        EditorWindowManager.shared.sceneDidDisconnect(session: scene.session)
        window = nil
    }
}

private enum EditorFontMetrics {
    static func characterSize(for font: UIFont) -> CGSize {
        let char = TerminalGeometryCalculator.characterMetrics(for: font)
        return CGSize(width: char.width, height: char.lineHeight)
    }

    static func metrics(for bounds: CGRect,
                        safeInsets: UIEdgeInsets,
                        font: UIFont) -> (TerminalGeometryMetrics, CGSize) {
        let usableWidth = bounds.width - safeInsets.left - safeInsets.right
        let usableHeight = bounds.height - safeInsets.top - safeInsets.bottom
        let usableSize = CGSize(width: usableWidth, height: usableHeight)

        let grid = TerminalGeometryCalculator.calculateCapacity(
            for: usableSize,
            font: font
        )
        let metrics = TerminalGeometryMetrics(columns: grid.columns, rows: grid.rows)
        let charSize = characterSize(for: font)
        return (metrics, charSize)
    }
}

final class TerminalEditorViewController: UIViewController {
    private let hostingController = UIHostingController(rootView: EditorFloatingRendererView())
    private let inputViewBridge = TerminalKeyInputView()
    private var lastReportedMetrics: TerminalGeometryMetrics?
    private var fontObserver: NSObjectProtocol?

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = TerminalFontSettings.shared.backgroundColor

        addChild(hostingController)
        hostingController.view.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(hostingController.view)
        NSLayoutConstraint.activate([
            hostingController.view.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            hostingController.view.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            hostingController.view.topAnchor.constraint(equalTo: view.topAnchor),
            hostingController.view.bottomAnchor.constraint(equalTo: view.bottomAnchor)
        ])
        hostingController.didMove(toParent: self)

        configureInputBridge()
        fontObserver = NotificationCenter.default.addObserver(forName: TerminalFontSettings.appearanceDidChangeNotification,
                                                              object: nil,
                                                              queue: .main) { [weak self] _ in
            self?.reportGeometryIfNeeded()
        }
    }

    deinit {
        if let observer = fontObserver {
            NotificationCenter.default.removeObserver(observer)
        }
    }

    override func viewSafeAreaInsetsDidChange() {
        super.viewSafeAreaInsetsDidChange()
        reportGeometryIfNeeded()
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        reportGeometryIfNeeded()
    }

    override func traitCollectionDidChange(_ previousTraitCollection: UITraitCollection?) {
        super.traitCollectionDidChange(previousTraitCollection)
        if previousTraitCollection?.preferredContentSizeCategory != traitCollection.preferredContentSizeCategory {
            reportGeometryIfNeeded()
        }
    }

    private func configureInputBridge() {
        inputViewBridge.backgroundColor = .clear
        inputViewBridge.textColor = .clear
        inputViewBridge.tintColor = .clear
        inputViewBridge.isScrollEnabled = false
        inputViewBridge.text = ""
        inputViewBridge.autocorrectionType = .no
        inputViewBridge.autocapitalizationType = .none
        inputViewBridge.spellCheckingType = .no
        inputViewBridge.smartQuotesType = .no
        inputViewBridge.smartInsertDeleteType = .no
        inputViewBridge.smartDashesType = .no
        inputViewBridge.keyboardAppearance = .dark
        inputViewBridge.onInput = { text in
            PscalRuntimeBootstrap.shared.send(text)
        }
        inputViewBridge.inputAssistantItem.leadingBarButtonGroups = []
        inputViewBridge.inputAssistantItem.trailingBarButtonGroups = []
        inputViewBridge.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(inputViewBridge)
        inputViewBridge.widthAnchor.constraint(equalToConstant: 1).isActive = true
        inputViewBridge.heightAnchor.constraint(equalToConstant: 1).isActive = true
        inputViewBridge.bottomAnchor.constraint(equalTo: view.bottomAnchor).isActive = true
        inputViewBridge.leadingAnchor.constraint(equalTo: view.leadingAnchor).isActive = true

        DispatchQueue.main.async {
            self.inputViewBridge.becomeFirstResponder()
        }
    }

    func resignInputFocus() {
        DispatchQueue.main.async {
            self.inputViewBridge.resignFirstResponder()
        }
    }

    private func reportGeometryIfNeeded() {
        guard view.window != nil else {
            return
        }
        let bounds = view.bounds
        let safeInsets = view.safeAreaInsets
        let safeBounds = bounds.inset(by: safeInsets)
        guard safeBounds.width > 0, safeBounds.height > 0 else { return }
        let font = TerminalEditorViewController.buildPreferredEditorFont(for: traitCollection)
        let (metrics, charSize) = EditorFontMetrics.metrics(for: bounds,
                                                           safeInsets: safeInsets,
                                                           font: font)
        if metrics == lastReportedMetrics {
            return
        }
        lastReportedMetrics = metrics
        editorRuntimeLog(String(format: "[EditorScene] reporting size %.1fx%.1f -> rows=%d cols=%d font=%@ %.2fpt char(%.2f x %.2f)",
                               safeBounds.width,
                               safeBounds.height,
                               metrics.rows,
                               metrics.columns,
                               font.fontName,
                               font.pointSize,
                               charSize.width,
                               charSize.height))
        PscalRuntimeBootstrap.shared.updateElvisWindowSize(columns: metrics.columns, rows: metrics.rows)
    }

    private static func buildPreferredEditorFont(for traits: UITraitCollection) -> UIFont {
        let pointSize = resolvedFontPointSize(for: traits)
        return TerminalFontSettings.shared.font(forPointSize: pointSize)
    }

    private static func resolvedFontPointSize(for traits: UITraitCollection) -> CGFloat {
        if let env = getenv("PSCALI_ELVIS_FONT_SIZE") {
            let value = String(cString: env).trimmingCharacters(in: .whitespacesAndNewlines)
            if value.caseInsensitiveCompare("inherit") == .orderedSame ||
                value.caseInsensitiveCompare("system") == .orderedSame {
                return UIFont.preferredFont(forTextStyle: .body, compatibleWith: traits).pointSize
            }
            if let parsed = Double(value), parsed > 0 {
                return CGFloat(parsed)
            }
        }
        return TerminalFontSettings.shared.pointSize
    }
}

final class GwinWindowManager {
    static let shared = GwinWindowManager()
    static let activityType = "com.pscal.gwin.scene"

    private var activeSession: UISceneSession?
    private weak var controller: GwinViewController?
    private var pendingMessage: String?

    private init() {}

    func showWindow(message: String) {
        DispatchQueue.main.async {
            if let controller = self.controller {
                controller.update(message: message)
                return
            }
            self.pendingMessage = message
            let activity = NSUserActivity(activityType: Self.activityType)
            activity.userInfo = ["message": message]
            UIApplication.shared.requestSceneSessionActivation(
                nil,
                userActivity: activity,
                options: nil
            ) { error in
                NSLog("GwinWindowManager: failed to activate scene: %@", error.localizedDescription)
            }
        }
    }

    func sceneDidConnect(session: UISceneSession,
                         controller: GwinViewController,
                         initialMessage: String?) {
        DispatchQueue.main.async {
            self.activeSession = session
            self.controller = controller
            let message = initialMessage ?? self.pendingMessage ?? "gwin for the win"
            self.pendingMessage = nil
            controller.update(message: message)
        }
    }

    func sceneDidDisconnect(session: UISceneSession) {
        DispatchQueue.main.async {
            if self.activeSession == session {
                self.activeSession = nil
                self.controller = nil
                self.pendingMessage = nil
            }
        }
    }

    func hideWindow() {
        DispatchQueue.main.async {
            guard let session = self.activeSession else {
                self.pendingMessage = nil
                return
            }
            UIApplication.shared.requestSceneSessionDestruction(session, options: nil, errorHandler: nil)
            self.activeSession = nil
            self.controller = nil
            self.pendingMessage = nil
        }
    }
}

@objc(GwinSceneDelegate)
class GwinSceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?

    func scene(_ scene: UIScene,
               willConnectTo session: UISceneSession,
               options connectionOptions: UIScene.ConnectionOptions) {
        guard let windowScene = scene as? UIWindowScene else { return }
        let controller = GwinViewController()
        let activity = (connectionOptions.userActivities.first { $0.activityType == GwinWindowManager.activityType })
            ?? session.stateRestorationActivity
        if let message = activity?.userInfo?["message"] as? String {
            controller.update(message: message)
        }
        let window = UIWindow(windowScene: windowScene)
        window.rootViewController = controller
        self.window = window
        window.makeKeyAndVisible()
        GwinWindowManager.shared.sceneDidConnect(session: windowScene.session,
                                                 controller: controller,
                                                 initialMessage: controller.currentMessage)
    }

    func stateRestorationActivity(for scene: UIScene) -> NSUserActivity? {
        let activity = NSUserActivity(activityType: GwinWindowManager.activityType)
        if let controller = window?.rootViewController as? GwinViewController {
            activity.userInfo = ["message": controller.currentMessage ?? "gwin for the win"]
        }
        return activity
    }

    func sceneDidDisconnect(_ scene: UIScene) {
        GwinWindowManager.shared.sceneDidDisconnect(session: scene.session)
        window = nil
    }
}

final class GwinViewController: UIViewController {
    private let messageLabel = UILabel()
    private(set) var currentMessage: String?
    private lazy var closeCommands: [UIKeyCommand] = {
        [
            UIKeyCommand(input: "q", modifierFlags: [], action: #selector(handleCloseCommand)),
            UIKeyCommand(input: "Q", modifierFlags: [], action: #selector(handleCloseCommand))
        ]
    }()

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = UIColor.systemBackground
        messageLabel.translatesAutoresizingMaskIntoConstraints = false
        messageLabel.numberOfLines = 0
        messageLabel.textAlignment = .center
        messageLabel.font = UIFont.preferredFont(forTextStyle: .title2)
        messageLabel.textColor = UIColor.label
        view.addSubview(messageLabel)
        NSLayoutConstraint.activate([
            messageLabel.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 24),
            messageLabel.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -24),
            messageLabel.centerYAnchor.constraint(equalTo: view.centerYAnchor)
        ])
        messageLabel.text = currentMessage ?? "gwin for the win"
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        becomeFirstResponder()
    }

    override var canBecomeFirstResponder: Bool { true }

    override var keyCommands: [UIKeyCommand]? {
        closeCommands
    }

    @objc private func handleCloseCommand() {
        GwinWindowManager.shared.hideWindow()
    }

    func update(message: String) {
        currentMessage = message
        if isViewLoaded {
            messageLabel.text = message
        }
    }
}

@_cdecl("pscalShowGwinMessage")
func pscalShowGwinMessage(_ text: UnsafePointer<CChar>?) {
    let message = text.flatMap { String(cString: $0) } ?? "gwin for the win"
    GwinWindowManager.shared.showWindow(message: message)
}
