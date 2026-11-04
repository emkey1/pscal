// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceViewController.m at 0f75fee1c) and
// licensed to PSCAL under the Apache License 2.0.

import QuartzCore
import UIKit
import WebKit

/// The Browser's home page, bookmarks and pinned tabs, which all its windows share. An address
/// is a URL's text, or the shell's path for a local file: a real path holds the app's container,
/// which moves when the app is updated.
private enum WorkspaceBrowserStore {
    static let defaultAddress = "https://duckduckgo.com/"

    private static let homeKey = "com.pscal.workspace.browser.home"
    private static let bookmarksKey = "com.pscal.workspace.browser.bookmarks"
    private static let pinnedTabsKey = "com.pscal.workspace.browser.pinnedTabs"

    static var homeAddress: String {
        let stored = UserDefaults.standard.string(forKey: homeKey)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return stored.isEmpty ? defaultAddress : stored
    }

    static func setHomeAddress(_ address: String?) {
        let trimmed = address?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        if trimmed.isEmpty || trimmed == defaultAddress {
            UserDefaults.standard.removeObject(forKey: homeKey)
        } else {
            UserDefaults.standard.set(trimmed, forKey: homeKey)
        }
    }

    static var bookmarks: [[String: String]] {
        UserDefaults.standard.array(forKey: bookmarksKey) as? [[String: String]] ?? []
    }

    static func isBookmarked(_ address: String?) -> Bool {
        guard let address, !address.isEmpty else { return false }
        return bookmarks.contains { $0["url"] == address }
    }

    static func addBookmark(title: String?, address: String) {
        guard !address.isEmpty, !isBookmarked(address) else { return }
        let name = title.flatMap { $0.isEmpty ? nil : $0 } ?? address
        UserDefaults.standard.set(bookmarks + [["title": name, "url": address]], forKey: bookmarksKey)
    }

    static func removeBookmark(address: String) {
        UserDefaults.standard.set(bookmarks.filter { $0["url"] != address }, forKey: bookmarksKey)
    }

    // Pinned tabs are keyed by tab position, not by address: pinning tab 3 to a page means a
    // new tab landing in slot 3 loads that page. It doesn't touch a tab already open there, and
    // unpinning only removes the record, so new tabs there go back to the home page.
    private static var pinnedTabs: [String: [String: String]] {
        UserDefaults.standard.dictionary(forKey: pinnedTabsKey) as? [String: [String: String]] ?? [:]
    }

    static func pinnedTab(at index: Int) -> (title: String, address: String)? {
        guard let record = pinnedTabs[String(index)], let address = record["url"], !address.isEmpty else { return nil }
        return (record["title"] ?? address, address)
    }

    static func pinTab(at index: Int, title: String?, address: String) {
        guard !address.isEmpty else { return }
        var records = pinnedTabs
        records[String(index)] = ["title": title.flatMap { $0.isEmpty ? nil : $0 } ?? address, "url": address]
        UserDefaults.standard.set(records, forKey: pinnedTabsKey)
    }

    static func unpinTab(at index: Int) {
        var records = pinnedTabs
        records[String(index)] = nil
        UserDefaults.standard.set(records, forKey: pinnedTabsKey)
    }
}

/// How many tabs this device gets, from its memory: 4, 6, 10 and 18 at under 4, 4, 8 and 16 GB.
private enum WorkspaceBrowserLimits {
    private static let oneGB: UInt64 = 1_000_000_000

    static var physicalGB: Double {
        Double(ProcessInfo.processInfo.physicalMemory) / Double(oneGB)
    }

    private static var marketedGB: UInt64 {
        (ProcessInfo.processInfo.physicalMemory + oneGB - 1) / oneGB
    }

    static var hasLowMemory: Bool { marketedGB < 4 }

    static var maximumTabCount: Int {
        marketedGB < 4 ? 4 : Int(2 * (3 + (marketedGB - 4) / 2))
    }
}

/// What the address field and `ws open` mean by what they're given, and what a local page may read.
private enum WorkspaceBrowserAddress {
    enum Target {
        case web(URL)
        case localFile(String)
        case otherScheme(URL)
        case invalid
    }

    static func target(for input: String) -> Target {
        var trimmed = input.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty {
            trimmed = WorkspaceBrowserStore.defaultAddress
        }
        if trimmed.hasPrefix("/") {
            return .localFile(trimmed)
        }
        let lower = trimmed.lowercased()
        let explicit = lower.hasPrefix("http://") || lower.hasPrefix("https://") || lower.hasPrefix("file://")
        if !explicit && trimmed.rangeOfCharacter(from: .whitespacesAndNewlines) != nil {
            return searchTarget(trimmed)
        }
        if let url = parsed(trimmed), let scheme = url.scheme?.lowercased(), !isHostAndPort(trimmed, scheme: scheme) {
            switch scheme {
            case "http", "https":
                return .web(url)
            case "file":
                return .localFile(WorkspaceFileService.shownPath(url.path))
            case "about":
                return url.absoluteString == "about:blank" ? .web(url) : .invalid
            case "javascript", "data", "blob":
                return .invalid
            default:
                return .otherScheme(url)
            }
        }
        return parsed((isLocalHost(trimmed) ? "http://" : "https://") + trimmed).map(Target.web) ?? .invalid
    }

    private static func searchTarget(_ text: String) -> Target {
        var allowed = CharacterSet.urlQueryAllowed
        allowed.remove(charactersIn: "&+=")
        let query = text.addingPercentEncoding(withAllowedCharacters: allowed) ?? ""
        return URL(string: "https://duckduckgo.com/?q=\(query)").map(Target.web) ?? .invalid
    }

    /// Before iOS 17 a URL can't hold a space or a non-ASCII character unescaped.
    private static func parsed(_ text: String) -> URL? {
        if let url = URL(string: text) {
            return url
        }
        var allowed = CharacterSet.urlFragmentAllowed
        allowed.insert(charactersIn: "#%")
        return text.addingPercentEncoding(withAllowedCharacters: allowed).flatMap(URL.init(string:))
    }

    /// `example.com:8080/x` parses as the scheme `example.com`, but it's an address.
    private static func isHostAndPort(_ text: String, scheme: String) -> Bool {
        guard scheme.contains(".") || scheme == "localhost" else { return false }
        return text.dropFirst(scheme.count + 1).first?.isNumber == true
    }

    private static func isLocalHost(_ text: String) -> Bool {
        ["localhost", "127.", "10.", "192.168.", "172.16.", "172.17.", "172.18.", "172.19.", "172.2", "172.30.", "172.31."]
            .contains { text.hasPrefix($0) }
    }

    /// What the person sees and what gets saved: the shell's path for a local page.
    static func display(_ url: URL) -> String {
        url.isFileURL ? WorkspaceFileService.shownPath(url.path) : url.absoluteString
    }

    /// By text alone, as this runs on the main thread: without `.` and `..`, and without the
    /// alias that WebKit may or may not report a real path under.
    private static func comparablePath(_ path: String) -> String {
        var clean = URL(fileURLWithPath: path, isDirectory: false).standardized.path
        if clean.hasPrefix("/private/") {
            clean.removeFirst("/private".count)
        }
        return clean
    }

    static func url(_ url: URL, isInside scope: URL) -> Bool {
        let path = comparablePath(url.path)
        let container = comparablePath(scope.path)
        return path == container || path.hasPrefix(container + "/")
    }

