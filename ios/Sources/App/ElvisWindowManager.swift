import SwiftUI
import UIKit
import Darwin

@_silgen_name("pscalRuntimeDebugLog")
private func c_pscalRuntimeDebugLog(_ message: UnsafePointer<CChar>) -> Void

private func elvisRuntimeLog(_ message: String) {
    message.withCString { c_pscalRuntimeDebugLog($0) }
}

final class ElvisWindowManager {
    static let shared = ElvisWindowManager()
    static let activityType = "com.pscal.elvis.scene"

    static var externalWindowEnabled: Bool {
        return TerminalFontSettings.shared.elvisWindowEnabled
    }

    private var preferenceObserver: NSObjectProtocol?
    private var activeSession: UISceneSession?
    private weak var controller: TerminalElvisViewController?
    private weak var mainSession: UISceneSession?
    private var pendingSceneActivity: NSUserActivity?

    private init() {
        preferenceObserver = NotificationCenter.default.addObserver(forName: TerminalFontSettings.preferencesDidChangeNotification,
                                                                    object: nil,
                                                                    queue: .main) { [weak self] _ in
            self?.applyPreferenceChange()
        }
    }

    deinit {
        if let observer = preferenceObserver {
            NotificationCenter.default.removeObserver(observer)
        }
    }

    var isVisible: Bool {
        guard ElvisWindowManager.externalWindowEnabled else { return false }
        return controller != nil || pendingSceneActivity != nil
    }

    func showWindow() {
        guard ElvisWindowManager.externalWindowEnabled else { return }
        DispatchQueue.main.async {
            guard self.activeSession == nil, self.pendingSceneActivity == nil else { return }
            let activity = NSUserActivity(activityType: ElvisWindowManager.activityType)
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
        guard ElvisWindowManager.externalWindowEnabled else { return }
        DispatchQueue.main.async {
            if let session = self.activeSession {
                UIApplication.shared.requestSceneSessionDestruction(session, options: nil, errorHandler: nil)
                self.activeSession = nil
                self.controller = nil
                if let mainSession = self.mainSession {
                    UIApplication.shared.requestSceneSessionActivation(mainSession,
                                                                       userActivity: nil,
                                                                       options: nil,
                                                                       errorHandler: nil)
                }
            } else {
                self.pendingSceneActivity = nil
            }
        }
    }

    func refreshWindow() {
        guard ElvisWindowManager.externalWindowEnabled else { return }
    }

    func sceneDidConnect(session: UISceneSession, controller: TerminalElvisViewController) {
        guard ElvisWindowManager.externalWindowEnabled else { return }
        DispatchQueue.main.async {
            self.activeSession = session
            self.controller = controller
            self.pendingSceneActivity = nil
        }
    }

    func sceneDidDisconnect(session: UISceneSession) {
        guard ElvisWindowManager.externalWindowEnabled else { return }
        DispatchQueue.main.async {
            if self.activeSession == session {
                self.activeSession = nil
                self.controller = nil
                self.pendingSceneActivity = nil
                if let mainSession = self.mainSession {
                    UIApplication.shared.requestSceneSessionActivation(mainSession, userActivity: nil, options: nil, errorHandler: nil)
                }
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

    private func applyPreferenceChange() {
        if ElvisWindowManager.externalWindowEnabled {
            if ElvisTerminalBridge.shared.isActive {
                showWindow()
                refreshWindow()
            }
        } else {
            hideWindow()
        }
    }
}

class PscalAppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     configurationForConnecting connectingSceneSession: UISceneSession,
                     options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        let config = UISceneConfiguration(name: "Default Configuration", sessionRole: connectingSceneSession.role)
        let hasGwinActivity = options.userActivities.contains { $0.activityType == GwinWindowManager.activityType } ||
            connectingSceneSession.stateRestorationActivity?.activityType == GwinWindowManager.activityType
        let hasElvisActivity = options.userActivities.contains { $0.activityType == ElvisWindowManager.activityType } ||
            connectingSceneSession.stateRestorationActivity?.activityType == ElvisWindowManager.activityType
        if hasGwinActivity {
            config.delegateClass = GwinSceneDelegate.self
        } else if hasElvisActivity {
            config.delegateClass = ElvisSceneDelegate.self
        } else {
            config.delegateClass = MainSceneDelegate.self
        }
        return config
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
        window.rootViewController = UIHostingController(rootView: TerminalView())
        self.window = window
        window.makeKeyAndVisible()
        ElvisWindowManager.shared.mainSceneDidConnect(session: windowScene.session)
    }

    func sceneDidDisconnect(_ scene: UIScene) {
        ElvisWindowManager.shared.mainSceneDidDisconnect(session: scene.session)
        window = nil
    }
}

@objc(ElvisSceneDelegate)
class ElvisSceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?

    func scene(_ scene: UIScene,
               willConnectTo session: UISceneSession,
               options connectionOptions: UIScene.ConnectionOptions) {
        guard let windowScene = scene as? UIWindowScene else { return }
        let controller = TerminalElvisViewController()
        let window = UIWindow(windowScene: windowScene)
        window.rootViewController = controller
        self.window = window
        window.makeKeyAndVisible()
        ElvisWindowManager.shared.sceneDidConnect(session: windowScene.session, controller: controller)
    }

    func sceneDidDisconnect(_ scene: UIScene) {
        ElvisWindowManager.shared.sceneDidDisconnect(session: scene.session)
        window = nil
    }
}

private enum ElvisFontMetrics {
    static func characterSize(for font: UIFont) -> CGSize {
        let width = max(1, ("W" as NSString).size(withAttributes: [.font: font]).width)
        let height = max(1, font.lineHeight)
        return CGSize(width: width, height: height)
    }

    static func metrics(for bounds: CGRect,
                        safeInsets: UIEdgeInsets,
                        font: UIFont) -> (TerminalGeometryMetrics, CGSize) {
        let safeBounds = bounds.inset(by: safeInsets)
        let charSize = characterSize(for: font)
        let usableWidth = max(0, safeBounds.width)
        let usableHeight = max(0, safeBounds.height)
        let rawColumns = Int(floor(usableWidth / charSize.width))
        let rawRows = Int(floor(usableHeight / charSize.height))
        let metrics = TerminalGeometryMetrics(columns: max(10, rawColumns),
                                              rows: max(4, rawRows))
        return (metrics, charSize)
    }
}

final class TerminalElvisViewController: UIViewController {
    private let hostingController = UIHostingController(rootView: ElvisFloatingRendererView())
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

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        reportGeometryIfNeeded()
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

    private func reportGeometryIfNeeded() {
        guard view.window != nil else {
            return
        }
        let bounds = view.bounds
        let safeInsets = view.safeAreaInsets
        let safeBounds = bounds.inset(by: safeInsets)
        guard safeBounds.width > 0, safeBounds.height > 0 else { return }
        let font = TerminalElvisViewController.buildPreferredElvisFont(for: traitCollection)
        let (metrics, charSize) = ElvisFontMetrics.metrics(for: bounds,
                                                           safeInsets: safeInsets,
                                                           font: font)
        if metrics == lastReportedMetrics {
            return
        }
        lastReportedMetrics = metrics
        elvisRuntimeLog(String(format: "[ElvisScene] reporting size %.1fx%.1f -> rows=%d cols=%d font=%@ %.2fpt char(%.2f x %.2f)",
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

    private static func buildPreferredElvisFont(for traits: UITraitCollection) -> UIFont {
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