    /// What a page opened from `file` may read: its folder, so its relative links, styles and
    /// images work, and nothing else in the app. A page sitting at the top of the app's tree
    /// gets only itself, since its folder is the whole tree, keys and all. Runs off the main
    /// thread.
    static func readScope(for file: URL) -> URL {
        let folder = file.deletingLastPathComponent()
        let path = comparablePath(folder.path)
        guard path != "/" else { return file }
        var roots = [NSHomeDirectory(), WorkspaceFileService.hostPath("/")]
        if let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first {
            roots.append(documents.path)
            roots.append(documents.appendingPathComponent("home", isDirectory: true).path)
        }
        let holdsRoot = roots.map(comparablePath).contains { $0 == path || $0.hasPrefix(path + "/") }
        return holdsRoot ? file : folder
    }
}

/// One tab: its web view, and what the applet knows about it that the web view doesn't.
@MainActor
private final class WorkspaceBrowserTab {
    let webView: WKWebView
    var observations: [NSKeyValueObservation] = []
    /// What the tab was last asked to show, until a page commits. It stays after a failure, so
    /// the address field says what failed and Reload can try it again.
    var requestedAddress: String?
    var statusMessage: String?
    /// What this tab's local pages may read: what was opened in it.
    var readScopes: [URL] = []
    /// A local load the applet has just asked for, which the policy check lets through.
    var ownLocalLoad: URL?
    /// Each load the applet starts bumps this, which discards a late answer about an older one.
    var loadGeneration = 0

    init(webView: WKWebView) {
        self.webView = webView
    }

    func readScope(containing url: URL) -> URL? {
        readScopes.first { WorkspaceBrowserAddress.url(url, isInside: $0) }
    }
}

/// A web browser with tabs, bookmarks and a home page, for http, https and local files.
///
/// Anything else a page links to leaves through `UIApplication.open`, only after a tap on the
/// link and the person's yes. A page's own script can do neither.
@MainActor
final class WorkspaceBrowserAppletViewController: WorkspaceAppletViewController, WorkspaceFileOpenable, WorkspaceStatefulTool, WorkspaceTextScalable, WorkspaceFocusable, UITextFieldDelegate, UIGestureRecognizerDelegate, WKNavigationDelegate, WKUIDelegate {
    private static let fileQueue = DispatchQueue(label: "com.pscal.workspace.browser.files", qos: .userInitiated)
    /// How long after a tap a link's navigation can still be that tap's. WebKit answers late
    /// when a page's main thread is busy.
    private static let tapWindow: CFTimeInterval = 2

    private lazy var toolbarCard = themeCardView()
    private lazy var browserCard = themeCardView()
    private let addressContainer = UIView()
    private let addressField = UITextField()
    private let tabsScrollView = UIScrollView()
    private let tabsStack = UIStackView()
    private let progressView = UIProgressView(progressViewStyle: .default)
    private let statusView = UIView()
    private let statusLabel = UILabel()

    private let backButton = UIButton(type: .system)
    private let forwardButton = UIButton(type: .system)
    private let reloadButton = UIButton(type: .system)
    private let homeButton = UIButton(type: .system)
    private let bookmarkButton = UIButton(type: .system)
    private let goButton = UIButton(type: .system)
    private let addTabButton = UIButton(type: .system)
    private let closeTabButton = UIButton(type: .system)
    private var actionButtons: [UIButton] {
        [backButton, forwardButton, reloadButton, homeButton, bookmarkButton, goButton, addTabButton, closeTabButton]
    }
    private var tabButtons: [UIButton] = []

    private var tabs: [WorkspaceBrowserTab] = []
    private var selectedTabIndex = 0
    private weak var attachedWebView: WKWebView?
    private var maximumTabCount = WorkspaceBrowserLimits.maximumTabCount
    private var isWindowHidden = false
    private var desktopObserver: NSObjectProtocol?
    private var lastPageTap: CFTimeInterval = 0
    private var refusedURL: URL?
    private var externalPromptURL: URL?
    private weak var ownPresentation: UIViewController?

    deinit {
        if let desktopObserver {
            NotificationCenter.default.removeObserver(desktopObserver)
        }
    }

    private var currentTab: WorkspaceBrowserTab? {
        tabs.indices.contains(selectedTabIndex) ? tabs[selectedTabIndex] : nil
    }

    private func tab(for webView: WKWebView) -> WorkspaceBrowserTab? {
        tabs.first { $0.webView === webView }
    }

    // MARK: - Testing

    /// The current tab's local file as the shell sees it, if that's what it shows.
    var currentPathForTesting: String? {
        guard let tab = currentTab else { return nil }
        if let url = tab.webView.url, url.isFileURL, tab.requestedAddress == nil {
            return WorkspaceBrowserAddress.display(url)
        }
        return tab.requestedAddress.flatMap { $0.hasPrefix("/") ? $0 : nil }
    }
    var currentAddressForTesting: String? { addressField.text }
    var statusMessageForTesting: String? { currentTab?.statusMessage }
    var pageTitleForTesting: String? { currentTab?.webView.title }
    var tabCountForTesting: Int { tabs.count }
    var maximumTabCountForTesting: Int { maximumTabCount }
    var selectedTabIndexForTesting: Int { selectedTabIndex }
    var isLoadingForTesting: Bool { currentTab?.webView.isLoading ?? false }
    /// Whether this window is on another Desktop, its media suspended and its page out of the
    /// view tree.
    var isMediaSuspendedForTesting: Bool { isWindowHidden }
    var isWebViewAttachedForTesting: Bool { currentTab?.webView.superview != nil }
    var readScopeForTesting: String? { currentTab?.readScopes.last?.path }
    /// The last address of another kind that a page tried to open without a tap.
    var refusedURLForTesting: String? { refusedURL?.absoluteString }
    /// The address in the "Open in another app?" question, while it's showing.
    var externalPromptURLForTesting: String? { externalPromptURL?.absoluteString }

    // MARK: - Building

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "Browser"
        buildInterface()
        addTab(activate: true)
        desktopObserver = NotificationCenter.default.addObserver(forName: .workspaceDesktopsDidChange, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.scheduleVisibilityCheck()
            }
        }
        // On the next turn: a file or a saved page may be arriving right after this, and
        // neither should cost a request to the home page first.
        DispatchQueue.main.async { [weak self] in
            guard let self, let tab = self.currentTab, tab.loadGeneration == 0 else { return }
            self.load(self.addressForNewTab(at: self.selectedTabIndex), in: tab, byPerson: false)
        }
    }

    private func configure(_ button: UIButton, title: String, phoneWidth: CGFloat, padWidth: CGFloat, label: String, hint: String, identifier: String, action: Selector) {
        let phone = WorkspaceMetrics.usesPhoneLayout
        button.translatesAutoresizingMaskIntoConstraints = false
        button.layer.cornerRadius = 10
        button.layer.borderWidth = 1
        button.titleLabel?.font = .systemFont(ofSize: WorkspaceTheme.fontSize(.footnote), weight: .semibold)
        button.setTitle(title, for: .normal)
        button.addTarget(self, action: action, for: .touchUpInside)
        button.accessibilityLabel = label
        button.accessibilityHint = hint
        button.accessibilityIdentifier = identifier
        NSLayoutConstraint.activate([
            button.widthAnchor.constraint(equalToConstant: phone ? phoneWidth : padWidth),
            button.heightAnchor.constraint(equalToConstant: phone ? 30 : 34),
        ])
    }

    /// Tap does the thing, and touch-and-hold configures it, on Home, Reload and Bookmark alike.
    private func addLongPress(to button: UIView, action: Selector) {
        let press = UILongPressGestureRecognizer(target: self, action: action)
        press.minimumPressDuration = 0.35
        button.addGestureRecognizer(press)
    }

    private func buildInterface() {
        toolContentView.addSubview(toolbarCard)
        toolContentView.addSubview(browserCard)

        let controlsRow = UIView()
        controlsRow.translatesAutoresizingMaskIntoConstraints = false
        toolbarCard.addSubview(controlsRow)

        configure(backButton, title: "<", phoneWidth: 30, padWidth: 34, label: "Back",
                  hint: "Navigates back to the previous page.", identifier: "workspace.browser.back", action: #selector(goBack))
        configure(forwardButton, title: ">", phoneWidth: 30, padWidth: 34, label: "Forward",
                  hint: "Navigates forward to the next page.", identifier: "workspace.browser.forward", action: #selector(goForward))
        configure(reloadButton, title: "R", phoneWidth: 30, padWidth: 34, label: "Reload",
                  hint: "Touch and hold to open the current page in Safari.", identifier: "workspace.browser.reload", action: #selector(reloadOrStop))
        configure(homeButton, title: "Home", phoneWidth: 52, padWidth: 60, label: "Home",
                  hint: "Touch and hold to set the home page.", identifier: "workspace.browser.home", action: #selector(goHome))
        configure(bookmarkButton, title: "☆", phoneWidth: 30, padWidth: 34, label: "Bookmark",
                  hint: "Toggles a bookmark for the current page. Touch and hold to view bookmarks.", identifier: "workspace.browser.bookmark", action: #selector(toggleBookmark))
        configure(goButton, title: "Go", phoneWidth: 42, padWidth: 48, label: "Go",
                  hint: "Loads the entered address.", identifier: "workspace.browser.go", action: #selector(commitAddress))
        configure(addTabButton, title: "+", phoneWidth: 30, padWidth: 34, label: "Add Tab",
                  hint: "Opens a new browser tab.", identifier: "workspace.browser.addTab", action: #selector(addTabTapped))
        configure(closeTabButton, title: "×", phoneWidth: 30, padWidth: 34, label: "Close Tab",
                  hint: "Closes the current browser tab.", identifier: "workspace.browser.closeTab", action: #selector(closeCurrentTab))
        addLongPress(to: homeButton, action: #selector(homeLongPressed(_:)))
        // Some sites reject embedded web views whatever they can do. The fix is handing the
        // page to Safari, not pretending to be it.
        addLongPress(to: reloadButton, action: #selector(reloadLongPressed(_:)))
        addLongPress(to: bookmarkButton, action: #selector(bookmarkLongPressed(_:)))
        [backButton, forwardButton, reloadButton, bookmarkButton, goButton].forEach(controlsRow.addSubview)

        addressContainer.translatesAutoresizingMaskIntoConstraints = false
        addressContainer.layer.cornerRadius = 10
        addressContainer.layer.borderWidth = 1
        controlsRow.addSubview(addressContainer)

        addressField.translatesAutoresizingMaskIntoConstraints = false
        addressField.accessibilityLabel = "Address bar"
        addressField.accessibilityIdentifier = "workspace.browser.address"
        addressField.delegate = self
        addressField.clearButtonMode = .whileEditing
        addressField.returnKeyType = .go
        addressField.autocapitalizationType = .none
        addressField.autocorrectionType = .no
        addressField.spellCheckingType = .no
        addressField.smartDashesType = .no
        addressField.smartQuotesType = .no
        addressField.smartInsertDeleteType = .no
        addressContainer.addSubview(addressField)

        progressView.translatesAutoresizingMaskIntoConstraints = false
        progressView.accessibilityIdentifier = "workspace.browser.progress"
        toolbarCard.addSubview(progressView)

        let tabsRow = UIView()
        tabsRow.translatesAutoresizingMaskIntoConstraints = false
        toolbarCard.addSubview(tabsRow)

        tabsScrollView.translatesAutoresizingMaskIntoConstraints = false
        tabsScrollView.showsHorizontalScrollIndicator = false
        tabsRow.addSubview(tabsScrollView)
        tabsStack.translatesAutoresizingMaskIntoConstraints = false
        tabsStack.axis = .horizontal
        tabsStack.alignment = .center
        tabsStack.spacing = 6
        tabsScrollView.addSubview(tabsStack)
        [homeButton, addTabButton, closeTabButton].forEach(tabsRow.addSubview)

        statusView.translatesAutoresizingMaskIntoConstraints = false
        statusView.isHidden = true
        browserCard.addSubview(statusView)
        statusLabel.translatesAutoresizingMaskIntoConstraints = false
        statusLabel.numberOfLines = 0
        statusLabel.textAlignment = .center
        statusLabel.font = .systemFont(ofSize: 15)
        statusLabel.accessibilityIdentifier = "workspace.browser.status"
        statusView.addSubview(statusLabel)

        let phone = WorkspaceMetrics.usesPhoneLayout
        let inset: CGFloat = phone ? 6 : 8
        let rowHeight: CGFloat = phone ? 30 : 34
        NSLayoutConstraint.activate([
            toolbarCard.topAnchor.constraint(equalTo: toolContentView.topAnchor, constant: inset),
            toolbarCard.leadingAnchor.constraint(equalTo: toolContentView.leadingAnchor, constant: inset),
            toolbarCard.trailingAnchor.constraint(equalTo: toolContentView.trailingAnchor, constant: -inset),

            browserCard.topAnchor.constraint(equalTo: toolbarCard.bottomAnchor, constant: inset),
            browserCard.leadingAnchor.constraint(equalTo: toolContentView.leadingAnchor, constant: inset),
            browserCard.trailingAnchor.constraint(equalTo: toolContentView.trailingAnchor, constant: -inset),
            browserCard.bottomAnchor.constraint(equalTo: toolContentView.bottomAnchor, constant: -inset),

            controlsRow.topAnchor.constraint(equalTo: toolbarCard.topAnchor, constant: inset),
            controlsRow.leadingAnchor.constraint(equalTo: toolbarCard.leadingAnchor, constant: inset),
            controlsRow.trailingAnchor.constraint(equalTo: toolbarCard.trailingAnchor, constant: -inset),

            backButton.leadingAnchor.constraint(equalTo: controlsRow.leadingAnchor),
            backButton.centerYAnchor.constraint(equalTo: addressContainer.centerYAnchor),
            forwardButton.leadingAnchor.constraint(equalTo: backButton.trailingAnchor, constant: 6),
            forwardButton.centerYAnchor.constraint(equalTo: addressContainer.centerYAnchor),
            reloadButton.leadingAnchor.constraint(equalTo: forwardButton.trailingAnchor, constant: 6),
            reloadButton.centerYAnchor.constraint(equalTo: addressContainer.centerYAnchor),
            bookmarkButton.leadingAnchor.constraint(equalTo: reloadButton.trailingAnchor, constant: 6),
            bookmarkButton.centerYAnchor.constraint(equalTo: addressContainer.centerYAnchor),

            goButton.trailingAnchor.constraint(equalTo: controlsRow.trailingAnchor),
            goButton.centerYAnchor.constraint(equalTo: addressContainer.centerYAnchor),

            addressContainer.leadingAnchor.constraint(equalTo: bookmarkButton.trailingAnchor, constant: 6),
            addressContainer.trailingAnchor.constraint(equalTo: goButton.leadingAnchor, constant: -6),
            addressContainer.topAnchor.constraint(equalTo: controlsRow.topAnchor),
            addressContainer.bottomAnchor.constraint(equalTo: controlsRow.bottomAnchor),
            addressContainer.heightAnchor.constraint(equalToConstant: rowHeight),

            addressField.topAnchor.constraint(equalTo: addressContainer.topAnchor),
            addressField.leadingAnchor.constraint(equalTo: addressContainer.leadingAnchor, constant: 8),
            addressField.trailingAnchor.constraint(equalTo: addressContainer.trailingAnchor, constant: -8),
            addressField.bottomAnchor.constraint(equalTo: addressContainer.bottomAnchor),

            progressView.topAnchor.constraint(equalTo: controlsRow.bottomAnchor, constant: 6),
            progressView.leadingAnchor.constraint(equalTo: toolbarCard.leadingAnchor, constant: inset),
            progressView.trailingAnchor.constraint(equalTo: toolbarCard.trailingAnchor, constant: -inset),
            progressView.heightAnchor.constraint(equalToConstant: 2),

            tabsRow.topAnchor.constraint(equalTo: progressView.bottomAnchor, constant: 6),
            tabsRow.leadingAnchor.constraint(equalTo: toolbarCard.leadingAnchor, constant: inset),
            tabsRow.trailingAnchor.constraint(equalTo: toolbarCard.trailingAnchor, constant: -inset),
            tabsRow.bottomAnchor.constraint(equalTo: toolbarCard.bottomAnchor, constant: -inset),
            tabsRow.heightAnchor.constraint(equalToConstant: rowHeight),

            homeButton.leadingAnchor.constraint(equalTo: tabsRow.leadingAnchor),
            homeButton.centerYAnchor.constraint(equalTo: tabsRow.centerYAnchor),

            tabsScrollView.leadingAnchor.constraint(equalTo: homeButton.trailingAnchor, constant: 6),
            tabsScrollView.topAnchor.constraint(equalTo: tabsRow.topAnchor),
            tabsScrollView.bottomAnchor.constraint(equalTo: tabsRow.bottomAnchor),
            tabsScrollView.trailingAnchor.constraint(equalTo: addTabButton.leadingAnchor, constant: -6),

            tabsStack.topAnchor.constraint(equalTo: tabsScrollView.contentLayoutGuide.topAnchor),
            tabsStack.leadingAnchor.constraint(equalTo: tabsScrollView.contentLayoutGuide.leadingAnchor),
            tabsStack.trailingAnchor.constraint(equalTo: tabsScrollView.contentLayoutGuide.trailingAnchor),
            tabsStack.bottomAnchor.constraint(equalTo: tabsScrollView.contentLayoutGuide.bottomAnchor),
            tabsStack.heightAnchor.constraint(equalTo: tabsScrollView.frameLayoutGuide.heightAnchor),

            addTabButton.centerYAnchor.constraint(equalTo: tabsRow.centerYAnchor),
            closeTabButton.centerYAnchor.constraint(equalTo: tabsRow.centerYAnchor),
            closeTabButton.trailingAnchor.constraint(equalTo: tabsRow.trailingAnchor),
            addTabButton.trailingAnchor.constraint(equalTo: closeTabButton.leadingAnchor, constant: -6),

            statusView.topAnchor.constraint(equalTo: browserCard.topAnchor, constant: inset),
            statusView.leadingAnchor.constraint(equalTo: browserCard.leadingAnchor, constant: inset),
            statusView.trailingAnchor.constraint(equalTo: browserCard.trailingAnchor, constant: -inset),
            statusView.bottomAnchor.constraint(equalTo: browserCard.bottomAnchor, constant: -inset),
            statusLabel.centerXAnchor.constraint(equalTo: statusView.centerXAnchor),
            statusLabel.centerYAnchor.constraint(equalTo: statusView.centerYAnchor),
            statusLabel.leadingAnchor.constraint(greaterThanOrEqualTo: statusView.leadingAnchor, constant: 16),
            statusLabel.trailingAnchor.constraint(lessThanOrEqualTo: statusView.trailingAnchor, constant: -16),
        ])
    }

    // MARK: - Tabs

    private func makeTab() -> WorkspaceBrowserTab {
        let phone = WorkspaceMetrics.usesPhoneLayout
        let webView = WKWebView(frame: .zero, configuration: WKWebViewConfiguration())
        webView.translatesAutoresizingMaskIntoConstraints = false
        webView.navigationDelegate = self
        webView.uiDelegate = self
        webView.allowsBackForwardNavigationGestures = true
        webView.layer.cornerRadius = phone ? 12 : 16
        webView.layer.masksToBounds = true
        webView.scrollView.keyboardDismissMode = .interactive
        // A tab opened after Cmd+= starts at its window's size, not at 100%.
        webView.pageZoom = workspaceTextScale
        webView.accessibilityIdentifier = "workspace.browser.web"
        if isWindowHidden {
            webView.setAllMediaPlaybackSuspended(true)
        }

        let tap = UITapGestureRecognizer(target: self, action: #selector(pageTapped))
        tap.cancelsTouchesInView = false
        tap.delaysTouchesEnded = false
        tap.delegate = self
        webView.addGestureRecognizer(tap)

        let tab = WorkspaceBrowserTab(webView: webView)
        tab.observations = [
            watch(\.estimatedProgress, of: tab),
            watch(\.isLoading, of: tab),
            watch(\.canGoBack, of: tab),
            watch(\.canGoForward, of: tab),
            watch(\.url, of: tab),
        ]
        return tab
    }

    private func watch<Value>(_ keyPath: KeyPath<WKWebView, Value>, of tab: WorkspaceBrowserTab) -> NSKeyValueObservation {
        tab.webView.observe(keyPath) { [weak self, weak tab] _, _ in
            MainActor.assumeIsolated {
                guard let self, let tab else { return }
                self.refreshChrome(ifCurrent: tab)
            }
        }
    }

    /// The page a new tab in this slot opens at: its pin if it has one, or the home page.
    private func addressForNewTab(at index: Int) -> String {
        WorkspaceBrowserStore.pinnedTab(at: index)?.address ?? WorkspaceBrowserStore.homeAddress
    }

    /// With no page yet: the caller says what it shows.
    @discardableResult
    private func addTab(activate: Bool) -> WorkspaceBrowserTab? {
        guard tabs.count < maximumTabCount else { return nil }
        let tab = makeTab()
        tabs.append(tab)
        rebuildTabButtons()
        if activate || tabs.count == 1 {
            selectTab(at: tabs.count - 1)
        } else {
            refreshChrome()
        }
        return tab
    }

    private func selectTab(at index: Int) {
        guard tabs.indices.contains(index) else { return }
        selectedTabIndex = index
        attachCurrentWebView()
        refreshChrome()
    }

    /// Only the selected tab's page is in the view tree, and none while the window is hidden:
    /// WebKit takes a page in a hidden view for a visible one, and a page out of the tree it
    /// knows is hidden, so that's what stops its timers and animation.
    private func attachCurrentWebView() {
        guard let current = currentTab?.webView else { return }
        if let attachedWebView, attachedWebView !== current {
            attachedWebView.removeFromSuperview()
        }
        attachedWebView = current
        if isWindowHidden {
            current.removeFromSuperview()
            return
        }
        guard current.superview !== browserCard else { return }
        current.removeFromSuperview()
        browserCard.insertSubview(current, belowSubview: statusView)
        let inset: CGFloat = WorkspaceMetrics.usesPhoneLayout ? 6 : 8
        NSLayoutConstraint.activate([
            current.topAnchor.constraint(equalTo: browserCard.topAnchor, constant: inset),
            current.leadingAnchor.constraint(equalTo: browserCard.leadingAnchor, constant: inset),
            current.trailingAnchor.constraint(equalTo: browserCard.trailingAnchor, constant: -inset),
            current.bottomAnchor.constraint(equalTo: browserCard.bottomAnchor, constant: -inset),
        ])
    }

    private func rebuildTabButtons() {
        clearArrangedSubviews(of: tabsStack)
        tabButtons = tabs.indices.map(makeTabButton)
        tabButtons.forEach(tabsStack.addArrangedSubview)
    }

    private func makeTabButton(index: Int) -> UIButton {
        let phone = WorkspaceMetrics.usesPhoneLayout
        let button = UIButton(type: .system)
        button.translatesAutoresizingMaskIntoConstraints = false
        button.tag = index
        button.layer.cornerRadius = 10
        button.layer.borderWidth = 1
        button.titleLabel?.font = .systemFont(ofSize: WorkspaceTheme.fontSize(.footnote), weight: .semibold)
        button.titleLabel?.adjustsFontSizeToFitWidth = true
        button.titleLabel?.minimumScaleFactor = 0.6
        button.accessibilityIdentifier = "workspace.browser.tab.\(index)"
        button.addTarget(self, action: #selector(selectTabFromButton(_:)), for: .touchUpInside)
        addLongPress(to: button, action: #selector(tabLongPressed(_:)))
        NSLayoutConstraint.activate([
            button.widthAnchor.constraint(equalToConstant: phone ? 30 : 34),
            button.heightAnchor.constraint(equalToConstant: phone ? 26 : 30),
        ])
        updateTabButtonTitle(button)
        return button
    }

    private func updateTabButtonTitle(_ button: UIButton) {
        let index = button.tag
        if let pinned = WorkspaceBrowserStore.pinnedTab(at: index) {
            button.setTitle("\(index + 1)📌", for: .normal)
            button.accessibilityLabel = "Tab \(index + 1), pinned to \(pinned.title)"
            button.accessibilityHint = "Touch and hold to unpin."
        } else {
            button.setTitle("\(index + 1)", for: .normal)
            button.accessibilityLabel = "Tab \(index + 1)"
            button.accessibilityHint = "Touch and hold to pin this tab's current page."
        }
    }

    @objc private func selectTabFromButton(_ sender: UIButton) {
        selectTab(at: sender.tag)
    }

    @objc private func addTabTapped() {
        maximumTabCount = WorkspaceBrowserLimits.maximumTabCount
        guard tabs.count < maximumTabCount else {
            presentTabLimitAlert()
            return
        }
        if let tab = addTab(activate: true) {
            load(addressForNewTab(at: tabs.count - 1), in: tab, byPerson: false)
        }
    }

    @objc private func closeCurrentTab() {
        guard tabs.count > 1, let tab = currentTab else { return }
        tab.loadGeneration += 1
        tab.observations.removeAll()
        tab.webView.stopLoading()
        tab.webView.navigationDelegate = nil
        tab.webView.uiDelegate = nil
        tab.webView.removeFromSuperview()
        tabs.remove(at: selectedTabIndex)
        rebuildTabButtons()
        selectTab(at: min(selectedTabIndex, tabs.count - 1))
    }

    private func presentTabLimitAlert() {
        let message: String
        if WorkspaceBrowserLimits.hasLowMemory {
            message = "This device is limited to \(maximumTabCount) browser tabs in Workspace because it has less than 4 GB of RAM."
        } else {
            message = "This device is limited to \(maximumTabCount) browser tabs in Workspace based on its available RAM (\(Int(WorkspaceBrowserLimits.physicalGB.rounded(.down))) GB)."
        }
        let alert = UIAlertController(title: "Tab Limit Reached", message: message, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "OK", style: .default))
        presentOwned(alert)
    }

    /// `presentedViewController` also answers for what an ancestor shows, such as another
    /// window's alert, so closing this window remembers which one is its own.
    private func presentOwned(_ controller: UIViewController) {
        ownPresentation = controller
        present(controller, animated: true)
    }

    // MARK: - Workspace

    /// A file from the File Manager, or a path or web address from `ws open`: in a new tab
    /// while the tab limit leaves room, or else in the current one. A Browser with only its
    /// first tab, never navigated, uses that one.
    func workspaceOpenFile(atPath path: String) {
        loadViewIfNeeded()
        let currentUnused = tabs.count == 1 && currentTab?.webView.backForwardList.backList.isEmpty == true
        // The address goes straight in, so a pin on the new tab's slot doesn't win over it.
        if !currentUnused, let tab = addTab(activate: true) {
            load(path, in: tab, byPerson: false)
        } else if let tab = currentTab {
            load(path, in: tab, byPerson: false)
        }
    }

    /// Every tab's address is saved, but only the selected one is reopened: tabs are built
    /// through their own path, and driving it from a restore is separate work. Saving them all
    /// means the rest can come back later without a change of format.
    func workspaceToolStateForSaving() -> [String: Any]? {
        guard !tabs.isEmpty else { return nil }
        let addresses = tabs.map { tab in
            tab.webView.url.map(WorkspaceBrowserAddress.display) ?? tab.requestedAddress ?? ""
        }
        return ["tabs": addresses, "selected": selectedTabIndex]
    }

    func workspaceRestoreToolState(_ state: [String: Any]) {
        guard let addresses = state["tabs"] as? [String], !addresses.isEmpty, let tab = currentTab else { return }
        let saved = (state["selected"] as? NSNumber)?.intValue ?? 0
        let address = addresses[addresses.indices.contains(saved) ? saved : 0]
        if !address.isEmpty {
            load(address, in: tab, byPerson: false)
        }
    }

    /// Takes the keyboard from whatever terminal had it, unless the address field or a page's
    /// own field has it, and finds out if a local page's file has gone while this window was
    /// behind another.
    func workspaceToolDidBecomeFrontmost() {
        if !Self.holdsFirstResponder(view) {
            becomeFirstResponder()
        }
        scheduleVisibilityCheck()
        guard let tab = currentTab, let url = tab.webView.url, url.isFileURL, !tab.webView.isLoading, tab.statusMessage == nil else { return }
        let generation = tab.loadGeneration
        let path = WorkspaceBrowserAddress.display(url)
        WorkspaceFileService.shared.item(path) { [weak self, weak tab] result in
            guard let self, let tab, tab.loadGeneration == generation else { return }
            switch result {
            case .success(let item) where item.kind == .regular:
                break
            case .success:
                self.setStatus(Self.goneMessage(path), in: tab)
            case .failure(let error):
                self.setStatus(self.message(forFileError: error, path: path), in: tab)
            }
        }
    }

    override var canBecomeFirstResponder: Bool { true }

    private static func holdsFirstResponder(_ view: UIView) -> Bool {
        view.isFirstResponder || view.subviews.contains(where: holdsFirstResponder)
    }

    /// The notification comes as focus moves a window, before the windows are shown or hidden,
    /// so this looks on the next turn. Nothing else says a window was hidden: it's a view
    /// property, and an applet's own appearance callbacks don't fire for it.
    private func scheduleVisibilityCheck() {
        DispatchQueue.main.async { [weak self] in
            guard let self, let window = self.workspaceHost?.window(holding: self) else { return }
            self.setWindowHidden(window.isHidden)
        }
    }

    /// On another Desktop a page keeps its place and its network, but plays nothing and runs no
    /// timers. Coming back, what was playing goes on.
    private func setWindowHidden(_ hidden: Bool) {
        guard hidden != isWindowHidden else { return }
        isWindowHidden = hidden
        for tab in tabs {
            tab.webView.setAllMediaPlaybackSuspended(hidden)
        }
        attachCurrentWebView()
    }

    /// The window is closing, and a page's sound must not outlast it while WebKit lets go.
    override func willMove(toParent parent: UIViewController?) {
        super.willMove(toParent: parent)
        guard parent == nil else { return }
        ownPresentation?.dismiss(animated: false)
        for tab in tabs {
            tab.loadGeneration += 1
            tab.webView.stopLoading()
            tab.webView.setAllMediaPlaybackSuspended(true)
        }
    }

    // MARK: - Text scale

    /// Cmd+=, Cmd+- and Cmd+0 zoom the pages, as Safari's do: pageZoom reflows a page, where
    /// pinch zoom only magnifies it, and a page sets its own sizes in CSS so there's nothing to
    /// re-font. Every tab follows, so switching tabs doesn't change the size. The address field
    /// and the buttons are chrome in fixed-size boxes, and stay as they are.
    override func applyTextScale() {
        super.applyTextScale()
        for tab in tabs {
            tab.webView.pageZoom = workspaceTextScale
        }
    }

    // MARK: - Loading

    /// `byPerson` means the person asked for this just now by typing or tapping, which is what
    /// lets an address of another kind go to another app.
    private func load(_ address: String, in tab: WorkspaceBrowserTab, byPerson: Bool) {
        tab.loadGeneration += 1
        tab.statusMessage = nil
        switch WorkspaceBrowserAddress.target(for: address) {
        case .web(let url):
            tab.requestedAddress = WorkspaceBrowserAddress.display(url)
            tab.webView.load(URLRequest(url: url))
            refreshChrome(ifCurrent: tab)
        case .localFile(let path):
            tab.requestedAddress = path
            refreshChrome(ifCurrent: tab)
            openLocalFile(path, in: tab, generation: tab.loadGeneration)
        case .otherScheme(let url):
            if byPerson {
                confirmOpeningElsewhere(url)
            } else {
                tab.requestedAddress = address
                setStatus("Only web pages and local files open here, and “\(url.scheme ?? address)” is neither.", in: tab)
            }
        case .invalid:
            tab.requestedAddress = address
            setStatus("That address can’t be opened here.", in: tab)
        }
    }

    /// Checks the file first: WebKit would wait for ever on a FIFO, say nothing useful about a
    /// folder, and show an empty file as a window that failed to load.
    private func openLocalFile(_ path: String, in tab: WorkspaceBrowserTab, generation: Int) {
        WorkspaceFileService.shared.item(path) { [weak self, weak tab] result in
            guard let self, let tab, tab.loadGeneration == generation else { return }
            switch result {
            case .success(let item) where item.kind == .regular && item.size == 0:
                self.setStatus("This file is empty.", in: tab)
            case .success(let item) where item.kind == .regular:
                self.startLocalLoad(path, in: tab, generation: generation)
            case .success(let item) where item.kind == .directory:
                self.setStatus("“\((path as NSString).lastPathComponent)” is a folder.", in: tab)
            case .success(let item) where item.isBrokenSymlink:
                self.setStatus(Self.goneMessage(path), in: tab)
            case .success:
                self.setStatus(WorkspaceFileError.notRegularFile.localizedDescription, in: tab)
            case .failure(let error):
                self.setStatus(self.message(forFileError: error, path: path), in: tab)
            }
        }
    }

    private func startLocalLoad(_ path: String, in tab: WorkspaceBrowserTab, generation: Int) {
        Self.fileQueue.async { [weak self, weak tab] in
            let file = URL(fileURLWithPath: WorkspaceFileService.hostPath(path), isDirectory: false)
            let scope = WorkspaceBrowserAddress.readScope(for: file)
            DispatchQueue.main.async { [weak self, weak tab] in
                MainActor.assumeIsolated {
                    guard let self, let tab, tab.loadGeneration == generation else { return }
                    if !tab.readScopes.contains(scope) {
                        tab.readScopes.append(scope)
                    }
                    tab.ownLocalLoad = file
                    tab.webView.loadFileURL(file, allowingReadAccessTo: scope)
                    self.refreshChrome(ifCurrent: tab)
                }
            }
        }
    }

    private static func goneMessage(_ path: String) -> String {
        "“\((path as NSString).lastPathComponent)” isn’t there any more. It may have been deleted or moved."
    }

    private func message(forFileError error: Error, path: String) -> String {
        if case WorkspaceFileError.system(ENOENT) = error {
            return Self.goneMessage(path)
        }
        return error.localizedDescription.isEmpty ? "Couldn’t open this file." : error.localizedDescription
    }

    /// nil for what isn't a failure the person needs to hear about.
    private func message(forNavigationError error: Error, in tab: WorkspaceBrowserTab) -> String? {
        let failure = error as NSError
        if failure.domain == NSURLErrorDomain {
            if failure.code == NSURLErrorCancelled {
                return nil
            }
            if failure.code == NSURLErrorFileDoesNotExist, let address = tab.requestedAddress {
                return Self.goneMessage(address)
            }
        }
        if failure.domain == "WebKitErrorDomain" {
            // WebKit's media player took over the load, which it reports this way.
            if failure.code == 204 {
                return nil
            }
            // The response is something a page can't show, such as an archive.
            if failure.code == 102 {
                return "This can’t be shown in the Browser."
            }
        }
        return failure.localizedDescription.isEmpty ? "Couldn’t load this page." : failure.localizedDescription
    }

    private func setStatus(_ message: String?, in tab: WorkspaceBrowserTab) {
        tab.statusMessage = message
        refreshChrome(ifCurrent: tab)
    }

    // MARK: - Chrome

    private func refreshChrome(ifCurrent tab: WorkspaceBrowserTab) {
        if tab === currentTab {
            refreshChrome()
        }
    }

    private func refreshChrome() {
        maximumTabCount = WorkspaceBrowserLimits.maximumTabCount
        let tab = currentTab
        let webView = tab?.webView
        let loading = webView?.isLoading ?? false
        backButton.isEnabled = webView?.canGoBack == true || tab.map(hasPageBehindMessage) == true
        forwardButton.isEnabled = webView?.canGoForward ?? false
        addTabButton.isEnabled = tabs.count < maximumTabCount
        closeTabButton.isEnabled = tabs.count > 1
        for button in [backButton, forwardButton, addTabButton, closeTabButton] {
            button.alpha = button.isEnabled ? 1 : 0.42
        }
        reloadButton.setTitle(loading ? "X" : "R", for: .normal)
        reloadButton.accessibilityLabel = loading ? "Stop loading" : "Reload"
        progressView.isHidden = !loading
        progressView.setProgress(Float(webView?.estimatedProgress ?? 0), animated: loading)
        if !addressField.isFirstResponder {
            addressField.text = tab?.requestedAddress ?? webView?.url.map(WorkspaceBrowserAddress.display) ?? WorkspaceBrowserStore.defaultAddress
        }
        statusLabel.text = tab?.statusMessage
        statusView.isHidden = tab?.statusMessage == nil
        updateBookmarkButtonState()
        tabButtons.forEach(updateTabButtonTitle)
        applyChromeColors()
    }

    // MARK: - Theme

    override func applyTheme() {
        super.applyTheme()
        applyChromeColors()
    }

    private func applyChromeColors() {
        let colors = theme
        for button in actionButtons {
            button.backgroundColor = colors.cardAlt.withAlphaComponent(0.92)
            button.layer.borderColor = colors.stroke.cgColor
            button.setTitleColor(button.isEnabled ? colors.primary : colors.secondary, for: .normal)
        }
        for button in tabButtons {
            let selected = button.tag == selectedTabIndex
            if selected {
                button.accessibilityTraits.insert(.selected)
            } else {
                button.accessibilityTraits.remove(.selected)
            }
            button.backgroundColor = selected ? colors.accent.withAlphaComponent(0.18) : colors.cardAlt.withAlphaComponent(0.92)
            button.layer.borderColor = (selected ? colors.accentAlt : colors.stroke).cgColor
            button.setTitleColor(selected ? colors.accent : colors.primary, for: .normal)
        }
        addressContainer.backgroundColor = colors.backgroundTop.withAlphaComponent(0.18)
        addressContainer.layer.borderColor = colors.stroke.cgColor
        addressField.textColor = colors.primary
        addressField.tintColor = colors.accent
        addressField.attributedPlaceholder = NSAttributedString(string: "Enter URL or search", attributes: [.foregroundColor: colors.secondary])
        progressView.trackTintColor = colors.backgroundTop.withAlphaComponent(0.24)
        progressView.progressTintColor = colors.accent
        // Opaque, so a message stands on its own over whatever page failed behind it.
        statusView.backgroundColor = colors.card
        statusView.layer.cornerRadius = WorkspaceMetrics.usesPhoneLayout ? 12 : 16
        statusLabel.textColor = colors.secondary
    }

    // MARK: - Toolbar

    /// A load that failed leaves the last page standing behind the message.
    private func hasPageBehindMessage(_ tab: WorkspaceBrowserTab) -> Bool {
        tab.statusMessage != nil && tab.webView.url != nil
    }

    @objc private func goBack() {
        guard let tab = currentTab else { return }
        // Back from the message is that page, not the one before it.
        if hasPageBehindMessage(tab) {
            tab.loadGeneration += 1
            tab.requestedAddress = nil
            setStatus(nil, in: tab)
            return
        }
        guard tab.webView.canGoBack else { return }
        tab.loadGeneration += 1
        tab.requestedAddress = nil
        tab.webView.goBack()
    }

    @objc private func goForward() {
        guard let tab = currentTab, tab.webView.canGoForward else { return }
        tab.loadGeneration += 1
        tab.requestedAddress = nil
        tab.webView.goForward()
    }

    @objc private func goHome() {
        if let tab = currentTab {
            load(WorkspaceBrowserStore.homeAddress, in: tab, byPerson: true)
        }
    }

    @objc private func reloadOrStop() {
        guard let tab = currentTab else { return }
        if tab.webView.isLoading {
            tab.webView.stopLoading()
        } else if let address = tab.requestedAddress, tab.statusMessage != nil || tab.webView.url == nil {
            load(address, in: tab, byPerson: false)
        } else {
            tab.webView.reload()
        }
        refreshChrome()
    }

    @objc private func commitAddress() {
        if let tab = currentTab {
            load(addressField.text ?? "", in: tab, byPerson: true)
        }
        addressField.resignFirstResponder()
        refreshChrome()
    }

    func textFieldShouldReturn(_ textField: UITextField) -> Bool {
        commitAddress()
        return true
    }

    private func presentSheet(_ sheet: UIAlertController, from source: UIView) {
        // An iPad presents this as a popover, which must have an anchor or UIKit raises.
        sheet.popoverPresentationController?.sourceView = source
        sheet.popoverPresentationController?.sourceRect = source.bounds
        presentOwned(sheet)
    }

    @objc private func homeLongPressed(_ recognizer: UILongPressGestureRecognizer) {
        guard recognizer.state == .began else { return }
        let alert = UIAlertController(title: "Browser Home", message: "Set the page opened by the Home button.", preferredStyle: .alert)
        alert.addTextField { field in
            field.text = WorkspaceBrowserStore.homeAddress
            field.placeholder = WorkspaceBrowserStore.defaultAddress
            field.clearButtonMode = .whileEditing
            field.autocapitalizationType = .none
            field.autocorrectionType = .no
            field.spellCheckingType = .no
            field.smartDashesType = .no
            field.smartQuotesType = .no
            field.smartInsertDeleteType = .no
            field.accessibilityIdentifier = "workspace.browser.homeField"
        }
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        alert.addAction(UIAlertAction(title: "Use Default", style: .default) { [weak self] _ in
            WorkspaceBrowserStore.setHomeAddress(nil)
            self?.goHome()
        })
        alert.addAction(UIAlertAction(title: "Save", style: .default) { [weak self, weak alert] _ in
            WorkspaceBrowserStore.setHomeAddress(alert?.textFields?.first?.text)
            self?.goHome()
        })
        presentOwned(alert)
    }

    @objc private func reloadLongPressed(_ recognizer: UILongPressGestureRecognizer) {
        guard recognizer.state == .began, let url = currentTab?.webView.url, url.scheme?.lowercased().hasPrefix("http") == true else { return }
        let sheet = UIAlertController(title: nil, message: url.absoluteString, preferredStyle: .actionSheet)
        sheet.addAction(UIAlertAction(title: "Open in Safari", style: .default) { _ in
            UIApplication.shared.open(url)
        })
        sheet.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        presentSheet(sheet, from: reloadButton)
    }

    // MARK: - Bookmarks

    private var currentPageAddress: String? {
        currentTab?.webView.url.map(WorkspaceBrowserAddress.display)
    }

    @objc private func toggleBookmark() {
        guard let address = currentPageAddress, !address.isEmpty else { return }
        if WorkspaceBrowserStore.isBookmarked(address) {
            WorkspaceBrowserStore.removeBookmark(address: address)
        } else {
            WorkspaceBrowserStore.addBookmark(title: currentTab?.webView.title, address: address)
        }
        updateBookmarkButtonState()
    }

    /// The label "Bookmark" overrides the star, so VoiceOver never speaks the glyph. The
    /// selected trait announces when set and says nothing when not, so a bookmarked page is
    /// carried by the trait and any other by a value.
    private func updateBookmarkButtonState() {
        let bookmarked = WorkspaceBrowserStore.isBookmarked(currentPageAddress)
        bookmarkButton.setTitle(bookmarked ? "★" : "☆", for: .normal)
        if bookmarked {
            bookmarkButton.accessibilityTraits.insert(.selected)
            bookmarkButton.accessibilityValue = nil
        } else {
            bookmarkButton.accessibilityTraits.remove(.selected)
            bookmarkButton.accessibilityValue = "Not bookmarked"
        }
    }

    @objc private func bookmarkLongPressed(_ recognizer: UILongPressGestureRecognizer) {
        if recognizer.state == .began {
            presentBookmarks(editing: false)
        }
    }

    private func presentBookmarks(editing: Bool) {
        let bookmarks = WorkspaceBrowserStore.bookmarks
        let message = bookmarks.isEmpty ? "No bookmarks yet. Tap the star to bookmark the current page." : (editing ? "Tap a bookmark to remove it." : nil)
        let sheet = UIAlertController(title: "Bookmarks", message: message, preferredStyle: .actionSheet)
        for bookmark in bookmarks {
            guard let address = bookmark["url"] else { continue }
            let title = bookmark["title"].flatMap { $0.isEmpty ? nil : $0 } ?? address
            sheet.addAction(UIAlertAction(title: title, style: editing ? .destructive : .default) { [weak self] _ in
                guard let self else { return }
                if editing {
                    WorkspaceBrowserStore.removeBookmark(address: address)
                    self.updateBookmarkButtonState()
                    // Presenting from inside this handler races the sheet's own dismissal,
                    // and silently does nothing when it loses.
                    DispatchQueue.main.async { [weak self] in
                        self?.presentBookmarks(editing: true)
                    }
                } else if let tab = self.currentTab {
                    self.load(address, in: tab, byPerson: true)
                }
            })
        }
        if !bookmarks.isEmpty {
            sheet.addAction(UIAlertAction(title: editing ? "Done" : "Edit Bookmarks…", style: .default) { [weak self] _ in
                DispatchQueue.main.async { [weak self] in
                    self?.presentBookmarks(editing: !editing)
                }
            })
        }
        sheet.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        presentSheet(sheet, from: bookmarkButton)
    }

    // MARK: - Pinned tabs

    @objc private func tabLongPressed(_ recognizer: UILongPressGestureRecognizer) {
        guard recognizer.state == .began, let button = recognizer.view as? UIButton, tabs.indices.contains(button.tag) else { return }
        let index = button.tag
        let webView = tabs[index].webView
        let pinned = WorkspaceBrowserStore.pinnedTab(at: index)
        let message = pinned.map { "Pinned to \($0.address). New tabs opened in this slot load that page instead of Home." }
            ?? "Pin this tab so new tabs opened in this slot load its current page instead of Home."
        let sheet = UIAlertController(title: "Tab \(index + 1)", message: message, preferredStyle: .actionSheet)
        if pinned != nil {
            sheet.addAction(UIAlertAction(title: "Unpin Tab", style: .destructive) { [weak self] _ in
                WorkspaceBrowserStore.unpinTab(at: index)
                self?.refreshChrome()
            })
        } else if let address = webView.url.map(WorkspaceBrowserAddress.display), !address.isEmpty {
            let title = webView.title
            sheet.addAction(UIAlertAction(title: "Pin Tab to Current Page", style: .default) { [weak self] _ in
                WorkspaceBrowserStore.pinTab(at: index, title: title, address: address)
                self?.refreshChrome()
            })
        }
        sheet.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        presentSheet(sheet, from: button)
    }

    // MARK: - Other schemes

    @objc private func pageTapped() {
        lastPageTap = CACurrentMediaTime()
    }

    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool {
        true
    }

    /// A page asked to go to an address of another kind. A script can click a link but can't
    /// touch the screen, so this wants both a link and a tap, spends the tap, and then still
    /// asks: a script waiting for any tap at all gets a question rather than another app.
    private func handleOtherScheme(_ url: URL, navigationType: WKNavigationType) {
        let tapped = CACurrentMediaTime() - lastPageTap < Self.tapWindow
        guard navigationType == .linkActivated, tapped, !isWindowHidden else {
            refusedURL = url
            return
        }
        lastPageTap = 0
        confirmOpeningElsewhere(url)
    }

    private func confirmOpeningElsewhere(_ url: URL) {
        guard presentedViewController == nil else { return }
        let alert = UIAlertController(title: "Open in Another App?", message: url.absoluteString, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel) { [weak self] _ in
            self?.externalPromptURL = nil
        })
        alert.addAction(UIAlertAction(title: "Open", style: .default) { [weak self] _ in
            self?.externalPromptURL = nil
            UIApplication.shared.open(url, options: [:]) { [weak self] opened in
                guard !opened, let self, let tab = self.currentTab else { return }
                self.setStatus("No app here opens “\(url.scheme ?? url.absoluteString)” addresses.", in: tab)
            }
        })
        externalPromptURL = url
        presentOwned(alert)
    }

    // MARK: - WKNavigationDelegate

    /// Only http, https and local files become pages. A frame a page builds for itself can be
    /// empty, or come from `data:` or `blob:`, which as a whole page can pose as anything.
    func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction, decisionHandler: @escaping @MainActor @Sendable (WKNavigationActionPolicy) -> Void) {
        guard let url = navigationAction.request.url, let scheme = url.scheme?.lowercased() else {
            decisionHandler(.cancel)
            return
        }
        let mainFrame = navigationAction.targetFrame?.isMainFrame ?? true
        switch scheme {
        case "http", "https":
            decisionHandler(.allow)
        case "about":
            let empty = url.absoluteString.hasPrefix("about:blank") || url.absoluteString.hasPrefix("about:srcdoc")
            decisionHandler(empty ? .allow : .cancel)
        case "data", "blob":
            decisionHandler(mainFrame ? .cancel : .allow)
        case "file":
            decisionHandler(allowsLocalNavigation(to: url, navigationAction, in: webView, mainFrame: mainFrame) ? .allow : .cancel)
        default:
            decisionHandler(.cancel)
            handleOtherScheme(url, navigationType: navigationAction.navigationType)
        }
    }

    /// A load the applet asked for, history in this tab, or a local page's link or frame that
    /// stays inside what the person opened. A web page never reaches a local file.
    private func allowsLocalNavigation(to url: URL, _ action: WKNavigationAction, in webView: WKWebView, mainFrame: Bool) -> Bool {
        guard let tab = tab(for: webView) else { return false }
        if let own = tab.ownLocalLoad, WorkspaceBrowserAddress.url(url, isInside: own) {
            tab.ownLocalLoad = nil
            return true
        }
        let inScope = tab.readScope(containing: url) != nil
        if inScope {
            if action.navigationType == .backForward || action.navigationType == .reload || webView.url?.isFileURL == true {
                return true
            }
        } else if mainFrame && webView.url?.isFileURL == true {
            setStatus("This page can’t open “\(url.lastPathComponent)”, since it’s outside the folder that was opened. Open it from the File Manager.", in: tab)
        }
        return false
    }

    func webView(_ webView: WKWebView, didStartProvisionalNavigation navigation: WKNavigation!) {
        guard let tab = tab(for: webView) else { return }
        tab.statusMessage = nil
        if let url = webView.url {
            tab.requestedAddress = WorkspaceBrowserAddress.display(url)
        }
        refreshChrome(ifCurrent: tab)
    }

    /// The zoom belongs to the web view, and WebKit carries it from page to page. Setting it
    /// again as each page commits is a guard, in any tab, should some path bring a page up at
    /// the default.
    func webView(_ webView: WKWebView, didCommit navigation: WKNavigation!) {
        webView.pageZoom = workspaceTextScale
        guard let tab = tab(for: webView) else { return }
        tab.requestedAddress = nil
        refreshChrome(ifCurrent: tab)
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        if let tab = tab(for: webView) {
            refreshChrome(ifCurrent: tab)
        }
    }

    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
        if let tab = tab(for: webView) {
            refreshChrome(ifCurrent: tab)
        }
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
        guard let tab = tab(for: webView) else { return }
        if let message = message(forNavigationError: error, in: tab) {
            setStatus(message, in: tab)
        } else {
            refreshChrome(ifCurrent: tab)
        }
    }

    /// The system killed the page's process, usually for memory. Reloading by itself could go
    /// round and round, so this says what happened and leaves Reload to the person.
    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
        if let tab = tab(for: webView) {
            setStatus("This page stopped working. Reload to try again.", in: tab)
        }
    }

    // MARK: - WKUIDelegate

    /// A link or a script asking for a new window opens in this tab instead.
    func webView(_ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration, for navigationAction: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
        guard navigationAction.targetFrame?.isMainFrame != true, let url = navigationAction.request.url, let scheme = url.scheme?.lowercased() else { return nil }
        switch scheme {
        case "http", "https":
            webView.load(navigationAction.request)
        case "file":
            if webView.url?.isFileURL == true, let tab = tab(for: webView), let scope = tab.readScope(containing: url) {
                tab.ownLocalLoad = url
                webView.loadFileURL(url, allowingReadAccessTo: scope)
            }
        case "about", "data", "blob", "javascript":
            break
        default:
            handleOtherScheme(url, navigationType: navigationAction.navigationType)
        }
        return nil
    }
}
